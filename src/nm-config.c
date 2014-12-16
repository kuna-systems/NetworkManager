/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2013 Thomas Bechtold <thomasbechtold@jpberlin.de>
 */

#include "config.h"

#include <string.h>
#include <stdio.h>

#include "nm-config.h"
#include "nm-logging.h"
#include "nm-utils.h"
#include "nm-glib-compat.h"
#include "nm-device.h"
#include "NetworkManagerUtils.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#define NM_DEFAULT_SYSTEM_CONF_FILE    NMCONFDIR "/NetworkManager.conf"
#define NM_DEFAULT_SYSTEM_CONF_DIR     NMCONFDIR "/conf.d"
#define NM_OLD_SYSTEM_CONF_FILE        NMCONFDIR "/nm-system-settings.conf"
#define NM_NO_AUTO_DEFAULT_STATE_FILE  NMSTATEDIR "/no-auto-default.state"

struct NMConfigCmdLineOptions {
	char *config_main_file;
	char *config_dir;
	char *no_auto_default_file;
	char *plugins;
	char *connectivity_uri;

	/* We store interval as signed internally to track whether it's
	 * set or not via GOptionEntry
	 */
	int connectivity_interval;
	char *connectivity_response;
};

typedef struct {
	NMConfigCmdLineOptions cli;

	NMConfigData *config_data;
	NMConfigData *config_data_orig;

	char *config_main_file;
	char *config_dir;
	char *config_description;
	char *no_auto_default_file;
	GKeyFile *keyfile;

	char **plugins;
	gboolean monitor_connection_files;
	gboolean auth_polkit;
	char *dhcp_client;
	char *dns_mode;

	char *log_level;
	char *log_domains;

	char *debug;

	char **no_auto_default;
	char **ignore_carrier;

	gboolean configure_and_quit;
} NMConfigPrivate;

enum {
	SIGNAL_CONFIG_CHANGED,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (NMConfig, nm_config, G_TYPE_OBJECT)

#define NM_CONFIG_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_CONFIG, NMConfigPrivate))

/************************************************************************/

static gboolean
_get_bool_value (GKeyFile *keyfile,
                 const char *section,
                 const char *key,
                 gboolean default_value)
{
	gboolean value = default_value;
	char *str;

	g_return_val_if_fail (keyfile != NULL, default_value);
	g_return_val_if_fail (section != NULL, default_value);
	g_return_val_if_fail (key != NULL, default_value);

	str = g_key_file_get_value (keyfile, section, key, NULL);
	if (!str)
		return default_value;

	g_strstrip (str);
	if (str[0]) {
		if (!g_ascii_strcasecmp (str, "true") || !g_ascii_strcasecmp (str, "yes") || !g_ascii_strcasecmp (str, "on") || !g_ascii_strcasecmp (str, "1"))
			value = TRUE;
		else if (!g_ascii_strcasecmp (str, "false") || !g_ascii_strcasecmp (str, "no") || !g_ascii_strcasecmp (str, "off") || !g_ascii_strcasecmp (str, "0"))
			value = FALSE;
		else {
			nm_log_warn (LOGD_CORE, "Unrecognized value for %s.%s: '%s'. Assuming '%s'",
			             section, key, str, default_value ? "true" : "false");
		}
	}

	g_free (str);
	return value;
}

/************************************************************************/

NMConfigData *
nm_config_get_data (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->config_data;
}

/* The NMConfigData instance is reloadable and will be swapped on reload.
 * nm_config_get_data_orig() returns the original configuration, when the NMConfig
 * instance was created. */
NMConfigData *
nm_config_get_data_orig (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->config_data_orig;
}

const char *
nm_config_get_config_main_file (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->config_main_file;
}

const char *
nm_config_get_config_description (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->config_description;
}

const char **
nm_config_get_plugins (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return (const char **) NM_CONFIG_GET_PRIVATE (config)->plugins;
}

gboolean
nm_config_get_monitor_connection_files (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, FALSE);

	return NM_CONFIG_GET_PRIVATE (config)->monitor_connection_files;
}

