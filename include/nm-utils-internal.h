/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2014 Red Hat, Inc.
 */

#ifndef __NM_UTILS_INTERNAL_H__
#define __NM_UTILS_INTERNAL_H__


#include <glib.h>

/********************************************************/

/* http://stackoverflow.com/a/11172679 */
#define  _NM_UTILS_MACRO_FIRST(...)                           __NM_UTILS_MACRO_FIRST_HELPER(__VA_ARGS__, throwaway)
#define __NM_UTILS_MACRO_FIRST_HELPER(first, ...)             first

#define  _NM_UTILS_MACRO_REST(...)                            __NM_UTILS_MACRO_REST_HELPER(__NM_UTILS_MACRO_REST_NUM(__VA_ARGS__), __VA_ARGS__)
#define __NM_UTILS_MACRO_REST_HELPER(qty, ...)                __NM_UTILS_MACRO_REST_HELPER2(qty, __VA_ARGS__)
#define __NM_UTILS_MACRO_REST_HELPER2(qty, ...)               __NM_UTILS_MACRO_REST_HELPER_##qty(__VA_ARGS__)
#define __NM_UTILS_MACRO_REST_HELPER_ONE(first)
#define __NM_UTILS_MACRO_REST_HELPER_TWOORMORE(first, ...)    , __VA_ARGS__
#define __NM_UTILS_MACRO_REST_NUM(...) \
    __NM_UTILS_MACRO_REST_SELECT_20TH(__VA_ARGS__, \
                TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE,\
                TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE,\
                TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE, TWOORMORE,\
                TWOORMORE, TWOORMORE, TWOORMORE, ONE, throwaway)
#define __NM_UTILS_MACRO_REST_SELECT_20TH(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, ...) a20

/********************************************************/

#if defined (__GNUC__)
#define _NM_PRAGMA_WARNING_DO(warning)       G_STRINGIFY(GCC diagnostic ignored warning)
#elif defined (__clang__)
#define _NM_PRAGMA_WARNING_DO(warning)       G_STRINGIFY(clang diagnostic ignored warning)
#endif

/* you can only suppress a specific warning that the compiler
 * understands. Otherwise you will get another compiler warning
 * about invalid pragma option.
 * It's not that bad however, because gcc and clang often have the
 * same name for the same warning. */

#if defined (__GNUC__)
#define NM_PRAGMA_WARNING_DISABLE(warning) \
        _Pragma("GCC diagnostic push"); \
        _Pragma(_NM_PRAGMA_WARNING_DO(warning))
#elif defined (__clang__)
#define NM_PRAGMA_WARNING_DISABLE(warning) \
        _Pragma("clang diagnostic push"); \
        _Pragma(_NM_PRAGMA_WARNING_DO(warning))
#else
#define NM_PRAGMA_WARNING_DISABLE(warning)
#endif

#if defined (__GNUC__)
#define NM_PRAGMA_WARNING_REENABLE \
    _Pragma("GCC diagnostic pop")
#elif defined (__clang__)
#define NM_PRAGMA_WARNING_REENABLE \
    _Pragma("clang diagnostic pop")
#else
#define NM_PRAGMA_WARNING_REENABLE
#endif

/********************************************************/

/* macro to return strlen() of a compile time string. */
#define STRLEN(str)     ( sizeof ("" str) - 1 )

#define NM_IN_SET(x, y, ...)                                    \
    ({                                                          \
        const typeof(y) _y = (y);                               \
        typeof(_y) _x = (x);                                    \
        unsigned _i;                                            \
        gboolean _found = FALSE;                                \
        for (_i = 0; _i < 1 + sizeof((typeof(_x)[]) { __VA_ARGS__ })/sizeof(typeof(_x)); _i++) { \
            if (((typeof(_x)[]) { _y, __VA_ARGS__ })[_i] == _x) { \
                _found = TRUE;                                  \
                break;                                          \
            }                                                   \
        }                                                       \
        _found;                                                 \
    })

/*****************************************************************************/

#define NM_DEFINE_SINGLETON_INSTANCE(TYPE) \
static TYPE *singleton_instance

#define NM_DEFINE_SINGLETON_WEAK_REF(TYPE) \
NM_DEFINE_SINGLETON_INSTANCE (TYPE); \
static void \
_singleton_instance_weak_ref_cb (gpointer data, \
                                 GObject *where_the_object_was) \
{ \
	nm_log_dbg (LOGD_CORE, "disposing %s singleton (%p)", G_STRINGIFY (TYPE), singleton_instance); \
	singleton_instance = NULL; \
} \
static inline void \
nm_singleton_instance_weak_ref_register (void) \
{ \
	g_object_weak_ref (G_OBJECT (singleton_instance), _singleton_instance_weak_ref_cb, NULL); \
}

#define NM_DEFINE_SINGLETON_DESTRUCTOR(TYPE) \
NM_DEFINE_SINGLETON_INSTANCE (TYPE); \
static void __attribute__((destructor)) \
_singleton_destructor (void) \
{ \
	if (singleton_instance) { \
		if (G_OBJECT (singleton_instance)->ref_count > 1) \
			nm_log_dbg (LOGD_CORE, "disown %s singleton (%p)", G_STRINGIFY (TYPE), singleton_instance); \
		g_object_unref (singleton_instance); \
	} \
}

/* By default, the getter will assert that the singleton will be created only once. You can
 * change this by redefining NM_DEFINE_SINGLETON_ALLOW_MULTIPLE. */
#ifndef NM_DEFINE_SINGLETON_ALLOW_MULTIPLE
#define NM_DEFINE_SINGLETON_ALLOW_MULTIPLE     FALSE
#endif

#define NM_DEFINE_SINGLETON_GETTER(TYPE, GETTER, GTYPE, ...) \
NM_DEFINE_SINGLETON_INSTANCE (TYPE); \
NM_DEFINE_SINGLETON_WEAK_REF (TYPE); \
TYPE * \
GETTER (void) \
{ \
	if (G_UNLIKELY (!singleton_instance)) { \
		static char _already_created = FALSE; \
\
		g_assert (!_already_created || (NM_DEFINE_SINGLETON_ALLOW_MULTIPLE)); \
		_already_created = TRUE;\
		singleton_instance = (g_object_new (GTYPE, ##__VA_ARGS__, NULL)); \
		g_assert (singleton_instance); \
		nm_singleton_instance_weak_ref_register (); \
		nm_log_dbg (LOGD_CORE, "create %s singleton (%p)", G_STRINGIFY (TYPE), singleton_instance); \
	} \
	return singleton_instance; \
} \
NM_DEFINE_SINGLETON_DESTRUCTOR(TYPE)

/*****************************************************************************/

#endif
