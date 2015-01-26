/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
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
 * Copyright (C) 2014 Red Hat, Inc.
 *
 */

#include "config.h"

#include <glib.h>
#include <string.h>
#include <errno.h>

#include "NetworkManagerUtils.h"
#include "nm-logging.h"
#include "nm-core-internal.h"

#include "nm-test-utils.h"

static void
test_nm_utils_ascii_str_to_int64_check (const char *str, guint base, gint64 min,
                                        gint64 max, gint64 fallback, int exp_errno,
                                        gint64 exp_val)
{
	gint64 v;

	errno = 1;
	v = nm_utils_ascii_str_to_int64 (str, base, min, max, fallback);
	g_assert_cmpint (errno, ==, exp_errno);
	g_assert_cmpint (v, ==, exp_val);
}

static void
test_nm_utils_ascii_str_to_int64_do (const char *str, guint base, gint64 min,
                                     gint64 max, gint64 fallback, int exp_errno,
                                     gint64 exp_val)
{
	const char *sign = "";
	const char *val;
	static const char *whitespaces[] = {
		"",
		" ",
		"\r\n\t",
		" \r\n\t ",
		" \r\n\t \t\r\n\t",
		NULL,
	};
	static const char *nulls[] = {
		"",
		"0",
		"00",
		"0000",
		"0000000000000000",
		"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
		"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
		NULL,
	};
	const char **ws_pre, **ws_post, **null;
	guint i;

	if (str == NULL || exp_errno != 0) {
		test_nm_utils_ascii_str_to_int64_check (str, base, min, max, fallback, exp_errno, exp_val);
		return;
	}

	if (strncmp (str, "-", 1) == 0)
		sign = "-";

	val = str + strlen (sign);

	for (ws_pre = whitespaces; *ws_pre; ws_pre++) {
		for (ws_post = whitespaces; *ws_post; ws_post++) {
			for (null = nulls; *null; null++) {
				for (i = 0; ; i++) {
					char *s;
					const char *str_base = "";

					if (base == 16) {
						if (i == 1)
							str_base = "0x";
						else if (i > 1)
							break;
					} else if (base == 8) {
						if (i == 1)
							str_base = "0";
						else if (i > 1)
							break;
					} else if (base == 0) {
						if (i > 0)
							break;
						/* with base==0, a leading zero would be interpreted as octal. Only test without *null */
						if ((*null)[0])
							break;
					} else {
						if (i > 0)
							break;
					}

					s = g_strdup_printf ("%s%s%s%s%s%s", *ws_pre, sign, str_base, *null, val, *ws_post);

					test_nm_utils_ascii_str_to_int64_check (s, base, min, max, fallback, exp_errno, exp_val);
					g_free (s);
				}
			}
		}
	}
}

