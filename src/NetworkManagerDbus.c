/* NetworkManager -- Network link manager
 *
 * Dan Williams <dcbw@redhat.com>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2004 Red Hat, Inc.
 */

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>
#include <stdarg.h>
#include <iwlib.h>


#include "NetworkManager.h"
#include "NetworkManagerUtils.h"
#include "NetworkManagerDevice.h"
#include "NetworkManagerDbus.h"
#include "NetworkManagerAP.h"
#include "NetworkManagerAPList.h"


static int test_dev_num = 0;

/*
 * nm_dbus_create_error_message
 *
 * Make a DBus error message
 *
 */
static DBusMessage *nm_dbus_create_error_message (DBusMessage *message, const char *exception_namespace,
										const char *exception, const char *format, ...)
{
	DBusMessage	*reply_message;
	va_list		 args;
	char			 error_text[512];


	va_start (args, format);
	vsnprintf (error_text, 512, format, args);
	va_end (args);

	char *exception_text = g_strdup_printf ("%s.%s", exception_namespace, exception);
	reply_message = dbus_message_new_error (message, exception_text, error_text);
	g_free (exception_text);

	return (reply_message);
}


/*
 * nm_dbus_get_object_path_from_device
 *
 * Copies the object path for a device object.  Caller must free returned string.
 *
 */
static unsigned char * nm_dbus_get_object_path_from_device (NMDevice *dev)
{
	g_return_val_if_fail (dev != NULL, NULL);

	return (g_strdup_printf ("%s/%s", NM_DBUS_PATH_DEVICES, nm_device_get_iface (dev)));
}


/*
 * nm_dbus_get_device_from_object_path
 *
 * Returns the device associated with a dbus object path
 *
 */
static NMDevice *nm_dbus_get_device_from_object_path (NMData *data, const char *path)
{
	NMDevice	*dev = NULL;

	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (data != NULL, NULL);

	/* FIXME
	 * This function could be much more efficient, for example we could
	 * actually _parse_ the object path, but that's a lot more code and
	 * stupid stuff.  The approach below is slower, less efficient, but
	 * less code and less error-prone.
	 */

	/* Iterate over device list */
	if (nm_try_acquire_mutex (data->dev_list_mutex, __FUNCTION__))
	{
		GSList	*element = data->dev_list;
		char		 compare_path[100];

		while (element)
		{
			if ((dev = (NMDevice *)(element->data)))
			{
				snprintf (compare_path, 100, "%s/%s", NM_DBUS_PATH_DEVICES, nm_device_get_iface (dev));
				/* Compare against our constructed path, but ignore any trailing elements */
				if (strncmp (path, compare_path, strlen (compare_path)) == 0)
					break;
				dev = NULL;
			}
			element = g_slist_next (element);
		}
		nm_unlock_mutex (data->dev_list_mutex, __FUNCTION__);
	}

	return (dev);
}


/*
 * nm_dbus_get_ap_from_object_path
 *
 * Returns the network (ap) associated with a dbus object path
 *
 */
static NMAccessPoint *nm_dbus_get_ap_from_object_path (const char *path, NMDevice *dev)
{
	NMAccessPoint		*ap = NULL;
	NMAccessPointList	*ap_list;
	NMAPListIter		*iter;
	char			 	 compare_path[100];

	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (dev != NULL, NULL);

	ap_list = nm_device_ap_list_get (dev);
	if (!ap_list)
		return (NULL);

	if (!(iter = nm_ap_list_iter_new (ap_list)))
		return (NULL);

	while ((ap = nm_ap_list_iter_next (iter)))
	{
		snprintf (compare_path, 100, "%s/%s/Networks/%s", NM_DBUS_PATH_DEVICES,
				nm_device_get_iface (dev), nm_ap_get_essid (ap));
		if (strncmp (path, compare_path, strlen (compare_path)) == 0)
			break;
	}
		
	nm_ap_list_iter_free (iter);
	return (ap);
}


/*
 * nm_dbus_nm_get_active_device
 *
 * Returns the object path of the currently active device
 *
 */
static DBusMessage *nm_dbus_nm_get_active_device (DBusConnection *connection, DBusMessage *message, NMData *data)
{
	DBusMessage	*reply_message = NULL;

	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (message != NULL, NULL);

	/* Construct object path of "active" device and return it */
	if (data->active_device)
	{
		char *object_path;

		reply_message = dbus_message_new_method_return (message);
		if (!reply_message)
			return (NULL);

		object_path = g_strdup_printf ("%s/%s", NM_DBUS_PATH_DEVICES, nm_device_get_iface (data->active_device));
		dbus_message_append_args (reply_message, DBUS_TYPE_STRING, object_path, DBUS_TYPE_INVALID);
		g_free (object_path);
	}
	else
	{
		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NoActiveDevice",
						"There is no currently active device.");
	}

	return (reply_message);
}


/*
 * nm_dbus_send_network_not_found
 *
 * Tell the info-daemon to alert the user that a requested network was
 * not found.
 *
 */
void nm_dbus_send_network_not_found (DBusConnection *connection, const char *network)
{
	DBusMessage		*message;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (network != NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "networkNotFound");
	if (message == NULL)
	{
		syslog (LOG_ERR, "nm_dbus_send_network_not_found(): Couldn't allocate the dbus message");
		return;
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, network, DBUS_TYPE_INVALID);
	if (!dbus_connection_send (connection, message, NULL))
		syslog (LOG_WARNING, "nm_dbus_send_network_not_found(): could not send dbus message");

	dbus_message_unref (message);
}


/*
 * nm_dbus_nm_set_active_device
 *
 * Notify the state modification handler that we want to lock to a specific
 * device.
 *
 */