gboolean
nm_config_get_auth_polkit (NMConfig *config)
{
	g_return_val_if_fail (NM_IS_CONFIG (config), NM_CONFIG_DEFAULT_AUTH_POLKIT);

	return NM_CONFIG_GET_PRIVATE (config)->auth_polkit;
}

const char *
nm_config_get_dhcp_client (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->dhcp_client;
}

const char *
nm_config_get_dns_mode (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->dns_mode;
}

const char *
nm_config_get_log_level (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->log_level;
}

const char *
nm_config_get_log_domains (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->log_domains;
}

const char *
nm_config_get_debug (NMConfig *config)
{
	g_return_val_if_fail (config != NULL, NULL);

	return NM_CONFIG_GET_PRIVATE (config)->debug;
}

gboolean
nm_config_get_configure_and_quit (NMConfig *config)
{
	return NM_CONFIG_GET_PRIVATE (config)->configure_and_quit;
}

char *
nm_config_get_value (NMConfig *config, const char *group, const char *key, GError **error)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);

	return g_key_file_get_string (priv->keyfile, group, key, error);
}

gboolean
nm_config_get_ignore_carrier (NMConfig *config, NMDevice *device)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);
	GSList *specs = NULL;
	int i;
	gboolean match;

	if (!priv->ignore_carrier)
		return FALSE;

	for (i = 0; priv->ignore_carrier[i]; i++)
		specs = g_slist_prepend (specs, priv->ignore_carrier[i]);

	match = nm_device_spec_match_list (device, specs);

	g_slist_free (specs);
	return match;
}

/************************************************************************/

static void
merge_no_auto_default_state (NMConfig *config)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);
	GPtrArray *updated;
	char **list;
	int i, j;
	char *data;

	/* If the config already matches everything, we don't need to do anything else. */
	if (priv->no_auto_default && !g_strcmp0 (priv->no_auto_default[0], "*"))
		return;

	updated = g_ptr_array_new ();
	if (priv->no_auto_default) {
		for (i = 0; priv->no_auto_default[i]; i++)
			g_ptr_array_add (updated, priv->no_auto_default[i]);
		g_free (priv->no_auto_default);
	}

	if (g_file_get_contents (priv->no_auto_default_file, &data, NULL, NULL)) {
		list = g_strsplit (data, "\n", -1);
		for (i = 0; list[i]; i++) {
			if (!*list[i])
				g_free (list[i]);
			else {
				for (j = 0; j < updated->len; j++) {
					if (!strcmp (list[i], updated->pdata[j]))
						break;
				}
				if (j == updated->len)
					g_ptr_array_add (updated, list[i]);
				else
					g_free (list[i]);
			}
		}
		g_free (list);
		g_free (data);
	}

	g_ptr_array_add (updated, NULL);
	priv->no_auto_default = (char **) g_ptr_array_free (updated, FALSE);
}

gboolean
nm_config_get_ethernet_can_auto_default (NMConfig *config, NMDevice *device)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);
	GSList *specs = NULL;
	int i;
	gboolean match;

	for (i = 0; priv->no_auto_default[i]; i++)
		specs = g_slist_prepend (specs, priv->no_auto_default[i]);

	match = nm_device_spec_match_list (device, specs);

	g_slist_free (specs);
	return !match;
}

void
nm_config_set_ethernet_no_auto_default (NMConfig *config, NMDevice *device)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);
	char *current;
	GString *updated;
	GError *error = NULL;

	if (!nm_config_get_ethernet_can_auto_default (config, device))
		return;

	updated = g_string_new (NULL);
	if (g_file_get_contents (priv->no_auto_default_file, &current, NULL, NULL)) {
		g_string_append (updated, current);
		g_free (current);
		if (updated->str[updated->len - 1] != '\n')
			g_string_append_c (updated, '\n');
	}

	g_string_append (updated, nm_device_get_hw_address (device));
	g_string_append_c (updated, '\n');

	if (!g_file_set_contents (priv->no_auto_default_file, updated->str, updated->len, &error)) {
		nm_log_warn (LOGD_SETTINGS, "Could not update no-auto-default.state file: %s",
		             error->message);
		g_error_free (error);
	}

	g_string_free (updated, TRUE);

	merge_no_auto_default_state (config);
}