static void
test_nm_utils_ascii_str_to_int64 (void)
{
	test_nm_utils_ascii_str_to_int64_do (NULL, 10, 0, 10000, -1, EINVAL, -1);
	test_nm_utils_ascii_str_to_int64_do ("", 10, 0, 10000, -1, EINVAL, -1);
	test_nm_utils_ascii_str_to_int64_do ("1x", 10, 0, 10000, -1, EINVAL, -1);
	test_nm_utils_ascii_str_to_int64_do ("4711", 10, 0, 10000, -1, 0, 4711);
	test_nm_utils_ascii_str_to_int64_do ("10000", 10, 0, 10000, -1, 0, 10000);
	test_nm_utils_ascii_str_to_int64_do ("10001", 10, 0, 10000, -1, ERANGE, -1);
	test_nm_utils_ascii_str_to_int64_do ("FF", 16, 0, 10000, -1, 0, 255);
	test_nm_utils_ascii_str_to_int64_do ("FF", 10, 0, 10000, -2, EINVAL, -2);
	test_nm_utils_ascii_str_to_int64_do ("9223372036854775807", 10, 0, G_MAXINT64, -2, 0, G_MAXINT64);
	test_nm_utils_ascii_str_to_int64_do ("7FFFFFFFFFFFFFFF", 16, 0, G_MAXINT64, -2, 0, G_MAXINT64);
	test_nm_utils_ascii_str_to_int64_do ("9223372036854775808", 10, 0, G_MAXINT64, -2, ERANGE, -2);
	test_nm_utils_ascii_str_to_int64_do ("-9223372036854775808", 10, G_MININT64, 0, -2, 0, G_MININT64);
	test_nm_utils_ascii_str_to_int64_do ("-9223372036854775808", 10, G_MININT64+1, 0, -2, ERANGE, -2);
	test_nm_utils_ascii_str_to_int64_do ("-9223372036854775809", 10, G_MININT64, 0, -2, ERANGE, -2);
	test_nm_utils_ascii_str_to_int64_do ("1.0", 10, 1, 1, -1, EINVAL, -1);
	test_nm_utils_ascii_str_to_int64_do ("1x0", 16, -10, 10, -100, EINVAL, -100);
	test_nm_utils_ascii_str_to_int64_do ("0", 16, -10, 10, -100, 0, 0);
	test_nm_utils_ascii_str_to_int64_do ("10001111", 2, -1000, 1000, -100000, 0, 0x8F);
	test_nm_utils_ascii_str_to_int64_do ("-10001111", 2, -1000, 1000, -100000, 0, -0x8F);
	test_nm_utils_ascii_str_to_int64_do ("1111111", 2, G_MININT64, G_MAXINT64, -1, 0, 0x7F);
	test_nm_utils_ascii_str_to_int64_do ("111111111111111", 2, G_MININT64, G_MAXINT64, -1, 0, 0x7FFF);
	test_nm_utils_ascii_str_to_int64_do ("11111111111111111111111111111111111111111111111", 2, G_MININT64, G_MAXINT64, -1, 0, 0x7FFFFFFFFFFF);
	test_nm_utils_ascii_str_to_int64_do ("111111111111111111111111111111111111111111111111111111111111111", 2, G_MININT64, G_MAXINT64, -1, 0, 0x7FFFFFFFFFFFFFFF);
	test_nm_utils_ascii_str_to_int64_do ("100000000000000000000000000000000000000000000000000000000000000", 2, G_MININT64, G_MAXINT64, -1, 0,  0x4000000000000000);
	test_nm_utils_ascii_str_to_int64_do ("1000000000000000000000000000000000000000000000000000000000000000", 2, G_MININT64, G_MAXINT64, -1, ERANGE, -1);
	test_nm_utils_ascii_str_to_int64_do ("-100000000000000000000000000000000000000000000000000000000000000", 2, G_MININT64, G_MAXINT64, -1, 0,  -0x4000000000000000);
	test_nm_utils_ascii_str_to_int64_do ("111111111111111111111111111111111111111111111111111111111111111",  2, G_MININT64, G_MAXINT64, -1, 0, 0x7FFFFFFFFFFFFFFF);
	test_nm_utils_ascii_str_to_int64_do ("-100000000000000000000000000000000000000000000000000000000000000",  2, G_MININT64, G_MAXINT64, -1, 0,  -0x4000000000000000);
	test_nm_utils_ascii_str_to_int64_do ("0x70",  10, G_MININT64, G_MAXINT64, -1, EINVAL, -1);
	test_nm_utils_ascii_str_to_int64_do ("4711",  0, G_MININT64, G_MAXINT64, -1, 0, 4711);
	test_nm_utils_ascii_str_to_int64_do ("04711",  0, G_MININT64, G_MAXINT64, -1, 0, 04711);
	test_nm_utils_ascii_str_to_int64_do ("0x4711",  0, G_MININT64, G_MAXINT64, -1, 0, 0x4711);
	test_nm_utils_ascii_str_to_int64_do ("080",  0, G_MININT64, G_MAXINT64, -1, EINVAL, -1);
	test_nm_utils_ascii_str_to_int64_do ("070",  0, G_MININT64, G_MAXINT64, -1, 0, 7*8);
	test_nm_utils_ascii_str_to_int64_do ("0x70",  0, G_MININT64, G_MAXINT64, -1, 0, 0x70);
}

