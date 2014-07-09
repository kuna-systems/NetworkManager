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
 * Copyright (C) 2011 Thomas Bechtold <thomasbechtold@jpberlin.de>
 * Copyright (C) 2011 Dan Williams <dcbw@redhat.com>
 */

#include "config.h"

#include <string.h>
#if WITH_CONCHECK
#include <libsoup/soup.h>
#endif

#include "nm-connectivity.h"
#include "nm-logging.h"
#include "nm-config.h"

G_DEFINE_TYPE (NMConnectivity, nm_connectivity, G_TYPE_OBJECT)

#define NM_CONNECTIVITY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_CONNECTIVITY, NMConnectivityPrivate))


#define DEFAULT_RESPONSE "NetworkManager is online" /* NOT LOCALIZED */

typedef struct {
	NMConfig *config;

	char *uri;
	char *response;
	guint interval;

#if WITH_CONCHECK
	SoupSession *soup_session;
	gboolean running;
	gboolean run_again;
	guint check_id;
#endif

	NMConnectivityState state;
} NMConnectivityPrivate;

enum {
	PROP_0,
	PROP_URI,
	PROP_INTERVAL,
	PROP_RESPONSE,
	PROP_STATE,
	LAST_PROP
};


NMConnectivityState
nm_connectivity_get_state (NMConnectivity *connectivity)
{
	g_return_val_if_fail (NM_IS_CONNECTIVITY (connectivity), NM_CONNECTIVITY_UNKNOWN);

	return NM_CONNECTIVITY_GET_PRIVATE (connectivity)->state;
}

static void
update_state (NMConnectivity *self, NMConnectivityState state)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (priv->state != state) {
		priv->state = state;
		g_object_notify (G_OBJECT (self), NM_CONNECTIVITY_STATE);
	}
}

#if WITH_CONCHECK
typedef struct {
	GSimpleAsyncResult *simple;
	char *uri;
	char *response;
} ConCheckCbData;

static void
nm_connectivity_check_cb (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
	ConCheckCbData *cb_data = user_data;
	GSimpleAsyncResult *simple = cb_data->simple;
	NMConnectivity *self;
	NMConnectivityState new_state;
	const char *nm_header;
	const char *uri = cb_data->uri;
	const char *response = cb_data->response ? cb_data->response : DEFAULT_RESPONSE;

	self = NM_CONNECTIVITY (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
	/* it is safe to unref @self here, @simple holds yet another reference. */
	g_object_unref (self);

	if (SOUP_STATUS_IS_TRANSPORT_ERROR (msg->status_code)) {
		nm_log_info (LOGD_CONCHECK, "Connectivity check for uri '%s' failed with '%s'.",
		             uri, msg->reason_phrase);
		new_state = NM_CONNECTIVITY_LIMITED;
		goto done;
	}

	/* Check headers; if we find the NM-specific one we're done */
	nm_header = soup_message_headers_get_one (msg->response_headers, "X-NetworkManager-Status");
	if (g_strcmp0 (nm_header, "online") == 0) {
		nm_log_dbg (LOGD_CONCHECK, "Connectivity check for uri '%s' with Status header successful.", uri);
		new_state = NM_CONNECTIVITY_FULL;
	} else if (msg->status_code == SOUP_STATUS_OK) {
		/* check response */
		if (msg->response_body->data && (g_str_has_prefix (msg->response_body->data, response))) {
			nm_log_dbg (LOGD_CONCHECK, "Connectivity check for uri '%s' successful.",
			            uri);
			new_state = NM_CONNECTIVITY_FULL;
		} else {
			nm_log_info (LOGD_CONCHECK, "Connectivity check for uri '%s' did not match expected response '%s'; assuming captive portal.",
			             uri, response);
			new_state = NM_CONNECTIVITY_PORTAL;
		}
	} else {
		nm_log_info (LOGD_CONCHECK, "Connectivity check for uri '%s' returned status '%d %s'; assuming captive portal.",
		             uri, msg->status_code, msg->reason_phrase);
		new_state = NM_CONNECTIVITY_PORTAL;
	}

 done:
	g_simple_async_result_set_op_res_gssize (simple, new_state);
	g_simple_async_result_complete (simple);

	g_free (cb_data->uri);
	g_free (cb_data->response);
	g_free (cb_data);

	update_state (self, new_state);
}

static gboolean run_check (gpointer user_data);

static void
run_check_complete (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
	NMConnectivity *self = NM_CONNECTIVITY (object);
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);
	GError *error = NULL;

	nm_connectivity_check_finish (self, result, &error);
	priv->running = FALSE;
	if (error) {
		nm_log_err (LOGD_CONCHECK, "Connectivity check failed: %s", error->message);
		g_error_free (error);
	}
	if (priv->run_again)
		run_check (self);
}