/************************************************************************/

static void
_nm_config_cmd_line_options_clear (NMConfigCmdLineOptions *cli)
{
	g_clear_pointer (&cli->config_main_file, g_free);
	g_clear_pointer (&cli->config_dir, g_free);
	g_clear_pointer (&cli->no_auto_default_file, g_free);
	g_clear_pointer (&cli->plugins, g_free);
	g_clear_pointer (&cli->connectivity_uri, g_free);
	g_clear_pointer (&cli->connectivity_response, g_free);
	cli->connectivity_interval = -1;
}

static void
_nm_config_cmd_line_options_copy (const NMConfigCmdLineOptions *cli, NMConfigCmdLineOptions *dst)
{
	g_return_if_fail (cli);
	g_return_if_fail (dst);
	g_return_if_fail (cli != dst);

	_nm_config_cmd_line_options_clear (dst);
	dst->config_dir = g_strdup (cli->config_dir);
	dst->config_main_file = g_strdup (cli->config_main_file);
	dst->no_auto_default_file = g_strdup (cli->no_auto_default_file);
	dst->plugins = g_strdup (cli->plugins);
	dst->connectivity_uri = g_strdup (cli->connectivity_uri);
	dst->connectivity_response = g_strdup (cli->connectivity_response);
	dst->connectivity_interval = cli->connectivity_interval;
}

NMConfigCmdLineOptions *
nm_config_cmd_line_options_new ()
{
	NMConfigCmdLineOptions *cli = g_new0 (NMConfigCmdLineOptions, 1);

	_nm_config_cmd_line_options_clear (cli);
	return cli;
}

void
nm_config_cmd_line_options_free (NMConfigCmdLineOptions *cli)
{
	g_return_if_fail (cli);

	_nm_config_cmd_line_options_clear (cli);
	g_free (cli);
}

void
nm_config_cmd_line_options_add_to_entries (NMConfigCmdLineOptions *cli,
                                           GOptionContext *opt_ctx)
{
	g_return_if_fail (opt_ctx);
	g_return_if_fail (cli);

	{
		GOptionEntry config_options[] = {
			{ "config", 0, 0, G_OPTION_ARG_FILENAME, &cli->config_main_file, N_("Config file location"), N_("/path/to/config.file") },
			{ "config-dir", 0, 0, G_OPTION_ARG_FILENAME, &cli->config_dir, N_("Config directory location"), N_("/path/to/config/dir") },
			{ "no-auto-default", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &cli->no_auto_default_file, "no-auto-default.state location", NULL },
			{ "plugins", 0, 0, G_OPTION_ARG_STRING, &cli->plugins, N_("List of plugins separated by ','"), N_("plugin1,plugin2") },

				/* These three are hidden for now, and should eventually just go away. */
			{ "connectivity-uri", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &cli->connectivity_uri, N_("An http(s) address for checking internet connectivity"), "http://example.com" },
			{ "connectivity-interval", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &cli->connectivity_interval, N_("The interval between connectivity checks (in seconds)"), "60" },
			{ "connectivity-response", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &cli->connectivity_response, N_("The expected start of the response"), N_("Bingo!") },
			{ 0 },
		};

		g_option_context_add_main_entries (opt_ctx, config_options, NULL);
	}
}

/************************************************************************/

GKeyFile *
nm_config_create_keyfile ()
{
	GKeyFile *keyfile;

	keyfile = g_key_file_new ();
	g_key_file_set_list_separator (keyfile, ',');
	return keyfile;
}