/* Reference implementation for nm_utils_ip6_address_clear_host_address.
 * Taken originally from set_address_masked(), src/rdisc/nm-lndp-rdisc.c
 **/
static void
ip6_address_clear_host_address_reference (struct in6_addr *dst, struct in6_addr *src, guint8 plen)
{
	guint nbytes = plen / 8;
	guint nbits = plen % 8;

	g_return_if_fail (plen <= 128);
	g_assert (src);
	g_assert (dst);

	if (plen >= 128)
		*dst = *src;
	else {
		memset (dst, 0, sizeof (*dst));
		memcpy (dst, src, nbytes);
		dst->s6_addr[nbytes] = (src->s6_addr[nbytes] & (0xFF << (8 - nbits)));
	}
}

static void
_randomize_in6_addr (struct in6_addr *addr, GRand *r)
{
	int i;

	for (i=0; i < 4; i++)
		((guint32 *)addr)[i] = g_rand_int (r);
}

static void
test_nm_utils_ip6_address_clear_host_address (void)
{
	GRand *r = g_rand_new ();
	int plen, i;

	g_rand_set_seed (r, 0);

	for (plen = 0; plen <= 128; plen++) {
		for (i =0; i<50; i++) {
			struct in6_addr addr_src, addr_ref;
			struct in6_addr addr1, addr2;

			_randomize_in6_addr (&addr_src, r);
			_randomize_in6_addr (&addr_ref, r);
			_randomize_in6_addr (&addr1, r);
			_randomize_in6_addr (&addr2, r);

			addr1 = addr_src;
			ip6_address_clear_host_address_reference (&addr_ref, &addr1, plen);

			_randomize_in6_addr (&addr1, r);
			_randomize_in6_addr (&addr2, r);
			addr1 = addr_src;
			nm_utils_ip6_address_clear_host_address (&addr2, &addr1, plen);
			g_assert_cmpint (memcmp (&addr1, &addr_src, sizeof (struct in6_addr)), ==, 0);
			g_assert_cmpint (memcmp (&addr2, &addr_ref, sizeof (struct in6_addr)), ==, 0);

			/* test for self assignment/inplace update. */
			_randomize_in6_addr (&addr1, r);
			addr1 = addr_src;
			nm_utils_ip6_address_clear_host_address (&addr1, &addr1, plen);
			g_assert_cmpint (memcmp (&addr1, &addr_ref, sizeof (struct in6_addr)), ==, 0);
		}
	}

	g_rand_free (r);
}


static void
test_nm_utils_log_connection_diff (void)
{
	NMConnection *connection;
	NMConnection *connection2;

	/* if logging is disabled (the default), nm_utils_log_connection_diff() returns
	 * early without doing anything. Hence, in the normal testing, this test does nothing.
	 * It only gets interesting, when run verbosely with NMTST_DEBUG=debug ... */

	nm_log (LOGL_DEBUG, LOGD_CORE, "START TEST test_nm_utils_log_connection_diff...");

	connection = nm_simple_connection_new ();
	nm_connection_add_setting (connection, nm_setting_connection_new ());
	nm_utils_log_connection_diff (connection, NULL, LOGL_DEBUG, LOGD_CORE, "test1", ">>> ");

	nm_connection_add_setting (connection, nm_setting_wired_new ());
	nm_utils_log_connection_diff (connection, NULL, LOGL_DEBUG, LOGD_CORE, "test2", ">>> ");

	connection2 = nm_simple_connection_new_clone (connection);
	nm_utils_log_connection_diff (connection, connection2, LOGL_DEBUG, LOGD_CORE, "test3", ">>> ");

	g_object_set (nm_connection_get_setting_connection (connection),
	              NM_SETTING_CONNECTION_ID, "id",
	              NM_SETTING_CONNECTION_UUID, "uuid",
	              NULL);
	g_object_set (nm_connection_get_setting_connection (connection2),
	              NM_SETTING_CONNECTION_ID, "id2",
	              NM_SETTING_CONNECTION_MASTER, "master2",
	              NULL);
	nm_utils_log_connection_diff (connection, connection2, LOGL_DEBUG, LOGD_CORE, "test4", ">>> ");

	nm_connection_add_setting (connection, nm_setting_802_1x_new ());
	nm_utils_log_connection_diff (connection, connection2, LOGL_DEBUG, LOGD_CORE, "test5", ">>> ");

	g_object_set (nm_connection_get_setting_802_1x (connection),
	              NM_SETTING_802_1X_PASSWORD, "id2",
	              NM_SETTING_802_1X_PASSWORD_FLAGS, NM_SETTING_SECRET_FLAG_NOT_SAVED,
	              NULL);
	nm_utils_log_connection_diff (connection, NULL, LOGL_DEBUG, LOGD_CORE, "test6", ">>> ");
	nm_utils_log_connection_diff (connection, connection2, LOGL_DEBUG, LOGD_CORE, "test7", ">>> ");
	nm_utils_log_connection_diff (connection2, connection, LOGL_DEBUG, LOGD_CORE, "test8", ">>> ");

	g_clear_object (&connection);
	g_clear_object (&connection2);

	connection = nmtst_create_minimal_connection ("id-vpn-1", NULL, NM_SETTING_VPN_SETTING_NAME, NULL);
	nm_utils_log_connection_diff (connection, NULL, LOGL_DEBUG, LOGD_CORE, "test-vpn-1", ">>> ");

	g_clear_object (&connection);
}

