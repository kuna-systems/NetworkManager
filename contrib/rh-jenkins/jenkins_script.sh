# Path for dependencies installed locally
#export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig/

get_timestamp() {
    date --utc '+%Y%m%d-%H%M%S'
}
log_timestamp() {
    printf "%s%s %s -- %s\n" ">>" ">>" "$(get_timestamp)" "$*"
}
DATE="`get_timestamp`"
REPO=ssh://Jenkins-nm-user/var/lib/git/NetworkManager.git

MAKE_JOBS="-j $((3 * $(grep -c ^processor /proc/cpuinfo || echo 1)))"

git_notes() {
    if [[ "$GIT_NOTES_DISABLED" == true ]]; then
        return 0
    fi

    git fetch "$REPO" +refs/notes/test:refs/notes/test || git update-ref -d refs/notes/test

    # git-notes append adds a newline so merge them by hand...
    NOTE="$(git notes --ref=test show HEAD 2>/dev/null || true)"
    if [[ "x$NOTE" != "x" ]]; then
        newline='
'
        if [[ "${NOTE#"${NOTE%?}"}" != "$newline" ]]; then
            NOTE="$NOTE$newline"
        fi
    fi

    git notes --ref test add -f -m "$NOTE$1" HEAD
    git push "$REPO" refs/notes/test:refs/notes/test
}

git_notes_ok() {
    git_notes "Tested: OK   $DATE $BUILD_URL"
}
git_notes_fail() {
    git_notes "Tested: FAIL $DATE $BUILD_URL"
}

trap "git_notes_fail; exit 1" ERR



temporary_workaround_01() {
    # https://bugzilla.gnome.org/show_bug.cgi?id=705160
    # otherwise current mem leaks check fail...
    if [[ "$(git merge-base 2540966492340ad87cd5a894d544580b8e20c558 HEAD 2>/dev/null || true)" != "2540966492340ad87cd5a894d544580b8e20c558" ]]; then
        wget 'https://bugzilla.gnome.org/attachment.cgi?id=256245' -O valgrind.suppressions.patch
        git apply valgrind.suppressions.patch || true
    fi
}

clean_all() {
    git reset --hard HEAD
    git clean -fdx
    git submodule foreach git reset --hard HEAD
    git submodule foreach git clean -fdx
    git submodule update

    temporary_workaround_01
}


if [[ "$OUT_OF_TREE_BUILD" == true ]]; then
    log_timestamp "Starting out of tree build"
    clean_all

    ./autogen.sh
    make distclean

    mkdir _build
    pushd _build
        ../configure --enable-maintainer-mode --prefix=$PWD/.INSTALL/ --with-dhclient=yes --with-dhcpcd=yes --with-crypto=nss --enable-more-warnings=error --enable-ppp=yes --enable-polkit=yes --with-session-tracking=systemd --with-suspend-resume=systemd --with-tests=yes --enable-tests=yes --with-valgrind=yes --enable-ifcfg-rh=yes --enable-ifupdown=yes --enable-ifnet=yes --enable-gtk-doc --enable-qt=yes --with-system-libndp=no --enable-static=libndp --enable-bluez4=no --enable-wimax=no --enable-vala=no --enable-modify-system=no
        make $MAKE_JOBS
    popd

    log_timestamp "Finished out of tree build"
fi

log_timestamp "start build"
clean_all

log_timestamp
./autogen.sh --enable-maintainer-mode --prefix=$PWD/.INSTALL/ --with-dhclient=yes --with-dhcpcd=yes --with-crypto=nss --enable-more-warnings=error --enable-ppp=yes --enable-polkit=yes --with-session-tracking=systemd --with-suspend-resume=systemd --with-tests=yes --enable-tests=yes --with-valgrind=yes --enable-ifcfg-rh=yes --enable-ifupdown=yes --enable-ifnet=yes --enable-gtk-doc --enable-qt=yes --with-system-libndp=no --enable-static=libndp --enable-bluez4=no --enable-wimax=no --enable-vala=no --enable-modify-system=no

log_timestamp
make $MAKE_JOBS

log_timestamp
make check

log_timestamp
make distcheck


if [[ "$RPM" == true ]]; then
    log_timestamp "start making RPM"
    wget http://file.brq.redhat.com/~thaller/nmtui-0.0.1.tar.xz
    git checkout origin/automation -- :/contrib/
    ./contrib/rpm/build.sh
    log_timestamp "finished making RPM"
fi


log_timestamp "finished with success"
git_notes_ok