static gboolean
read_config (GKeyFile *keyfile, const char *path, GError **error)
{
	GKeyFile *kf;
	char **groups, **keys;
	gsize ngroups, nkeys;
	int g, k;

	g_return_val_if_fail (keyfile, FALSE);
	g_return_val_if_fail (path, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	if (g_file_test (path, G_FILE_TEST_EXISTS) == FALSE) {
		g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND, "file %s not found", path);
		return FALSE;
	}

	nm_log_dbg (LOGD_SETTINGS, "Reading config file '%s'", path);

	kf = nm_config_create_keyfile ();
	if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, error)) {
		g_key_file_free (kf);
		return FALSE;
	}

	/* Override the current settings with the new ones */
	groups = g_key_file_get_groups (kf, &ngroups);
	for (g = 0; groups[g]; g++) {
		keys = g_key_file_get_keys (kf, groups[g], &nkeys, NULL);
		if (!keys)
			continue;
		for (k = 0; keys[k]; k++) {
			int len = strlen (keys[k]);
			char *v;

			if (keys[k][len - 1] == '+') {
				char *base_key = g_strndup (keys[k], len - 1);
				const char *old_val = g_key_file_get_value (keyfile, groups[g], base_key, NULL);
				const char *new_val = g_key_file_get_value (kf, groups[g], keys[k], NULL);

				if (old_val && *old_val) {
					char *combined = g_strconcat (old_val, ",", new_val, NULL);

					g_key_file_set_value (keyfile, groups[g], base_key, combined);
					g_free (combined);
				} else
					g_key_file_set_value (keyfile, groups[g], base_key, new_val);

				g_free (base_key);
				continue;
			}

			g_key_file_set_value (keyfile, groups[g], keys[k],
			                      v = g_key_file_get_value (kf, groups[g], keys[k], NULL));
			g_free (v);
		}
		g_strfreev (keys);
	}
	g_strfreev (groups);
	g_key_file_free (kf);

	return TRUE;
}

static gboolean
read_base_config (GKeyFile *keyfile,
                  const char *cli_config_main_file,
                  char **out_config_main_file,
                  GError **error)
{
	GError *my_error = NULL;

	g_return_val_if_fail (keyfile, FALSE);
	g_return_val_if_fail (out_config_main_file && !*out_config_main_file, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* Try a user-specified config file first */
	if (cli_config_main_file) {
		/* Bad user-specific config file path is a hard error */
		if (read_config (keyfile, cli_config_main_file, error)) {
			*out_config_main_file = g_strdup (cli_config_main_file);
			return TRUE;
		} else
			return FALSE;
	}

	/* Even though we prefer NetworkManager.conf, we need to check the
	 * old nm-system-settings.conf first to preserve compat with older
	 * setups.  In package managed systems dropping a NetworkManager.conf
	 * onto the system would make NM use it instead of nm-system-settings.conf,
	 * changing behavior during an upgrade.  We don't want that.
	 */

	/* Try deprecated nm-system-settings.conf first */
	if (read_config (keyfile, NM_OLD_SYSTEM_CONF_FILE, &my_error)) {
		*out_config_main_file = g_strdup (NM_OLD_SYSTEM_CONF_FILE);
		return TRUE;
	}

	if (!g_error_matches (my_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND)) {
		nm_log_warn (LOGD_CORE, "Old default config file %s invalid: %s\n",
		             NM_OLD_SYSTEM_CONF_FILE,
		             my_error->message);
	}
	g_clear_error (&my_error);

	/* Try the standard config file location next */
	if (read_config (keyfile, NM_DEFAULT_SYSTEM_CONF_FILE, &my_error)) {
		*out_config_main_file = g_strdup (NM_DEFAULT_SYSTEM_CONF_FILE);
		return TRUE;
	}

	if (!g_error_matches (my_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND)) {
		nm_log_warn (LOGD_CORE, "Default config file %s invalid: %s\n",
		             NM_DEFAULT_SYSTEM_CONF_FILE,
		             my_error->message);
		g_propagate_error (error, my_error);
		return FALSE;
	}
	g_clear_error (&my_error);

	/* If for some reason no config file exists, use the default
	 * config file path.
	 */
	*out_config_main_file = g_strdup (NM_DEFAULT_SYSTEM_CONF_FILE);
	nm_log_info (LOGD_CORE, "No config file found or given; using %s\n",
	             NM_DEFAULT_SYSTEM_CONF_FILE);
	return TRUE;
}

static int
sort_asciibetically (gconstpointer a, gconstpointer b)
{
	const char *s1 = *(const char **)a;
	const char *s2 = *(const char **)b;

	return strcmp (s1, s2);
}

static GPtrArray *
_get_config_dir_files (const char *config_main_file,
                       const char *config_dir,
                       char **out_config_description)
{
	GFile *dir;
	GFileEnumerator *direnum;
	GFileInfo *info;
	GPtrArray *confs;
	GString *config_description;
	const char *name;

	g_return_val_if_fail (config_main_file, NULL);
	g_return_val_if_fail (config_dir, NULL);
	g_return_val_if_fail (out_config_description && !*out_config_description, NULL);

	confs = g_ptr_array_new_with_free_func (g_free);
	config_description = g_string_new (config_main_file);
	dir = g_file_new_for_path (config_dir);
	direnum = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME, 0, NULL, NULL);
	if (direnum) {
		while ((info = g_file_enumerator_next_file (direnum, NULL, NULL))) {
			name = g_file_info_get_name (info);
			if (g_str_has_suffix (name, ".conf")) {
				g_ptr_array_add (confs, g_build_filename (config_dir, name, NULL));
				if (confs->len == 1)
					g_string_append (config_description, " and conf.d: ");
				else
					g_string_append (config_description, ", ");
				g_string_append (config_description, name);
			}
			g_object_unref (info);
		}
		g_object_unref (direnum);
	}
	g_object_unref (dir);

	g_ptr_array_sort (confs, sort_asciibetically);

	*out_config_description = g_string_free (config_description, FALSE);
	return confs;
}