static DBusMessage *nm_dbus_nm_set_active_device (DBusConnection *connection, DBusMessage *message, NMData *data)
{
	NMDevice			*dev = NULL;
	DBusMessage		*reply_message = NULL;
	char				*dev_path = NULL;
	char				*network = NULL;
	DBusError			 error;

	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (message != NULL, NULL);
	g_return_val_if_fail (data != NULL, NULL);

	/* Try to grab both device _and_ network first, and if that fails then just the device. */
	dbus_error_init (&error);
	if (!dbus_message_get_args (message, &error, DBUS_TYPE_STRING, &dev_path,
							DBUS_TYPE_STRING, &network, DBUS_TYPE_INVALID))
	{
		network = NULL;

		if (dbus_error_is_set (&error))
			dbus_error_free (&error);

		/* So if that failed, try getting just the device */
		dbus_error_init (&error);
		if (!dbus_message_get_args (message, &error, DBUS_TYPE_STRING, &dev_path, DBUS_TYPE_INVALID))
		{
			if (dbus_error_is_set (&error))
				dbus_error_free (&error);

			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "InvalidArguments",
							"NetworkManager::setActiveDevice called with invalid arguments.");
			return (reply_message);
		} else syslog (LOG_INFO, "FORCE: device '%s'", dev_path);
	} else syslog (LOG_INFO, "FORCE: device '%s', network '%s'", dev_path, network);
	
	/* So by now we have a valid device and possibly a network as well */

	dev = nm_dbus_get_device_from_object_path (data, dev_path);
	dbus_free (dev_path);
	if (!dev || (nm_device_get_driver_support_level (dev) == NM_DRIVER_UNSUPPORTED))
	{
		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceNotFound",
						"The requested network device does not exist.");
		return (reply_message);
	}
	nm_device_ref (dev);

	/* Make sure network is valid and device is wireless */
	if (nm_device_is_wireless (dev) && !network)
	{
		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "InvalidArguments",
							"NetworkManager::setActiveDevice called with invalid arguments.");
		goto out;
	}

	if (!(reply_message = dbus_message_new_method_return (message)))
		goto out;

	/* If the user specificed a wireless network too, force that as well */
	if (nm_device_is_wireless (dev) && !nm_device_find_and_use_essid (dev, network))
		nm_dbus_send_network_not_found (data->dbus_connection, network);
	else
	{
		if (nm_try_acquire_mutex (data->user_device_mutex, __FUNCTION__))
		{
			if (data->user_device)
				nm_device_unref (data->user_device);
			data->user_device = dev;
			nm_device_ref (data->user_device);
			nm_unlock_mutex (data->user_device_mutex, __FUNCTION__);
		}
		else
			nm_dbus_send_network_not_found (data->dbus_connection, network);
	}

	/* Have to mark our state changed since we blew away our connection trying out
	 * the user-requested network.
	 */
	nm_data_mark_state_changed (data);

out:
	dbus_free (network);
	nm_device_unref (dev);
	return (reply_message);
}


/*
 * nm_dbus_nm_get_devices
 *
 * Returns a string array of object paths corresponding to the
 * devices in the device list.
 *
 */
static DBusMessage *nm_dbus_nm_get_devices (DBusConnection *connection, DBusMessage *message, NMData *data)
{
	DBusMessage		*reply_message = NULL;
	DBusMessageIter	 iter;
	DBusMessageIter	 iter_array;

	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (message != NULL, NULL);

	/* Check for no devices */
	if (!data->dev_list)
		return (nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NoDevices",
					"There are no available network devices."));

	if (!(reply_message = dbus_message_new_method_return (message)))
		return (NULL);

	dbus_message_iter_init (reply_message, &iter);
	dbus_message_iter_append_array (&iter, &iter_array, DBUS_TYPE_STRING);

	/* Iterate over device list and grab index of "active device" */
	if (nm_try_acquire_mutex (data->dev_list_mutex, __FUNCTION__))
	{
		GSList	*element = data->dev_list;
		gboolean	 appended = FALSE;

		while (element)
		{
			NMDevice	*dev = (NMDevice *)(element->data);

			if (dev && (nm_device_get_driver_support_level (dev) != NM_DRIVER_UNSUPPORTED))
			{
				char *object_path = g_strdup_printf ("%s/%s", NM_DBUS_PATH_DEVICES, nm_device_get_iface (dev));
				dbus_message_iter_append_string (&iter_array, object_path);
				g_free (object_path);
				appended = TRUE;
			}
			element = g_slist_next (element);
		}

		/* If by some chance there is a device list, but it has no devices in it
		 * (something which should never happen), die.
		 */
		if (!appended)
			g_assert ("Device list existed, but no devices were in it.");

		nm_unlock_mutex (data->dev_list_mutex, __FUNCTION__);
	}
	else
	{
		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "Retry",
						"NetworkManager could not lock device list, try again.");
	}

	return (reply_message);
}


/*-------------------------------------------------------------*/
/* Handler code */
/*-------------------------------------------------------------*/


/*
 * nm_dbus_signal_device_status_change
 *
 * Notifies the bus that a particular device has had a status change, either
 * active or no longer active
 *
 */
void nm_dbus_signal_device_status_change (DBusConnection *connection, NMDevice *dev, DeviceStatus status)
{
	DBusMessage		*message;
	unsigned char		*dev_path;
	unsigned char		*signal = NULL;
	NMAccessPoint		*ap = NULL;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (dev != NULL);

	if (!(dev_path = nm_dbus_get_object_path_from_device (dev)))
		return;

	switch (status)
	{
		case (DEVICE_NO_LONGER_ACTIVE):
			signal = "DeviceNoLongerActive";
			break;
		case (DEVICE_NOW_ACTIVE):
			signal = "DeviceNowActive";
			break;
		case (DEVICE_ACTIVATING):
			signal = "DeviceActivating";
			break;
		case (DEVICE_LIST_CHANGE):
			signal = "DevicesChanged";
			break;
		case (DEVICE_ACTIVATION_FAILED):
			signal = "DeviceActivationFailed";
			break;
		default:
			syslog (LOG_ERR, "nm_dbus_signal_device_status_change(): got a bad signal name");
			return;
	}

	if (!(message = dbus_message_new_signal (NM_DBUS_PATH, NM_DBUS_INTERFACE, signal)))
	{
		syslog (LOG_ERR, "nm_dbus_signal_device_status_change(): Not enough memory for new dbus message!");
		g_free (dev_path);
		return;
	}

	if ((status == DEVICE_ACTIVATION_FAILED) && nm_device_is_wireless (dev))
		ap = nm_device_get_best_ap (dev);
	/* If the device was wireless, attach the name of the wireless network that failed to activate */
	if (ap && nm_ap_get_essid (ap))
		dbus_message_append_args (message, DBUS_TYPE_STRING, dev_path, DBUS_TYPE_STRING, nm_ap_get_essid (ap), DBUS_TYPE_INVALID);
	else
		dbus_message_append_args (message, DBUS_TYPE_STRING, dev_path, DBUS_TYPE_INVALID);
	g_free (dev_path);

	if (!dbus_connection_send (connection, message, NULL))
		syslog (LOG_WARNING, "nm_dbus_signal_device_status_change(): Could not raise the signal!");

	dbus_message_unref (message);
}