/*******************************************/

static NMConnection *
_match_connection_new (void)
{
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	NMSettingIPConfig *s_ip4, *s_ip6;
	char *uuid;

	connection = nm_simple_connection_new ();

	s_con = (NMSettingConnection *) nm_setting_connection_new ();
	nm_connection_add_setting (connection, (NMSetting *) s_con);
	uuid = nm_utils_uuid_generate ();
	g_object_set (G_OBJECT (s_con),
	              NM_SETTING_CONNECTION_ID, "blahblah",
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NULL);
	g_free (uuid);

	s_wired = (NMSettingWired *) nm_setting_wired_new ();
	nm_connection_add_setting (connection, (NMSetting *) s_wired);

	s_ip4 = (NMSettingIPConfig *) nm_setting_ip4_config_new ();
	nm_connection_add_setting (connection, (NMSetting *) s_ip4);
	g_object_set (G_OBJECT (s_ip4),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
	              NULL);

	s_ip6 = (NMSettingIPConfig *) nm_setting_ip6_config_new ();
	nm_connection_add_setting (connection, (NMSetting *) s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
	              NULL);

	return connection;
}

static void
test_connection_match_basic (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingIPConfig *s_ip4;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == copy);

	/* Now change a material property like IPv4 method and ensure matching fails */
	s_ip4 = nm_connection_get_setting_ip4_config (orig);
	g_assert (s_ip4);
	g_object_set (G_OBJECT (s_ip4),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL,
	              NULL);
	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == NULL);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static void
test_connection_match_ip6_method (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingIPConfig *s_ip6;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	/* Check that if the generated connection is IPv6 method=link-local, and the
	 * candidate is both method=auto and may-faily=true, that the candidate is
	 * matched.
	 */
	s_ip6 = nm_connection_get_setting_ip6_config (orig);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL,
	              NULL);

	s_ip6 = nm_connection_get_setting_ip6_config (copy);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
	              NM_SETTING_IP_CONFIG_MAY_FAIL, TRUE,
	              NULL);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == copy);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static void
test_connection_match_ip6_method_ignore (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingIPConfig *s_ip6;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	/* Check that if the generated connection is IPv6 method=link-local, and the
	 * candidate is method=ignore, that the candidate is matched.
	 */
	s_ip6 = nm_connection_get_setting_ip6_config (orig);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL,
	              NULL);

	s_ip6 = nm_connection_get_setting_ip6_config (copy);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
	              NULL);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == copy);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static void