static GKeyFile *
read_entire_config (const NMConfigCmdLineOptions *cli,
                    const char *config_dir,
                    char **out_config_main_file,
                    char **out_config_description,
                    GError **error)
{
	GKeyFile *keyfile = nm_config_create_keyfile ();
	GPtrArray *confs;
	guint i;
	char *o_config_main_file = NULL;
	char *o_config_description = NULL;

	g_return_val_if_fail (config_dir, NULL);
	g_return_val_if_fail (out_config_main_file && !*out_config_main_file, FALSE);
	g_return_val_if_fail (out_config_description && !*out_config_description, NULL);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* First read the base config file */
	if (   cli
	    && !read_base_config (keyfile, cli->config_main_file, &o_config_main_file, error)) {
		g_key_file_free (keyfile);
		return NULL;
	}

	g_assert (o_config_main_file);

	confs = _get_config_dir_files (o_config_main_file, config_dir, &o_config_description);
	for (i = 0; i < confs->len; i++) {
		if (!read_config (keyfile, confs->pdata[i], error)) {
			g_key_file_free (keyfile);
			keyfile = NULL;
			break;
		}
	}
	g_ptr_array_unref (confs);

	if (keyfile) {
		*out_config_main_file = o_config_main_file;
		*out_config_description = o_config_description;
	} else {
		g_free (o_config_main_file);
		g_free (o_config_description);
	}
	return keyfile;
}

/************************************************************************/

void
nm_config_reload (NMConfig *self)
{
	NMConfigPrivate *priv;
	NMConfig *new;
	GError *error = NULL;
	GHashTable *changes;
	NMConfigData *old_data;
	NMConfigData *new_data = NULL;

	g_return_if_fail (NM_IS_CONFIG (self));

	priv = NM_CONFIG_GET_PRIVATE (self);

	/* pass on the original command line options. This means, that
	 * options specified at command line cannot ever be reloaded from
	 * file. That seems desirable.
	 */
	new = nm_config_new (&priv->cli, &error);

	if (!new) {
		nm_log_err (LOGD_CORE, "Failed to reload the configuration: %s", error->message);
		g_clear_error (&error);
		return;
	}

	old_data = priv->config_data;
	new_data = nm_config_get_data (new);

	changes = g_hash_table_new (g_str_hash, g_str_equal);

	/* reloading configuration means we have to carefully check every single option
	 * that we want to support and take specific actions. */

	if (   nm_config_data_get_connectivity_interval (old_data) != nm_config_data_get_connectivity_interval (new_data)
	    || g_strcmp0 (nm_config_data_get_connectivity_uri (old_data), nm_config_data_get_connectivity_uri (new_data))
	    || g_strcmp0 (nm_config_data_get_connectivity_response (old_data), nm_config_data_get_connectivity_response (new_data))) {
		nm_log_dbg (LOGD_CORE, "config: reload: change '" NM_CONFIG_CHANGES_CONNECTIVITY "'");
		g_hash_table_insert (changes, NM_CONFIG_CHANGES_CONNECTIVITY, NULL);
	}

	if (g_hash_table_size (changes))
		new_data = g_object_ref (new_data);
	else
		new_data = NULL;

	g_object_unref (new);

	if (new_data) {
		old_data = priv->config_data;
		priv->config_data = new_data;
		g_signal_emit (self, signals[SIGNAL_CONFIG_CHANGED], 0, changes, old_data);
		g_object_unref (old_data);
	}

	g_hash_table_destroy (changes);
}