static gboolean
run_check (gpointer user_data)
{
	NMConnectivity *self = NM_CONNECTIVITY (user_data);
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	g_assert (priv->uri);

	nm_log_dbg (LOGD_CONCHECK, "Connectivity check with uri '%s' started.", priv->uri);
	nm_connectivity_check_async (self, run_check_complete, NULL);
	priv->running = TRUE;
	priv->run_again = FALSE;

	return TRUE;
}

static void
_run_check_cancel (NMConnectivity *self)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (priv->check_id) {
		g_source_remove (priv->check_id);
		priv->check_id = 0;
	}
}

static void
_run_check_ensure_scheduled (NMConnectivity *self, gboolean force_restart)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (priv->uri && priv->interval) {
		if (force_restart && priv->check_id) {
			g_source_remove (priv->check_id);
			priv->check_id = 0;
		}
		if (!priv->check_id)
			priv->check_id = g_timeout_add_seconds (priv->interval, run_check, self);
	} else if (priv->check_id) {
		g_source_remove (priv->check_id);
		priv->check_id = 0;
	}
}
#endif

void
nm_connectivity_set_online (NMConnectivity *self,
                            gboolean        online)
{
#if WITH_CONCHECK
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (online) {
		_run_check_ensure_scheduled (self, FALSE);

		if (priv->check_id) {
			if (!priv->running)
				run_check (self);
			else
				priv->run_again = TRUE;
			return;
		}
	} else
		_run_check_cancel (self);
#endif

	/* Either @online is %TRUE but we aren't checking connectivity, or
	 * @online is %FALSE. Either way we can update our status immediately.
	 */
	update_state (self, online ? NM_CONNECTIVITY_FULL : NM_CONNECTIVITY_NONE);
}

void
nm_connectivity_check_async (NMConnectivity      *self,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
	NMConnectivityPrivate *priv;
	GSimpleAsyncResult *simple;

	g_return_if_fail (NM_IS_CONNECTIVITY (self));
	priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	simple = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                    nm_connectivity_check_async);

#if WITH_CONCHECK
	if (priv->uri && priv->interval) {
		SoupMessage *msg;
		ConCheckCbData *cb_data = g_new (ConCheckCbData, 1);

		msg = soup_message_new ("GET", priv->uri);
		soup_message_set_flags (msg, SOUP_MESSAGE_NO_REDIRECT);
		cb_data->simple = simple;
		cb_data->uri = g_strdup (priv->uri);
		cb_data->response = g_strdup (priv->response);
		soup_session_queue_message (priv->soup_session,
		                            msg,
		                            nm_connectivity_check_cb,
		                            cb_data);

		nm_log_dbg (LOGD_CONCHECK, "connectivity check: send request to '%s'", priv->uri);
		return;
	}else
		nm_log_dbg (LOGD_CONCHECK, "connectivity check: faking request. Connectivity check disabled");
#else
	nm_log_dbg (LOGD_CONCHECK, "connectivity check: faking request. Compiled without libsoup support");
#endif

	g_simple_async_result_set_op_res_gssize (simple, priv->state);
	g_simple_async_result_complete_in_idle (simple);
}

NMConnectivityState
nm_connectivity_check_finish (NMConnectivity  *self,
                              GAsyncResult    *result,
                              GError         **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self), nm_connectivity_check_async), NM_CONNECTIVITY_UNKNOWN);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (simple, error))
		return NM_CONNECTIVITY_UNKNOWN;
	return (NMConnectivityState) g_simple_async_result_get_op_res_gssize (simple);
}