test_connection_match_ip6_method_ignore_auto (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingIPConfig *s_ip6;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	/* Check that if the generated connection is IPv6 method=auto, and the
	 * candidate is method=ignore, that the candidate is matched.
	 */
	s_ip6 = nm_connection_get_setting_ip6_config (orig);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
	              NULL);

	s_ip6 = nm_connection_get_setting_ip6_config (copy);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
	              NULL);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == copy);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}


static void
test_connection_match_ip4_method (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingIPConfig *s_ip4;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	/* Check that if the generated connection is IPv4 method=disabled, and the
	 * candidate is both method=auto and may-faily=true, and the device has no
	 * carrier that the candidate is matched.
	 */
	s_ip4 = nm_connection_get_setting_ip4_config (orig);
	g_assert (s_ip4);
	g_object_set (G_OBJECT (s_ip4),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_DISABLED,
	              NULL);

	s_ip4 = nm_connection_get_setting_ip4_config (copy);
	g_assert (s_ip4);
	g_object_set (G_OBJECT (s_ip4),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
	              NM_SETTING_IP_CONFIG_MAY_FAIL, TRUE,
	              NULL);

	matched = nm_utils_match_connection (connections, orig, FALSE, NULL, NULL);
	g_assert (matched == copy);

	/* Ensure when carrier=true matching fails */
	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == NULL);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static void
test_connection_match_interface_name (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingConnection *s_con;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	/* Check that if the generated connection has an interface name and the
	 * candidate's interface name is NULL, that the candidate is matched.
	 */
	s_con = nm_connection_get_setting_connection (orig);
	g_assert (s_con);
	g_object_set (G_OBJECT (s_con),
	              NM_SETTING_CONNECTION_INTERFACE_NAME, "em1",
	              NULL);

	s_con = nm_connection_get_setting_connection (copy);
	g_assert (s_con);
	g_object_set (G_OBJECT (s_con),
	              NM_SETTING_CONNECTION_INTERFACE_NAME, NULL,
	              NULL);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == copy);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static void
test_connection_match_wired (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingWired *s_wired;
	char *subchan_arr[] = { "0.0.8000", "0.0.8001", "0.0.8002", NULL };
	const char *mac = "52:54:00:ab:db:23";

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	s_wired = nm_connection_get_setting_wired (orig);
	g_assert (s_wired);
	g_object_set (G_OBJECT (s_wired),
	              NM_SETTING_WIRED_PORT, "tp",           /* port is not compared */
	              NM_SETTING_WIRED_MAC_ADDRESS, mac,     /* we allow MAC address just in one connection */
	              NM_SETTING_WIRED_S390_SUBCHANNELS, subchan_arr,
	              NM_SETTING_WIRED_S390_NETTYPE, "qeth",
	              NULL);

	s_wired = nm_connection_get_setting_wired (copy);
	g_assert (s_wired);
	g_object_set (G_OBJECT (s_wired),
	              NM_SETTING_WIRED_S390_SUBCHANNELS, subchan_arr,
	              NM_SETTING_WIRED_S390_NETTYPE, "qeth",
	              NULL);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched == copy);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static void