/*
 * nm_dbus_network_status_from_data
 *
 * Return a network status string based on our network data
 *
 * Caller MUST free returned value
 *
 */
static char *nm_dbus_network_status_from_data (NMData *data)
{
	char *status = NULL;

	g_return_val_if_fail (data != NULL, NULL);

	if (data->active_device && nm_device_is_activating (data->active_device))
	{
		if (nm_device_is_wireless (data->active_device) && nm_device_is_scanning (data->active_device))
			status = g_strdup ("scanning");
		else
			status = g_strdup ("connecting");
	}
	else if (data->active_device)
		status = g_strdup ("connected");
	else
		status = g_strdup ("disconnected");

	return (status);
}


/*
 * nm_dbus_signal_network_status_change
 *
 * Signal a change in general network status.
 *
 */
void nm_dbus_signal_network_status_change (DBusConnection *connection, NMData *data)
{
	DBusMessage	*message;
	char			*status = NULL;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (data != NULL);

	if (!(message = dbus_message_new_signal (NM_DBUS_PATH, NM_DBUS_INTERFACE, "NetworkStatusChange")))
	{
		syslog (LOG_ERR, "nm_dbus_signal_device_status_change(): Not enough memory for new dbus message!");
		return;
	}

	if ((status = nm_dbus_network_status_from_data (data)))
	{
		dbus_message_append_args (message, DBUS_TYPE_STRING, status, DBUS_TYPE_INVALID);

		if (!dbus_connection_send (connection, message, NULL))
			syslog (LOG_WARNING, "nm_dbus_signal_device_status_change(): Could not raise the signal!");
		g_free (status);
	}

	dbus_message_unref (message);
}


/*
 * nm_dbus_signal_device_ip4_address_change
 *
 * Notifies the bus that a particular device's IPv4 address changed.
 *
 */
void nm_dbus_signal_device_ip4_address_change (DBusConnection *connection, NMDevice *dev)
{
	DBusMessage		*message;
	unsigned char		*dev_path;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (dev != NULL);

	if (!(dev_path = nm_dbus_get_object_path_from_device (dev)))
		return;

	message = dbus_message_new_signal (NM_DBUS_PATH, NM_DBUS_INTERFACE, "DeviceIP4AddressChange");
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_signal_device_ip4_address_change(): Not enough memory for new dbus message!");
		g_free (dev_path);
		return;
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, dev_path, DBUS_TYPE_INVALID);
	g_free (dev_path);

	if (!dbus_connection_send (connection, message, NULL))
		syslog (LOG_WARNING, "nm_dbus_signal_device_ip4_address_change(): Could not raise the IP4AddressChange signal!");

	dbus_message_unref (message);
}


/*
 * nm_dbus_signal_wireless_network_change
 *
 * Notifies the bus that a new wireless network has come into range
 *
 */
void nm_dbus_signal_wireless_network_change (DBusConnection *connection, NMDevice *dev, NMAccessPoint *ap, gboolean gone)
{
	DBusMessage	*message;
	char			*dev_path;
	char			*ap_path;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (dev != NULL);
	g_return_if_fail (ap != NULL);

	if (!(dev_path = nm_dbus_get_object_path_from_device (dev)))
		return;

	if (!(ap_path = nm_device_get_path_for_ap (dev, ap)))
	{
		g_free (dev_path);
		return;
	}

	message = dbus_message_new_signal (NM_DBUS_PATH, NM_DBUS_INTERFACE,
								(gone ? "WirelessNetworkDisappeared" : "WirelessNetworkAppeared"));
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_signal_wireless_network_appeared(): Not enough memory for new dbus message!");
		g_free (dev_path);
		g_free (ap_path);
		return;
	}

	dbus_message_append_args (message,
							DBUS_TYPE_STRING, dev_path,
							DBUS_TYPE_STRING, ap_path,
							DBUS_TYPE_INVALID);
	g_free (ap_path);
	g_free (dev_path);

	if (!dbus_connection_send (connection, message, NULL))
		syslog (LOG_WARNING, "nnm_dbus_signal_wireless_network_appeared(): Could not raise the WirelessNetworkAppeared signal!");

	dbus_message_unref (message);
}


/*
 * nm_dbus_get_user_key_for_network
 *
 * Asks NetworkManagerInfo for a user-entered WEP key.
 *
 */
void nm_dbus_get_user_key_for_network (DBusConnection *connection, NMDevice *dev, NMAccessPoint *ap)
{
	DBusMessage		*message;

	g_return_if_fail (connection != NULL);
	g_return_if_fail (dev != NULL);
	g_return_if_fail (ap != NULL);
	g_return_if_fail (nm_ap_get_essid (ap) != NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "getKeyForNetwork");
	if (message == NULL)
	{
		syslog (LOG_ERR, "nm_dbus_get_user_key_for_network(): Couldn't allocate the dbus message");
		return;
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, nm_device_get_iface (dev),
								DBUS_TYPE_STRING, nm_ap_get_essid (ap),
								DBUS_TYPE_INVALID);

	if (!dbus_connection_send (connection, message, NULL))
		syslog (LOG_WARNING, "nm_dbus_get_user_key_for_network(): could not send dbus message");

	dbus_message_unref (message);
}