/**************************************************************************/

static gboolean
_set_property_uri (NMConnectivity *self, const char *uri)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (uri && !*uri)
		uri = NULL;

	if (!g_strcmp0 (uri, priv->uri))
		return FALSE;

	g_free (priv->uri);
	priv->uri = g_strdup (uri);

#if WITH_CONCHECK
	if (priv->uri) {
		SoupURI *soup_uri = soup_uri_new (priv->uri);

		if (!soup_uri || !SOUP_URI_VALID_FOR_HTTP (soup_uri)) {
			nm_log_err (LOGD_CONCHECK, "Invalid uri '%s' for connectivity check.", priv->uri);
			priv->uri = NULL;
		}
		if (soup_uri)
			soup_uri_free (soup_uri);
	}
#endif
	g_object_notify (G_OBJECT (self), NM_CONNECTIVITY_URI);
	return TRUE;
}

static gboolean
_set_property_interval (NMConnectivity *self, guint interval, gboolean rerun)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (priv->interval == interval)
		return FALSE;
	priv->interval = interval;
#if WITH_CONCHECK
	_run_check_ensure_scheduled (self, TRUE);
	if (rerun && priv->check_id) {
		if (!priv->running)
			run_check (self);
		else
			priv->run_again = TRUE;
	}
#endif
	g_object_notify (G_OBJECT (self), NM_CONNECTIVITY_URI);
	return TRUE;
}

static gboolean
_set_property_response (NMConnectivity *self, const char *response)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (!g_strcmp0 (response, priv->response))
		return FALSE;

		/* a response %NULL means, DEFAULT_RESPONSE. Any other response
	 * (including "") is accepted. */
	g_free (priv->response);
	priv->response = g_strdup (response);

	g_object_notify (G_OBJECT (self), NM_CONNECTIVITY_URI);
	return TRUE;
}

/**************************************************************************/

static void
_config_changed_cb (NMConfig *config, GHashTable *changes, NMConfigData *old_data, NMConnectivity *self)
{
	gboolean changed = FALSE;
	NMConfigData *new_data;

	g_return_if_fail (NM_CONNECTIVITY_GET_PRIVATE (self)->config == config);

	new_data = nm_config_get_data (config);

	g_object_freeze_notify (G_OBJECT (self));
	changed |= _set_property_uri (self, nm_config_data_get_connectivity_uri (new_data));
	changed |= _set_property_interval (self, nm_config_data_get_connectivity_interval (new_data), FALSE);
	changed |= _set_property_response (self, nm_config_data_get_connectivity_response (new_data));

	if (changed) {
#if WITH_CONCHECK
		NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

		if (priv->check_id) {
			if (!priv->running)
				run_check (self);
			else
				priv->run_again = TRUE;
		}
#endif
	}

	g_object_thaw_notify (G_OBJECT (self));
}

static void
_clear_config (NMConnectivity *self)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	if (priv->config) {
		g_object_remove_weak_pointer (G_OBJECT (priv->config), (gpointer *) &priv->config);
		g_signal_handlers_disconnect_by_func (priv->config, _config_changed_cb, self);
		priv->config = NULL;
	}
}

NMConnectivity *
nm_connectivity_new_with_config (NMConfig *config)
{
	NMConnectivity *self;
	NMConnectivityPrivate *priv;
	NMConfigData *config_data;

	g_return_val_if_fail (config, NULL);

	config_data = nm_config_get_data (config);
	self = g_object_new (NM_TYPE_CONNECTIVITY,
	                     NM_CONNECTIVITY_URI, nm_config_data_get_connectivity_uri (config_data),
	                     NM_CONNECTIVITY_INTERVAL, nm_config_data_get_connectivity_interval (config_data),
	                     NM_CONNECTIVITY_RESPONSE, nm_config_data_get_connectivity_response (config_data),
	                     NULL);
	priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	/* Creating an instance with nm_connectivity_new_with_config() connects
	 * the instance to the NMConfig instance. When the connectivity parameters
	 * of the NMConfig object change, the parameters propagate to the connectivity
	 * object.
	 *
	 * When resetting one of the properties externally, the connection
	 * to the NMConfig instance is released and the NMConnectivity instance
	 * again becomes entirely independent from NMConfig.
	 *
	 * Also, the instance only keeps a weak reference to the NMConfig instance.
	 */
	priv->config = config;
	g_object_add_weak_pointer (G_OBJECT (config), (gpointer *) &priv->config);
	g_signal_connect (G_OBJECT (config),
	                  NM_CONFIG_SIGNAL_CONFIG_CHANGED,
	                  G_CALLBACK (_config_changed_cb),
	                  self);
	return self;
}