NM_DEFINE_SINGLETON_DESTRUCTOR (NMConfig);
NM_DEFINE_SINGLETON_WEAK_REF (NMConfig);

NMConfig *
nm_config_get (void)
{
	g_assert (singleton_instance);
	return singleton_instance;
}

NMConfig *
nm_config_setup (const NMConfigCmdLineOptions *cli, GError **error)
{
	g_assert (!singleton_instance);

	singleton_instance = nm_config_new (cli, error);
	if (singleton_instance)
		nm_singleton_instance_weak_ref_register ();
	return singleton_instance;
}

NMConfig *
nm_config_new (const NMConfigCmdLineOptions *cli, GError **error)
{
	NMConfigPrivate *priv = NULL;
	NMConfig *self;
	char *connectivity_uri, *connectivity_response;
	guint connectivity_interval;
	GKeyFile *keyfile;

	self = NM_CONFIG (g_object_new (NM_TYPE_CONFIG, NULL));
	priv = NM_CONFIG_GET_PRIVATE (self);

	if (!cli)
		_nm_config_cmd_line_options_clear (&priv->cli);
	else
		_nm_config_cmd_line_options_copy (cli, &priv->cli);

	/* Now read the overrides in the config dir */
	if (priv->cli.config_dir)
		priv->config_dir = g_strdup (priv->cli.config_dir);
	else
		priv->config_dir = g_strdup (NM_DEFAULT_SYSTEM_CONF_DIR);

	keyfile = read_entire_config (&priv->cli,
	                              priv->config_dir,
	                              &priv->config_main_file,
	                              &priv->config_description,
	                              error);
	if (!keyfile) {
		g_object_unref (self);
		return NULL;
	}
	g_key_file_free (priv->keyfile);
	priv->keyfile = keyfile;

	/* Handle no-auto-default key and state file */
	priv->no_auto_default = g_key_file_get_string_list (priv->keyfile, "main", "no-auto-default", NULL, NULL);
	if (priv->cli.no_auto_default_file)
		priv->no_auto_default_file = g_strdup (priv->cli.no_auto_default_file);
	else
		priv->no_auto_default_file = g_strdup (NM_NO_AUTO_DEFAULT_STATE_FILE);
	merge_no_auto_default_state (self);

	/* Now let command-line options override the config files, and fill in priv. */
	if (priv->cli.plugins && priv->cli.plugins[0])
		g_key_file_set_value (priv->keyfile, "main", "plugins", priv->cli.plugins);
	priv->plugins = g_key_file_get_string_list (priv->keyfile, "main", "plugins", NULL, NULL);
	if (!priv->plugins && STRLEN (CONFIG_PLUGINS_DEFAULT) > 0)
		priv->plugins = g_strsplit (CONFIG_PLUGINS_DEFAULT, ",", -1);

	priv->monitor_connection_files = _get_bool_value (priv->keyfile, "main", "monitor-connection-files", FALSE);

	priv->auth_polkit = _get_bool_value (priv->keyfile, "main", "auth-polkit", NM_CONFIG_DEFAULT_AUTH_POLKIT);

	priv->dhcp_client = g_key_file_get_value (priv->keyfile, "main", "dhcp", NULL);
	priv->dns_mode = g_key_file_get_value (priv->keyfile, "main", "dns", NULL);

	priv->log_level = g_key_file_get_value (priv->keyfile, "logging", "level", NULL);
	priv->log_domains = g_key_file_get_value (priv->keyfile, "logging", "domains", NULL);

	priv->debug = g_key_file_get_value (priv->keyfile, "main", "debug", NULL);

	if (priv->cli.connectivity_uri && priv->cli.connectivity_uri[0])
		g_key_file_set_value (priv->keyfile, "connectivity", "uri", priv->cli.connectivity_uri);
	connectivity_uri = g_key_file_get_value (priv->keyfile, "connectivity", "uri", NULL);

	if (priv->cli.connectivity_interval >= 0)
		g_key_file_set_integer (priv->keyfile, "connectivity", "interval", priv->cli.connectivity_interval);
	connectivity_interval = g_key_file_get_integer (priv->keyfile, "connectivity", "interval", NULL);

	if (priv->cli.connectivity_response && priv->cli.connectivity_response[0])
		g_key_file_set_value (priv->keyfile, "connectivity", "response", priv->cli.connectivity_response);
	connectivity_response = g_key_file_get_value (priv->keyfile, "connectivity", "response", NULL);

	priv->ignore_carrier = g_key_file_get_string_list (priv->keyfile, "main", "ignore-carrier", NULL, NULL);

	priv->configure_and_quit = _get_bool_value (priv->keyfile, "main", "configure-and-quit", FALSE);

	priv->config_data = g_object_new (NM_TYPE_CONFIG_DATA,
	                                  NM_CONFIG_DATA_CONNECTIVITY_URI, connectivity_uri,
	                                  NM_CONFIG_DATA_CONNECTIVITY_INTERVAL, connectivity_interval,
	                                  NM_CONFIG_DATA_CONNECTIVITY_RESPONSE, connectivity_response,
	                                  NULL);
	priv->config_data_orig = g_object_ref (priv->config_data);
	g_free (connectivity_uri);
	g_free (connectivity_response);

	return self;
}