/*
 * nm_dbus_set_user_key_for_network
 *
 * In response to a NetworkManagerInfo message, sets the WEP key
 * for a particular wireless AP/network
 *
 */
static void nm_dbus_set_user_key_for_network (DBusConnection *connection, DBusMessage *message, NMData *data)
{
	DBusError		 error;
	char			*device;
	char			*network;
	char			*passphrase;
	NMEncKeyType	 key_type;

	g_return_if_fail (data != NULL);
	g_return_if_fail (connection != NULL);
	g_return_if_fail (message != NULL);

	dbus_error_init (&error);
	if (dbus_message_get_args (message, &error,
							DBUS_TYPE_STRING, &device,
							DBUS_TYPE_STRING, &network,
							DBUS_TYPE_STRING, &passphrase,
							DBUS_TYPE_INT32, &key_type,
							DBUS_TYPE_INVALID))
	{
		NMDevice		*dev;

		if ((dev = nm_get_device_by_iface (data, device)))
			nm_device_set_user_key_for_network (dev, data->invalid_ap_list, network, passphrase, key_type);

		dbus_free (device);
		dbus_free (network);
		dbus_free (passphrase);
	}
}

/*
 * nm_dbus_cancel_get_user_key_for_network
 *
 * Sends a user-key cancellation message to NetworkManagerInfo
 *
 */
void nm_dbus_cancel_get_user_key_for_network (DBusConnection *connection)
{
	DBusMessage		*message;

	g_return_if_fail (connection != NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "cancelGetKeyForNetwork");
	if (message == NULL)
	{
		syslog (LOG_ERR, "nm_dbus_cancel_get_user_key_for_network(): Couldn't allocate the dbus message");
		return;
	}

	if (!dbus_connection_send (connection, message, NULL))
		syslog (LOG_WARNING, "nm_dbus_cancel_get_user_key_for_network(): could not send dbus message");

	dbus_message_unref (message);
}


/*
 * nm_dbus_get_network_essid
 *
 * Get a network's essid from NetworkManagerInfo
 *
 * NOTE: caller MUST free returned value
 *
 */
char * nm_dbus_get_network_essid (DBusConnection *connection, NMNetworkType type, const char *network)
{
	DBusMessage		*message;
	DBusError			 error;
	DBusMessage		*reply;
	char				*essid = NULL;

	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (network != NULL, NULL);
	g_return_val_if_fail (type != NETWORK_TYPE_UNKNOWN, NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "getNetworkEssid");
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_get_network_essid(): Couldn't allocate the dbus message");
		return (NULL);
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, network,
								DBUS_TYPE_INT32, (int)type,
								DBUS_TYPE_INVALID);

	/* Send message and get essid back from NetworkManagerInfo */
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &error);
	if (dbus_error_is_set (&error))
		syslog (LOG_ERR, "nm_dbus_get_network_essid(): %s raised %s", error.name, error.message);
	else if (!reply)
		syslog (LOG_NOTICE, "nm_dbus_get_network_essid(): reply was NULL.");
	else
	{
		char	*dbus_string;

		dbus_error_init (&error);
		if (dbus_message_get_args (reply, &error, DBUS_TYPE_STRING, &dbus_string, DBUS_TYPE_INVALID))
		{
			essid = (dbus_string == NULL ? NULL : strdup (dbus_string));
			dbus_free (dbus_string);
		}
	}

	dbus_message_unref (message);
	if (reply)
		dbus_message_unref (reply);

	return (essid);
}


/*
 * nm_dbus_get_network_key
 *
 * Get a network's key and key type from NetworkManagerInfo.
 *
 * NOTE: caller MUST free returned value
 *
 */
char * nm_dbus_get_network_key (DBusConnection *connection, NMNetworkType type, const char *network, NMEncKeyType *enc_method)
{
	DBusMessage		*message;
	DBusError			 error;
	DBusMessage		*reply;
	char				*key = NULL;

	g_return_val_if_fail (enc_method != NULL, NULL);
	*enc_method = NM_ENC_TYPE_UNKNOWN;

	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (network != NULL, NULL);
	g_return_val_if_fail (type != NETWORK_TYPE_UNKNOWN, NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "getNetworkKey");
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_get_network_key(): Couldn't allocate the dbus message");
		return (NULL);
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, network,
								DBUS_TYPE_INT32, (int)type,
								DBUS_TYPE_INVALID);

	/* Send message and get key back from NetworkManagerInfo */
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &error);
	dbus_message_unref (message);
	if (dbus_error_is_set (&error))
	{
		syslog (LOG_ERR, "nm_dbus_get_network_key(): %s raised %s", error.name, error.message);
		dbus_error_free (&error);
	}
	else if (!reply)
		syslog (LOG_NOTICE, "nm_dbus_get_network_key(): reply was NULL.");
	else
	{
		char			*dbus_key;

		dbus_error_init (&error);
		if (dbus_message_get_args (reply, &error, DBUS_TYPE_STRING, &dbus_key, DBUS_TYPE_INT32, enc_method, DBUS_TYPE_INVALID))
		{
			key = (dbus_key == NULL ? NULL : strdup (dbus_key));
			dbus_free (dbus_key);
		}
		else
			*enc_method = NM_ENC_TYPE_UNKNOWN;
		if (dbus_error_is_set (&error))
			dbus_error_free (&error);

		dbus_message_unref (reply);
	}

	return (key);
}


/*
 * nm_dbus_get_network_timestamp
 *
 * Get a network's timestamp from NetworkManagerInfo
 *
 * Returns:	NULL on error
 *			timestamp if no error
 *
 */