test_connection_no_match_ip4_addr (void)
{
	NMConnection *orig, *copy, *matched;
	GSList *connections = NULL;
	NMSettingIPConfig *s_ip4, *s_ip6;
	NMIPAddress *nm_addr;
	GError *error = NULL;

	orig = _match_connection_new ();
	copy = nm_simple_connection_new_clone (orig);
	connections = g_slist_append (connections, copy);

	/* Check that if we have two differences, ipv6.method (exception we allow) and
	 * ipv4.addresses (which is fatal), we don't match the connections.
	 */
	s_ip6 = nm_connection_get_setting_ip6_config (orig);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL,
	              NULL);

	s_ip6 = nm_connection_get_setting_ip6_config (copy);
	g_assert (s_ip6);
	g_object_set (G_OBJECT (s_ip6),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
	              NULL);


	s_ip4 = nm_connection_get_setting_ip4_config (orig);
	g_assert (s_ip4);
	g_object_set (G_OBJECT (s_ip4),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
	              NM_SETTING_IP_CONFIG_GATEWAY, "1.1.1.254",
	              NULL);
	nm_addr = nm_ip_address_new (AF_INET, "1.1.1.4", 24, &error);
	g_assert_no_error (error);
	nm_setting_ip_config_add_address (s_ip4, nm_addr);
	nm_ip_address_unref (nm_addr);

	s_ip4 = nm_connection_get_setting_ip4_config (copy);
	g_assert (s_ip4);
	g_object_set (G_OBJECT (s_ip4),
	              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
	              NM_SETTING_IP_CONFIG_GATEWAY, "2.2.2.254",
	              NULL);
	nm_addr = nm_ip_address_new (AF_INET, "2.2.2.4", 24, &error);
	g_assert_no_error (error);
	nm_setting_ip_config_add_address (s_ip4, nm_addr);
	nm_ip_address_unref (nm_addr);

	matched = nm_utils_match_connection (connections, orig, TRUE, NULL, NULL);
	g_assert (matched != copy);

	g_slist_free (connections);
	g_object_unref (orig);
	g_object_unref (copy);
}

static NMConnection *
_create_connection_autoconnect (const char *id, gboolean autoconnect, int autoconnect_priority)
{
	NMConnection *c;
	NMSettingConnection *s_con;

	c = nmtst_create_minimal_connection (id, NULL, NM_SETTING_WIRED_SETTING_NAME, &s_con);
	g_object_set (s_con,
	              NM_SETTING_CONNECTION_AUTOCONNECT, autoconnect,
	              NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY, autoconnect_priority,
	              NULL);
	nmtst_connection_normalize (c);
	return c;
}

static void
_test_connection_sort_autoconnect_priority_one (NMConnection **list, gboolean shuffle)
{
	int i, j;
	int count = 0;
	gs_unref_ptrarray GPtrArray *connections = g_ptr_array_new ();

	while (list[count])
		count++;
	g_assert (count > 1);

	/* copy the list of connections over to @connections and shuffle. */
	for (i = 0; i < count; i++)
		g_ptr_array_add (connections, list[i]);
	if (shuffle) {
		for (i = count - 1; i > 0; i--) {
			j = g_rand_int (nmtst_get_rand ()) % (i + 1);
			NMTST_SWAP (connections->pdata[i], connections->pdata[j]);
		}
	}

	/* sort it... */
	g_ptr_array_sort (connections, (GCompareFunc) nm_utils_cmp_connection_by_autoconnect_priority);

	for (i = 0; i < count; i++) {
		if (list[i] == connections->pdata[i])
			continue;
		if (shuffle && nm_utils_cmp_connection_by_autoconnect_priority (&list[i], (NMConnection **) &connections->pdata[i]) == 0)
			continue;
		g_message ("After sorting, the order of connections is not as expected!! Offending index: %d", i);
		for (j = 0; j < count; j++)
			g_message ("  %3d:  %p/%-20s - %p/%-20s", j, list[j], nm_connection_get_id (list[j]), connections->pdata[j], nm_connection_get_id (connections->pdata[j]));
		g_assert_not_reached ();
	}
}

static void
_test_connection_sort_autoconnect_priority_free (NMConnection **list)
{
	while (*list) {
		g_object_unref (*list);
		*list = NULL;
	}
}