static void
nm_config_init (NMConfig *config)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (config);

	priv->auth_polkit = NM_CONFIG_DEFAULT_AUTH_POLKIT;

	priv->keyfile = nm_config_create_keyfile ();
}

static void
finalize (GObject *gobject)
{
	NMConfigPrivate *priv = NM_CONFIG_GET_PRIVATE (gobject);

	g_free (priv->config_main_file);
	g_free (priv->config_dir);
	g_free (priv->config_description);
	g_free (priv->no_auto_default_file);
	g_clear_pointer (&priv->keyfile, g_key_file_unref);
	g_strfreev (priv->plugins);
	g_free (priv->dhcp_client);
	g_free (priv->dns_mode);
	g_free (priv->log_level);
	g_free (priv->log_domains);
	g_free (priv->debug);
	g_strfreev (priv->no_auto_default);
	g_strfreev (priv->ignore_carrier);

	_nm_config_cmd_line_options_clear (&priv->cli);

	g_clear_object (&priv->config_data);
	g_clear_object (&priv->config_data_orig);

	G_OBJECT_CLASS (nm_config_parent_class)->finalize (gobject);
}


static void
nm_config_class_init (NMConfigClass *config_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (config_class);

	g_type_class_add_private (config_class, sizeof (NMConfigPrivate));
	object_class->finalize = finalize;

	signals[SIGNAL_CONFIG_CHANGED] =
	    g_signal_new (NM_CONFIG_SIGNAL_CONFIG_CHANGED,
	                  G_OBJECT_CLASS_TYPE (object_class),
	                  G_SIGNAL_RUN_FIRST,
	                  G_STRUCT_OFFSET (NMConfigClass, config_changed),
	                  NULL, NULL, NULL,
	                  G_TYPE_NONE, 2, G_TYPE_HASH_TABLE, NM_TYPE_CONFIG_DATA);
}