GTimeVal *nm_dbus_get_network_timestamp (DBusConnection *connection, NMNetworkType type, const char *network)
{
	DBusMessage		*message;
	DBusError			 error;
	DBusMessage		*reply;
	guint32			timestamp_secs;
	GTimeVal		*timestamp;

	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (network != NULL, NULL);
	g_return_val_if_fail (type != NETWORK_TYPE_UNKNOWN, NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "getNetworkTimestamp");
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_get_network_timestamp(): Couldn't allocate the dbus message");
		return NULL;
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, network,
								DBUS_TYPE_INT32, (int)type,
								DBUS_TYPE_INVALID);

	/* Send message and get timestamp back from NetworkManagerInfo */
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &error);
	if (dbus_error_is_set (&error))
		syslog (LOG_ERR, "nm_dbus_get_network_timestamp(): %s raised %s", error.name, error.message);
	else if (!reply)
		syslog (LOG_NOTICE, "nm_dbus_get_network_timestamp(): reply was NULL.");
	else
	{
		dbus_error_init (&error);
		if (!dbus_message_get_args (reply, &error, DBUS_TYPE_INT32, &timestamp_secs, DBUS_TYPE_INVALID))
			timestamp_secs = -1;
	}

	dbus_message_unref (message);
	if (reply)
		dbus_message_unref (reply);

	if (timestamp_secs < 0)
		return NULL;
	timestamp = g_new0 (GTimeVal, 1);
	timestamp->tv_sec = timestamp_secs;
	timestamp->tv_usec = 0;

	return (timestamp);
}


/*
 * nm_dbus_get_network_trusted
 *
 * Get whether or not a network is a "trusted" network from NetworkManagerInfo
 *
 * Returns:	FALSE on error or if network is not trusted
 *			TRUE if the network is trusted
 *
 */
gboolean nm_dbus_get_network_trusted (DBusConnection *connection, NMNetworkType type, const char *network)
{
	DBusMessage		*message;
	DBusError			 error;
	DBusMessage		*reply;
	gboolean			 trusted = FALSE;

	g_return_val_if_fail (connection != NULL, FALSE);
	g_return_val_if_fail (network != NULL, FALSE);
	g_return_val_if_fail (type != NETWORK_TYPE_UNKNOWN, FALSE);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "getNetworkTrusted");
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_get_network_trusted(): Couldn't allocate the dbus message");
		return (FALSE);
	}

	dbus_message_append_args (message, DBUS_TYPE_STRING, network,
								DBUS_TYPE_INT32, (int)type,
								DBUS_TYPE_INVALID);

	/* Send message and get trusted status back from NetworkManagerInfo */
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &error);
	if (dbus_error_is_set (&error))
	{
		syslog (LOG_ERR, "nm_dbus_get_network_trusted(): %s raised %s", error.name, error.message);
		dbus_error_free (&error);
	}
	else if (!reply)
		syslog (LOG_NOTICE, "nm_dbus_get_network_trusted(): reply was NULL.");
	else
	{
		dbus_error_init (&error);
		dbus_message_get_args (reply, &error, DBUS_TYPE_BOOLEAN, &trusted, DBUS_TYPE_INVALID);
		if (dbus_error_is_set (&error))
			dbus_error_free (&error);
	}

	dbus_message_unref (message);
	if (reply)
		dbus_message_unref (reply);

	return (trusted);
}


/*
 * nm_dbus_get_networks
 *
 * Get all networks of a specific type from NetworkManagerInfo
 *
 * NOTE: caller MUST free returned value using dbus_free_string_array()
 *
 */
char ** nm_dbus_get_networks (DBusConnection *connection, NMNetworkType type, int *num_networks)
{
	DBusMessage		*message;
	DBusError			 error;
	DBusMessage		*reply;
	char			    **networks = NULL;

	*num_networks = 0;
	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (type != NETWORK_TYPE_UNKNOWN, NULL);

	message = dbus_message_new_method_call (NMI_DBUS_SERVICE, NMI_DBUS_PATH,
						NMI_DBUS_INTERFACE, "getNetworks");
	if (!message)
	{
		syslog (LOG_ERR, "nm_dbus_get_networks(): Couldn't allocate the dbus message");
		return (NULL);
	}

	dbus_message_append_args (message, DBUS_TYPE_INT32, (int)type, DBUS_TYPE_INVALID);

	/* Send message and get essid back from NetworkManagerInfo */
	dbus_error_init (&error);
	reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &error);
	if (dbus_error_is_set (&error))
		syslog (LOG_ERR, "nm_dbus_get_networks(): %s raised %s", error.name, error.message);
	else if (!reply)
		syslog (LOG_NOTICE, "nm_dbus_get_networks(): reply was NULL.");
	else
	{
		DBusMessageIter	 iter;

		dbus_message_iter_init (reply, &iter);
		dbus_message_iter_get_string_array (&iter, &networks, num_networks);
	}

	dbus_message_unref (message);
	if (reply)
		dbus_message_unref (reply);

	return (networks);
}


/*
 * nm_dbus_nmi_filter
 *
 * Respond to NetworkManagerInfo signals about changing Allowed Networks
 *
 */