static void
test_connection_sort_autoconnect_priority (void)
{
	NMConnection *c1[] = {
		_create_connection_autoconnect ("AC/100", TRUE, 100),
		_create_connection_autoconnect ("AC/100", TRUE, 100),
		_create_connection_autoconnect ("AC/99", TRUE, 99),
		_create_connection_autoconnect ("AC/0", TRUE, 0),
		_create_connection_autoconnect ("AC/0", TRUE, 0),
		_create_connection_autoconnect ("AC/-1", TRUE, -1),
		_create_connection_autoconnect ("AC/-3", TRUE, -3),
		_create_connection_autoconnect ("ac/0", FALSE, 0),
		_create_connection_autoconnect ("ac/0", FALSE, 0),
		_create_connection_autoconnect ("ac/1", FALSE, 1),
		_create_connection_autoconnect ("ac/-1", FALSE, -1),
		_create_connection_autoconnect ("ac/1", FALSE, 1),
		_create_connection_autoconnect ("ac/0", FALSE, 0),
		NULL,
	};
	NMConnection *c2[] = {
		_create_connection_autoconnect ("AC/100", TRUE, 100),
		_create_connection_autoconnect ("AC/99", TRUE, 99),
		_create_connection_autoconnect ("AC/0", TRUE, 0),
		_create_connection_autoconnect ("AC/-1", TRUE, -1),
		_create_connection_autoconnect ("AC/-3", TRUE, -3),
		_create_connection_autoconnect ("ac/0", FALSE, 0),
		NULL,
	};

	_test_connection_sort_autoconnect_priority_one (c1, FALSE);
	_test_connection_sort_autoconnect_priority_one (c2, FALSE);
	_test_connection_sort_autoconnect_priority_one (c1, TRUE);
	_test_connection_sort_autoconnect_priority_one (c2, TRUE);

	_test_connection_sort_autoconnect_priority_free (c1);
	_test_connection_sort_autoconnect_priority_free (c2);
}

/*******************************************/

static void
__test_uuid (const char *expected_uuid, const char *str, gssize slen, char *uuid_test)
{
	g_assert (uuid_test);
	g_assert (nm_utils_is_uuid (uuid_test));

	if (strcmp (uuid_test, expected_uuid)) {
		g_error ("UUID test failed (1): text=%s, len=%lld, expected=%s, uuid_test=%s",
		         str, (long long) slen, expected_uuid, uuid_test);
	}
	g_free (uuid_test);

	uuid_test = nm_utils_uuid_generate_from_string (str, slen, NM_UTILS_UUID_TYPE_VARIANT3, NM_UTILS_UUID_NS);

	g_assert (uuid_test);
	g_assert (nm_utils_is_uuid (uuid_test));

	if (strcmp (uuid_test, expected_uuid)) {
		g_error ("UUID test failed (2): text=%s; len=%lld, expected=%s, uuid2=%s",
		         str, (long long) slen, expected_uuid, uuid_test);
	}
	g_free (uuid_test);
}

#define _test_uuid(expected_uuid, str, strlen, ...) __test_uuid (expected_uuid, str, strlen, nm_utils_uuid_generate_from_strings(__VA_ARGS__, NULL))

static void
test_nm_utils_uuid_generate_from_strings (void)
{
	_test_uuid ("b07c334a-399b-32de-8d50-58e4e08f98e3", "",         0, NULL);
	_test_uuid ("b8a426cb-bcb5-30a3-bd8f-6786fea72df9", "\0",       1, "");
	_test_uuid ("12a4a982-7aae-39e1-951e-41aeb1250959", "a\0",      2, "a");
	_test_uuid ("69e22c7e-f89f-3a43-b239-1cb52ed8db69", "aa\0",     3, "aa");
	_test_uuid ("59829fd3-5ad5-3d90-a7b0-4911747e4088", "\0\0",     2, "",   "");
	_test_uuid ("01ad0e06-6c50-3384-8d86-ddab81421425", "a\0\0",    3, "a",  "");
	_test_uuid ("e1ed8647-9ed3-3ec8-8c6d-e8204524d71d", "aa\0\0",   4, "aa", "");
	_test_uuid ("fb1c7cd6-275c-3489-9382-83b900da8af0", "\0a\0",    3, "",   "a");
	_test_uuid ("5d79494e-c4ba-31a6-80a2-d6016ccd7e17", "a\0a\0",   4, "a",  "a");
	_test_uuid ("fd698d86-1b60-3ebe-855f-7aada9950a8d", "aa\0a\0",  5, "aa", "a");
	_test_uuid ("8c573b48-0f01-30ba-bb94-c5f59f4fe517", "\0aa\0",   4, "",   "aa");
	_test_uuid ("2bdd3d46-eb83-3c53-a41b-a724d04b5544", "a\0aa\0",  5, "a",  "aa");
	_test_uuid ("13d4b780-07c1-3ba7-b449-81c4844ef039", "aa\0aa\0", 6, "aa", "aa");
	_test_uuid ("dd265bf7-c05a-3037-9939-b9629858a477", "a\0b\0",   4, "a",  "b");
}