static void
set_property (GObject *object, guint property_id,
              const GValue *value, GParamSpec *pspec)
{
	NMConnectivity *self = NM_CONNECTIVITY (object);

	switch (property_id) {
	case PROP_URI:
		_clear_config (self);
		_set_property_uri (self, g_value_get_string (value));
		break;
	case PROP_INTERVAL:
		_clear_config (self);
		_set_property_interval (self, g_value_get_uint (value), TRUE);
		break;
	case PROP_RESPONSE:
		_clear_config (self);
		_set_property_response (self, g_value_get_string (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
get_property (GObject *object, guint property_id,
              GValue *value, GParamSpec *pspec)
{
	NMConnectivity *self = NM_CONNECTIVITY (object);
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	switch (property_id) {
	case PROP_URI:
		g_value_set_string (value, priv->uri);
		break;
	case PROP_INTERVAL:
		g_value_set_uint (value, priv->interval);
		break;
	case PROP_RESPONSE:
		if (priv->response)
			g_value_set_string (value, priv->response);
		else
			g_value_set_static_string (value, DEFAULT_RESPONSE);
		break;
	case PROP_STATE:
		g_value_set_uint (value, priv->state);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}


static void
nm_connectivity_init (NMConnectivity *self)
{
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

#if WITH_CONCHECK
	priv->soup_session = soup_session_async_new_with_options (SOUP_SESSION_TIMEOUT, 15, NULL);
#endif
	priv->state = NM_CONNECTIVITY_NONE;
}


static void
dispose (GObject *object)
{
	NMConnectivity *self = NM_CONNECTIVITY (object);
	NMConnectivityPrivate *priv = NM_CONNECTIVITY_GET_PRIVATE (self);

	g_clear_object (&priv->uri);
	g_clear_object (&priv->response);

#if WITH_CONCHECK
	if (priv->soup_session) {
		soup_session_abort (priv->soup_session);
		g_clear_object (&priv->soup_session);
	}

	_run_check_cancel (self);
#endif
	_clear_config (self);
}


static void
nm_connectivity_class_init (NMConnectivityClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	g_type_class_add_private (klass, sizeof (NMConnectivityPrivate));

	/* virtual methods */
	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->dispose = dispose;

	/* properties */
	g_object_class_install_property
	    (object_class, PROP_URI,
	     g_param_spec_string (NM_CONNECTIVITY_URI, "", "",
	                          NULL,
	                          G_PARAM_READWRITE |
	                          G_PARAM_CONSTRUCT |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_INTERVAL,
	     g_param_spec_uint (NM_CONNECTIVITY_INTERVAL, "", "",
	                        0, G_MAXUINT, 300,
	                        G_PARAM_READWRITE |
	                        G_PARAM_CONSTRUCT |
	                        G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_RESPONSE,
	     g_param_spec_string (NM_CONNECTIVITY_RESPONSE, "", "",
	                          DEFAULT_RESPONSE,
	                          G_PARAM_READWRITE |
	                          G_PARAM_CONSTRUCT |
	                          G_PARAM_STATIC_STRINGS));

	g_object_class_install_property
	    (object_class, PROP_STATE,
	     g_param_spec_uint (NM_CONNECTIVITY_STATE, "", "",
	                        NM_CONNECTIVITY_UNKNOWN, NM_CONNECTIVITY_FULL, NM_CONNECTIVITY_UNKNOWN,
	                        G_PARAM_READABLE |
	                        G_PARAM_STATIC_STRINGS));
}