static DBusHandlerResult nm_dbus_nmi_filter (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	NMData			*data = (NMData *)user_data;
	const char		*object_path;
	const char		*method;
	gboolean			 handled = FALSE;

	g_return_val_if_fail (data != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
	g_return_val_if_fail (connection != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
	g_return_val_if_fail (message != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

	method = dbus_message_get_member (message);
	if (!(object_path = dbus_message_get_path (message)))
		return (DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

	/* syslog (LOG_DEBUG, "nm_dbus_nmi_filter() got method %s for path %s", method, object_path); */

	if (    (strcmp (object_path, NMI_DBUS_PATH) == 0)
		&& dbus_message_is_signal (message, NMI_DBUS_INTERFACE, "WirelessNetworkUpdate"))
	{
		char			*network = NULL;
		DBusError		 error;

		dbus_error_init (&error);
		if (!dbus_message_get_args (message, &error, DBUS_TYPE_STRING, &network, DBUS_TYPE_INVALID))
			return (DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

		syslog (LOG_DEBUG, "NetowrkManagerInfo triggered update of wireless network '%s'", network);
		nm_ap_list_update_network (data->allowed_ap_list, network, data);
		dbus_free (network);
		handled = TRUE;
	}
	else if (dbus_message_is_signal (message, DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS, "ServiceCreated"))
	{
		char 	*service;
		DBusError	 error;

		dbus_error_init (&error);
		if (    dbus_message_get_args (message, &error, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID)
			&& (strcmp (service, NMI_DBUS_SERVICE) == 0))
		{
			data->update_ap_lists = TRUE;
			data->info_daemon_avail = TRUE;
			data->notify_device_support = TRUE;
			nm_data_mark_state_changed (data);
		}
		/* Don't set handled = TRUE since other filter functions on this dbus connection
		 * may want to know about service signals.
		 */
	}
	else if (dbus_message_is_signal (message, DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS, "ServiceDeleted"))
	{
		char 	*service;
		DBusError	 error;

		dbus_error_init (&error);
		if (    dbus_message_get_args (message, &error, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID)
			&& (strcmp (service, NMI_DBUS_SERVICE) == 0))
		{
			data->update_ap_lists = TRUE;
			data->info_daemon_avail = FALSE;
			data->notify_device_support = TRUE;
			nm_data_mark_state_changed (data);
		}
		/* Don't set handled = TRUE since other filter functions on this dbus connection
		 * may want to know about service signals.
		 */
	}

	return (handled ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
}


/*
 * nm_dbus_devices_handle_networks_request
 *
 * Converts a property request on a _network_ into a dbus message.
 *
 */
static DBusMessage *nm_dbus_devices_handle_networks_request (DBusConnection *connection, DBusMessage *message,
									NMData *data, const char *path, const char *request, NMDevice *dev)
{
	NMAccessPoint		*ap;
	DBusMessage		*reply_message = NULL;

	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (message != NULL, NULL);
	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (request != NULL, NULL);
	g_return_val_if_fail (dev != NULL, NULL);

	if (!(ap = nm_dbus_get_ap_from_object_path (path, dev)))
	{
		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NetworkNotFound",
						"The requested network does not exist for this device.");
		return (reply_message);
	}

	if (!(reply_message = dbus_message_new_method_return (message)))
		return (NULL);

	if (strcmp ("getName", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_STRING, nm_ap_get_essid (ap), DBUS_TYPE_INVALID);
	else if (strcmp ("getAddress", request) == 0)
	{
		char		buf[20];

		memset (&buf[0], 0, 20);
		iw_ether_ntop((const struct ether_addr *) (nm_ap_get_address (ap)), &buf[0]);
		dbus_message_append_args (reply_message, DBUS_TYPE_STRING, &buf[0], DBUS_TYPE_INVALID);
	}
	else if (strcmp ("getStrength", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_INT32, nm_ap_get_strength (ap), DBUS_TYPE_INVALID);
	else if (strcmp ("getFrequency", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_DOUBLE, nm_ap_get_freq (ap), DBUS_TYPE_INVALID);
	else if (strcmp ("getRate", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_INT32, nm_ap_get_rate (ap), DBUS_TYPE_INVALID);
	else if (strcmp ("getEncrypted", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_BOOLEAN, nm_ap_get_encrypted (ap), DBUS_TYPE_INVALID);
	else
	{
		/* Must destroy the allocated message */
		dbus_message_unref (reply_message);

		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "UnknownMethod",
							"NetworkManager knows nothing about the method %s for object %s", request, path);
	}

	return (reply_message);
}


/*
 * nm_dbus_devices_handle_request
 *
 * Converts a property request into a dbus message.
 *
 */
static DBusMessage *nm_dbus_devices_handle_request (DBusConnection *connection, NMData *data, DBusMessage *message,
											const char *path, const char *request)
{
	NMDevice			*dev;
	DBusMessage		*reply_message = NULL;
	char				*object_path;

	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (message != NULL, NULL);
	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (request != NULL, NULL);

	if (!(dev = nm_dbus_get_device_from_object_path (data, path)))
	{
		reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceNotFound",
						"The requested network device does not exist.");
		return (reply_message);
	}

	/* Test whether or not the _networks_ of a device were queried instead of the device itself */
	object_path = g_strdup_printf ("%s/%s/Networks/", NM_DBUS_PATH_DEVICES, nm_device_get_iface (dev));
	if (strncmp (path, object_path, strlen (object_path)) == 0)
	{
		free (object_path);
		reply_message = nm_dbus_devices_handle_networks_request (connection, message, data, path, request, dev);
		return (reply_message);
	}
	free (object_path);

	if (!(reply_message = dbus_message_new_method_return (message)))
		return (NULL);

	if (strcmp ("getName", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_STRING, nm_device_get_iface (dev), DBUS_TYPE_INVALID);
	else if (strcmp ("getType", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_INT32, nm_device_get_type (dev), DBUS_TYPE_INVALID);
	else if (strcmp ("getHalUdi", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_STRING, nm_device_get_udi (dev), DBUS_TYPE_INVALID);
	else if (strcmp ("getIP4Address", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_UINT32, nm_device_get_ip4_address (dev), DBUS_TYPE_INVALID);
	else if (strcmp ("getStrength", request) == 0)
	{
		/* Only wireless devices have signal strength */
		if (!nm_device_is_wireless (dev))
		{
			dbus_message_unref (reply_message);
			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceNotWireless",
					"Wired devices cannot have signal strength.");
			return (reply_message);
		}

		dbus_message_append_args (reply_message, DBUS_TYPE_INT32, nm_device_get_signal_strength (dev), DBUS_TYPE_INVALID);
	}
	else if (strcmp ("getActiveNetwork", request) == 0)
	{
		NMAccessPoint	*ap;
		gboolean		 success = FALSE;

		/* Only wireless devices have an active network */
		if (!nm_device_is_wireless (dev))
		{
			dbus_message_unref (reply_message);
			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceNotWireless",
					"Wired devices cannot have active networks.");
			return (reply_message);
		}

		/* Return the network associated with the ESSID the card is currently associated with,
		 * if any, and only if that network is the "best" network.
		 */
		if (    (ap = nm_device_ap_list_get_ap_by_essid (dev, nm_device_get_essid (dev)))
			&& (!nm_device_need_ap_switch (dev))
			&& (object_path = nm_device_get_path_for_ap (dev, ap)))
		{
			dbus_message_append_args (reply_message, DBUS_TYPE_STRING, object_path, DBUS_TYPE_INVALID);
			g_free (object_path);
			success = TRUE;
		}

		if (!success)
		{
			dbus_message_unref (reply_message);
			return (nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NoActiveNetwork",
					"The device is not associated with any networks at this time."));
		}
	}
	else if (strcmp ("getNetworks", request) == 0)
	{
		DBusMessageIter	 iter;
		DBusMessageIter	 iter_array;
		NMAccessPoint		*ap = NULL;
		gboolean			 success = FALSE;
		NMAccessPointList	*ap_list;
		NMAPListIter		*list_iter;
	
		/* Only wireless devices have networks */
		if (!nm_device_is_wireless (dev))
		{
			dbus_message_unref (reply_message);
			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceNotWireless",
					"Wired devices cannot see wireless networks.");
			return (reply_message);
		}

		dbus_message_iter_init (reply_message, &iter);
		dbus_message_iter_append_array (&iter, &iter_array, DBUS_TYPE_STRING);
		
		if ((ap_list = nm_device_ap_list_get (dev)))
		{
			if ((list_iter = nm_ap_list_iter_new (ap_list)))
			{
				while ((ap = nm_ap_list_iter_next (list_iter)))
				{
					if (nm_ap_get_essid (ap))
					{
						object_path = g_strdup_printf ("%s/%s/Networks/%s", NM_DBUS_PATH_DEVICES,
								nm_device_get_iface (dev), nm_ap_get_essid (ap));
						dbus_message_iter_append_string (&iter_array, object_path);
						g_free (object_path);
						success = TRUE;
					}
				}
				nm_ap_list_iter_free (list_iter);
			}
		}

		if (!success)
		{
			dbus_message_unref (reply_message);
			return (nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NoNetworks",
					"The device cannot see any wireless networks."));
		}
	}
	else if (strcmp ("getLinkActive", request) == 0)
		dbus_message_append_args (reply_message, DBUS_TYPE_BOOLEAN, nm_device_get_link_active (dev), DBUS_TYPE_INVALID);
	else if (strcmp ("setLinkActive", request) == 0)
	{
		/* Can only set link status for active devices */
		if (nm_device_is_test_device (dev))
		{
			DBusError	error;
			gboolean	link;

			dbus_error_init (&error);
			if (dbus_message_get_args (message, &error, DBUS_TYPE_BOOLEAN, &link, DBUS_TYPE_INVALID))
			{
				nm_device_set_link_active (dev, link);
				nm_data_mark_state_changed (data);
			}
		}
		else
			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NotTestDevice",
						"Only test devices can have their link status set manually.");
	}
	else
	{
		/* Must destroy the allocated message */
		dbus_message_unref (reply_message);
		reply_message = NULL;
	}

	return (reply_message);
}


/*
 * nm_dbus_nm_message_handler
 *
 * Dispatch messages against our NetworkManager object
 *
 */
static DBusHandlerResult nm_dbus_nm_message_handler (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	NMData			*data = (NMData *)user_data;
	const char		*method;
	const char		*path;
	DBusMessage		*reply_message = NULL;
	gboolean			 handled = TRUE;

	g_return_val_if_fail (data != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
	g_return_val_if_fail (connection != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
	g_return_val_if_fail (message != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

	method = dbus_message_get_member (message);
	path = dbus_message_get_path (message);

	/* syslog (LOG_DEBUG, "nm_dbus_nm_message_handler() got method %s for path %s", method, path); */

	if (strcmp ("getActiveDevice", method) == 0)
		reply_message = nm_dbus_nm_get_active_device (connection, message, data);
	else if (strcmp ("getDevices", method) == 0)
		reply_message = nm_dbus_nm_get_devices (connection, message, data);
	else if (strcmp ("setActiveDevice", method) == 0)
		nm_dbus_nm_set_active_device (connection, message, data);
	else if (strcmp ("setKeyForNetwork", method) == 0)
		nm_dbus_set_user_key_for_network (connection, message, data);
	else if (strcmp ("status", method) == 0)
	{
		char *status = nm_dbus_network_status_from_data (data);
		if (status && (reply_message = dbus_message_new_method_return (message)))
				dbus_message_append_args (reply_message, DBUS_TYPE_STRING, status, DBUS_TYPE_INVALID);
		g_free (status);
	}
	else if (strcmp ("createTestDevice", method) == 0)
	{
		DBusError		error;
		NMDeviceType	type;

		dbus_error_init (&error);
		if (    dbus_message_get_args (message, &error, DBUS_TYPE_INT32, &type, DBUS_TYPE_INVALID)
			&& ((type == DEVICE_TYPE_WIRED_ETHERNET) || (type == DEVICE_TYPE_WIRELESS_ETHERNET)))
		{
			char		*interface = g_strdup_printf ("test%d", test_dev_num);
			char		*udi = g_strdup_printf ("/test-devices/%s", interface);
			NMDevice	*dev = NULL;

			dev = nm_create_device_and_add_to_list (data, udi, interface, TRUE, type);
			test_dev_num++;
			if ((reply_message = dbus_message_new_method_return (message)))
			{
				char		*dev_path = g_strdup_printf ("%s/%s", NM_DBUS_PATH_DEVICES, nm_device_get_iface (dev));
				dbus_message_append_args (reply_message, DBUS_TYPE_STRING, dev_path, DBUS_TYPE_INVALID);
				g_free (dev_path);
			}
			g_free (interface);
			g_free (udi);
		}
		else
			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "BadType",
						"The test device type was invalid.");
	}
	else if (strcmp ("removeTestDevice", method) == 0)
	{
		DBusError		 error;
		char			*dev_path;

		dbus_error_init (&error);
		if (dbus_message_get_args (message, &error, DBUS_TYPE_STRING, &dev_path, DBUS_TYPE_INVALID))
		{
			NMDevice	*dev;

			if ((dev = nm_dbus_get_device_from_object_path (data, dev_path)))
			{
				if (nm_device_is_test_device (dev))
					nm_remove_device_from_list (data, nm_device_get_udi (dev));
				else
					reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "NotTestDevice",
								"Only test devices can be removed via dbus calls.");
			}
			else
			{
				reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceNotFound",
								"The requested network device does not exist.");
			}
		}
		else
		{
			reply_message = nm_dbus_create_error_message (message, NM_DBUS_INTERFACE, "DeviceBad",
						"The device ID was bad.");
		}
	}
	else
		handled = FALSE;

	if (reply_message)
	{
		dbus_connection_send (connection, reply_message, NULL);
		dbus_message_unref (reply_message);
	}

	return (handled ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
}


/*
 * nm_dbus_nm_unregister_handler
 *
 * Nothing happens here.
 *
 */
void nm_dbus_nm_unregister_handler (DBusConnection *connection, void *user_data)
{
	/* do nothing */
}


/*
 * nm_dbus_devices_message_handler
 *
 * Dispatch messages against individual network devices
 *
 */
static DBusHandlerResult nm_dbus_devices_message_handler (DBusConnection *connection, DBusMessage *message, void *user_data)
{
	NMData			*data = (NMData *)user_data;
	gboolean			 handled = FALSE;
	const char		*method;
	const char		*path;
	DBusMessage		*reply_message = NULL;

	g_return_val_if_fail (data != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
	g_return_val_if_fail (connection != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
	g_return_val_if_fail (message != NULL, DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

	method = dbus_message_get_member (message);
	path = dbus_message_get_path (message);

	/*syslog (LOG_DEBUG, "nm_dbus_devices_message_handler() got method %s for path %s", method, path);*/

	if (method && path && (reply_message = nm_dbus_devices_handle_request (connection, data, message, path, method)))
	{
		dbus_connection_send (connection, reply_message, NULL);
		dbus_message_unref (reply_message);
		handled = TRUE;
	}

	return (handled ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
}


/*
 * nm_dbus_devices_unregister_handler
 *
 * Nothing happens here.
 *
 */
void nm_dbus_devices_unregister_handler (DBusConnection *connection, void *user_data)
{
	/* do nothing */
}


/*
 * nm_dbus_is_info_daemon_running
 *
 * Ask dbus whether or not the info daemon is providing its dbus service
 *
 */
gboolean nm_dbus_is_info_daemon_running (DBusConnection *connection)
{
	DBusError		error;
	gboolean		running = FALSE;

	g_return_val_if_fail (connection != NULL, FALSE);

	dbus_error_init (&error);
	running = dbus_bus_service_exists (connection, NMI_DBUS_SERVICE, &error);
	if (dbus_error_is_set (&error))
		dbus_error_free (&error);
	return (running);
}


/*
 * nm_dbus_init
 *
 * Connect to the system messagebus and register ourselves as a service.
 *
 */
DBusConnection *nm_dbus_init (NMData *data)
{
	DBusError		 		 dbus_error;
	dbus_bool_t			 success;
	DBusConnection			*connection;
	DBusObjectPathVTable	 nm_vtable = { &nm_dbus_nm_unregister_handler, &nm_dbus_nm_message_handler, NULL, NULL, NULL, NULL };
	DBusObjectPathVTable	 devices_vtable = { &nm_dbus_devices_unregister_handler, &nm_dbus_devices_message_handler, NULL, NULL, NULL, NULL };

	dbus_connection_set_change_sigpipe (TRUE);

	dbus_error_init (&dbus_error);
	connection = dbus_bus_get (DBUS_BUS_SYSTEM, &dbus_error);
	if ((connection == NULL) || dbus_error_is_set (&dbus_error))
	{
		syslog (LOG_ERR, "nm_dbus_init() could not get the system bus.  Make sure the message bus daemon is running?");
		if (dbus_error_is_set (&dbus_error))
			dbus_error_free (&dbus_error);
		return (NULL);
	}

	dbus_connection_set_exit_on_disconnect (connection, FALSE);
	dbus_connection_setup_with_g_main (connection, NULL);

	success = dbus_connection_register_object_path (connection, NM_DBUS_PATH, &nm_vtable, data);
	if (!success)
	{
		syslog (LOG_CRIT, "nm_dbus_init() could not register a handler for NetworkManager.  Not enough memory?");
		return (NULL);
	}

	success = dbus_connection_register_fallback (connection, NM_DBUS_PATH_DEVICES, &devices_vtable, data);
	if (!success)
	{
		syslog (LOG_CRIT, "nm_dbus_init() could not register a handler for NetworkManager devices.  Not enough memory?");
		return (NULL);
	}

	if (!dbus_connection_add_filter (connection, nm_dbus_nmi_filter, data, NULL))
	{
		syslog (LOG_CRIT, "nm_dbus_init() could not attach a dbus message filter.  The NetworkManager dbus security policy may not be loaded.  Restart dbus?");
		return (NULL);
	}

	dbus_bus_add_match (connection,
				"type='signal',"
				"interface='" NMI_DBUS_INTERFACE "',"
				"sender='" NMI_DBUS_SERVICE "',"
				"path='" NMI_DBUS_PATH "'",
				&dbus_error);
	dbus_error_free (&dbus_error);

	dbus_bus_add_match(connection,
				"type='signal',"
				"interface='" DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS "',"
				"sender='" DBUS_SERVICE_ORG_FREEDESKTOP_DBUS "'",
				&dbus_error);
	dbus_error_free (&dbus_error);

	dbus_bus_acquire_service (connection, NM_DBUS_SERVICE, 0, &dbus_error);
	if (dbus_error_is_set (&dbus_error))
	{
		syslog (LOG_ERR, "nm_dbus_init() could not acquire its service.  dbus_bus_acquire_service() says: '%s'", dbus_error.message);
		dbus_error_free (&dbus_error);
		return (NULL);
	}

	return (connection);
}