/*******************************************/

static void
test_match_spec_ifname (const char *spec_str, const char **matches, const char **no_matches, const char **neg_matches)
{
	char **spec_str_split;
	GSList *specs = NULL;
	guint i;

	g_assert (spec_str);
	spec_str_split = g_strsplit_set (spec_str, ";,", -1);
	for (i = 0; spec_str_split[i]; i++) {
		if (spec_str_split[i])
			specs = g_slist_prepend (specs, spec_str_split[i]);
	}
	specs = g_slist_reverse (specs);

	for (i = 0; matches && matches[i]; i++)
		g_assert (nm_match_spec_interface_name (specs, matches[i]) == NM_MATCH_SPEC_MATCH);
	for (i = 0; no_matches && no_matches[i]; i++)
		g_assert (nm_match_spec_interface_name (specs, no_matches[i]) == NM_MATCH_SPEC_NO_MATCH);
	for (i = 0; neg_matches && neg_matches[i]; i++)
		g_assert (nm_match_spec_interface_name (specs, neg_matches[i]) == NM_MATCH_SPEC_NEG_MATCH);

	g_slist_free (specs);
	g_strfreev (spec_str_split);
}

static void
test_nm_match_spec_interface_name (void)
{
#define S(...) ((const char *[]) { __VA_ARGS__, NULL } )
	test_match_spec_ifname ("interface-name:em1",
	                        S ("em1"),
	                        S ("em2", "em", "em11"),
	                        NULL);
	test_match_spec_ifname ("interface-name:em*",
	                        S ("em", "em1", "em2", "em11"),
	                        S ("e"),
	                        NULL);
	test_match_spec_ifname ("interface-name:em\\*",
	                        S ("em\\", "em\\1", "em\\2", "em\\11"),
	                        S ("em"),
	                        NULL);
	test_match_spec_ifname ("interface-name:=em*",
	                        S ("em*"),
	                        S ("em", "em1"),
	                        NULL);
#undef S
}

/*******************************************/

NMTST_DEFINE ();

int
main (int argc, char **argv)
{
	nmtst_init_with_logging (&argc, &argv, NULL, "ALL");

	g_test_add_func ("/general/nm_utils_ascii_str_to_int64", test_nm_utils_ascii_str_to_int64);
	g_test_add_func ("/general/nm_utils_ip6_address_clear_host_address", test_nm_utils_ip6_address_clear_host_address);
	g_test_add_func ("/general/nm_utils_log_connection_diff", test_nm_utils_log_connection_diff);

	g_test_add_func ("/general/connection-match/basic", test_connection_match_basic);
	g_test_add_func ("/general/connection-match/ip6-method", test_connection_match_ip6_method);
	g_test_add_func ("/general/connection-match/ip6-method-ignore", test_connection_match_ip6_method_ignore);
	g_test_add_func ("/general/connection-match/ip6-method-ignore-auto", test_connection_match_ip6_method_ignore_auto);
	g_test_add_func ("/general/connection-match/ip4-method", test_connection_match_ip4_method);
	g_test_add_func ("/general/connection-match/con-interface-name", test_connection_match_interface_name);
	g_test_add_func ("/general/connection-match/wired", test_connection_match_wired);
	g_test_add_func ("/general/connection-match/no-match-ip4-addr", test_connection_no_match_ip4_addr);

	g_test_add_func ("/general/connection-sort/autoconnect-priority", test_connection_sort_autoconnect_priority);

	g_test_add_func ("/general/nm_utils_uuid_generate_from_strings", test_nm_utils_uuid_generate_from_strings);
	g_test_add_func ("/general/nm_match_spec_interface_name", test_nm_match_spec_interface_name);

	return g_test_run ();
}

