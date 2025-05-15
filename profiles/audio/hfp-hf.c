// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *  Copyright © 2025 Collabora Ltd.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

#include <stdint.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "bluetooth/sdp.h"
#include "bluetooth/sdp_lib.h"
#include "lib/uuid.h"

#include "gdbus/gdbus.h"

#include "btio/btio.h"
#include "src/adapter.h"
#include "src/btd.h"
#include "src/dbus-common.h"
#include "src/device.h"
#include "src/error.h"
#include "src/log.h"
#include "src/plugin.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/shared/hfp.h"

#define TELEPHONY_AG_INTERFACE "org.bluez.TelephonyAg1"
#define TELEPHONY_CALL_INTERFACE "org.bluez.TelephonyCall1"

#define HAL_HF_CLIENT_CALL_IND_NO_CALL_IN_PROGRESS	0x00
#define HAL_HF_CLIENT_CALL_IND_CALL_IN_PROGRESS		0x01

#define HAL_HF_CLIENT_CHLD_FEAT_REL		0x00000001
#define HAL_HF_CLIENT_CHLD_FEAT_REL_ACC		0x00000002
#define HAL_HF_CLIENT_CHLD_FEAT_REL_X		0x00000004
#define HAL_HF_CLIENT_CHLD_FEAT_HOLD_ACC	0x00000008
#define HAL_HF_CLIENT_CHLD_FEAT_PRIV_X		0x00000010
#define HAL_HF_CLIENT_CHLD_FEAT_MERGE		0x00000020
#define HAL_HF_CLIENT_CHLD_FEAT_MERGE_DETACH	0x00000040

#define HFP_HF_FEAT_ECNR	0x00000001
#define HFP_HF_FEAT_3WAY	0x00000002
#define HFP_HF_FEAT_CLI		0x00000004
#define HFP_HF_FEAT_VR		0x00000008
#define HFP_HF_FEAT_RVC		0x00000010
#define HFP_HF_FEAT_ECS		0x00000020
#define HFP_HF_FEAT_ECC		0x00000040
#define HFP_HF_FEAT_CODEC	0x00000080
#define HFP_HF_FEAT_HF_IND	0x00000100
#define HFP_HF_FEAT_ESCO_S4_T2	0x00000200

#define HFP_AG_FEAT_3WAY	0x00000001
#define HFP_AG_FEAT_ECNR	0x00000002
#define HFP_AG_FEAT_VR		0x00000004
#define HFP_AG_FEAT_INBAND	0x00000008
#define HFP_AG_FEAT_VTAG	0x00000010
#define HFP_AG_FEAT_REJ_CALL	0x00000020
#define HFP_AG_FEAT_ECS		0x00000040
#define HFP_AG_FEAT_ECC		0x00000080
#define HFP_AG_FEAT_EXT_ERR	0x00000100
#define HFP_AG_FEAT_CODEC	0x00000200

#define HFP_HF_FEATURES (HFP_HF_FEAT_ECNR | HFP_HF_FEAT_3WAY |\
	HFP_HF_FEAT_CLI | HFP_HF_FEAT_VR |\
	HFP_HF_FEAT_RVC | HFP_HF_FEAT_ECS |\
	HFP_HF_FEAT_ECC)

enum hfp_indicator {
	HFP_INDICATOR_SERVICE = 0,
	HFP_INDICATOR_CALL,
	HFP_INDICATOR_CALLSETUP,
	HFP_INDICATOR_CALLHELD,
	HFP_INDICATOR_SIGNAL,
	HFP_INDICATOR_ROAM,
	HFP_INDICATOR_BATTCHG,
	HFP_INDICATOR_LAST
};

enum connection_state {
	CONNECTING = 0,
	SLC_CONNECTING,
	CONNECTED,
	DISCONNECTING
};

enum call_state {
	CALL_STATE_ACTIVE = 0,
	CALL_STATE_HELD,
	CALL_STATE_DIALING,
	CALL_STATE_ALERTING,
	CALL_STATE_INCOMING,
	CALL_STATE_WAITING,
	CALL_STATE_DISCONNECTED,
};

enum call_setup {
	CIND_CALLSETUP_NONE = 0,
	CIND_CALLSETUP_INCOMING,
	CIND_CALLSETUP_DIALING,
	CIND_CALLSETUP_ALERTING
};

enum call_held {
	CIND_CALLHELD_NONE = 0,
	CIND_CALLHELD_HOLD_AND_ACTIVE,
	CIND_CALLHELD_HOLD
};

typedef void (*ciev_func_t)(uint8_t val, void *user_data);

struct indicator {
	uint8_t index;
	uint32_t min;
	uint32_t max;
	uint32_t val;
	ciev_func_t cb;
};

struct hfp_ag_device;

struct call {
	struct hfp_ag_device	*device;
	char			*path;
	uint8_t			idx;

	char			*line_id;
	char			*incoming_line;
	char			*name;
	bool			multiparty;
	enum call_state		state;

	DBusMessage		*pending_msg;
};

struct hfp_ag_device {
	struct btd_service	*service;
	struct btd_device	*device;
	char			*path;
	bdaddr_t		src;
	bdaddr_t		dst;
	uint16_t		version;
	GIOChannel		*io;
	enum connection_state	state;
	uint32_t		hfp_hf_features;
	uint32_t		features;
	struct hfp_hf		*hf;
	struct indicator	ag_ind[HFP_INDICATOR_LAST];
	uint32_t		chld_features;
	bool			network_service;
	uint8_t			signal;
	bool			roaming;
	uint8_t			battchg;
	bool			call;
	enum call_setup		call_setup;
	enum call_held		call_held;
	uint8_t			id;
	GSList			*calls;
};

static struct hfp_ag_device *hfp_ag_device_new(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	const char *path = device_get_path(device);
	struct btd_adapter *adapter = device_get_adapter(device);
	struct hfp_ag_device *hf_ag_dev;

	hf_ag_dev = g_new0(struct hfp_ag_device, 1);
	bacpy(&hf_ag_dev->src, btd_adapter_get_address(adapter));
	bacpy(&hf_ag_dev->dst, device_get_address(device));
	hf_ag_dev->service = btd_service_ref(service);
	hf_ag_dev->device = btd_device_ref(device);
	hf_ag_dev->path = g_strdup_printf("%s/hfp", path);

	return hf_ag_dev;
}

static struct call *call_new(struct hfp_ag_device *dev, enum call_state state)
{
	struct call *call;

	call = g_new0(struct call, 1);
	call->device = dev;
	call->state = state;
	call->idx = dev->id++;
	call->path = g_strdup_printf("%s/call%u", dev->path, call->idx);

	return call;
}

static void call_delete(struct call *call)
{
	if (call->pending_msg)
		dbus_message_unref(call->pending_msg);

	g_free(call->name);
	g_free(call->incoming_line);
	g_free(call->line_id);
	g_free(call->path);
	g_free(call);
}

static const char *state_to_string(uint8_t state)
{
	switch (state) {
	case CONNECTING:
		return "connecting";
	case SLC_CONNECTING:
		return "slc_connecting";
	case CONNECTED:
		return "connected";
	case DISCONNECTING:
		return "disconnecting";
	}

	return NULL;
}

static const char *call_state_to_string(enum call_state state)
{
	switch (state) {
	case CALL_STATE_ACTIVE:
		return "active";
	case CALL_STATE_HELD:
		return "held";
	case CALL_STATE_DIALING:
		return "dialing";
	case CALL_STATE_ALERTING:
		return "alerting";
	case CALL_STATE_INCOMING:
		return "incoming";
	case CALL_STATE_WAITING:
		return "waiting";
	case CALL_STATE_DISCONNECTED:
		return "disconnected";
	}

	return NULL;
}

static void device_set_state(struct hfp_ag_device *dev,
					enum connection_state state)
{
	char address[18];

	if (dev->state == state)
		return;

	ba2str(&dev->dst, address);
	DBG("device %s state %s -> %s", address, state_to_string(dev->state),
						state_to_string(state));

	dev->state = state;

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
					dev->path, TELEPHONY_AG_INTERFACE,
					"State");
}

static void device_destroy(struct hfp_ag_device *dev)
{
	device_set_state(dev, DISCONNECTING);
	// queue_remove(devices, dev);

	DBG("%s", dev->path);

	if (dev->hf) {
		hfp_hf_unref(dev->hf);
		dev->hf = NULL;
	}

	if (dev->io) {
		g_io_channel_unref(dev->io);
		dev->io = NULL;
	}

	g_dbus_unregister_interface(btd_get_dbus_connection(), dev->path,
					TELEPHONY_AG_INTERFACE);
}

static void slc_error(struct hfp_ag_device *dev)
{
	error("Could not create SLC - dropping connection");
	hfp_hf_disconnect(dev->hf);
}

static void set_chld_feat(struct hfp_ag_device *dev, char *feat)
{
	DBG(" %s", feat);

	if (strcmp(feat, "0") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_REL;
	else if (strcmp(feat, "1") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_REL_ACC;
	else if (strcmp(feat, "1x") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_REL_X;
	else if (strcmp(feat, "2") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_HOLD_ACC;
	else if (strcmp(feat, "2x") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_PRIV_X;
	else if (strcmp(feat, "3") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_MERGE;
	else if (strcmp(feat, "4") == 0)
		dev->chld_features |= HAL_HF_CLIENT_CHLD_FEAT_MERGE_DETACH;
}

static void cmd_complete_cb(enum hfp_result result, enum hfp_error cme_err,
	void *user_data)
{
	DBusMessage *msg = user_data;
	DBusMessage *reply = NULL;

	DBG("%u", result);

	switch (result) {
	case HFP_RESULT_OK:
		if (msg)
			reply = g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
		break;
	case HFP_RESULT_NO_CARRIER:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR_NO_CARRIER;
		break;
	case HFP_RESULT_ERROR:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR;
		break;
	case HFP_RESULT_BUSY:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR_BUSY;
		break;
	case HFP_RESULT_NO_ANSWER:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR_NO_ANSWER;
		break;
	case HFP_RESULT_DELAYED:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR_DELAYED;
		break;
	case HFP_RESULT_REJECTED:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR_BACKLISTED;
		break;
	case HFP_RESULT_CME_ERROR:
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR_CME;
		// ev.cme = cme_err;
		break;
	case HFP_RESULT_CONNECT:
	case HFP_RESULT_RING:
	case HFP_RESULT_NO_DIALTONE:
	default:
		error("hf-client: Unknown error code %d", result);
		// ev.type = HAL_HF_CLIENT_CMD_COMP_ERR;
		break;
	}

	// ipc_send_notif(hal_ipc, HAL_SERVICE_ID_HANDSFREE_CLIENT,
	//	HAL_EV_CLIENT_COMMAND_COMPLETE, sizeof(ev), &ev);
	if (reply) {
		g_dbus_send_message(btd_get_dbus_connection(), reply);
		dbus_message_unref(msg);
	}
}

static DBusMessage *call_answer(DBusConnection *conn, DBusMessage *msg,
	void *user_data)
{
	struct call *call = user_data;
	struct hfp_ag_device *dev = call->device;

	DBG("");

	if (call->state != CALL_STATE_INCOMING)
		return btd_error_failed(msg, "Invalid state call");

	if (!hfp_hf_send_command(dev->hf, cmd_complete_cb,
			dbus_message_ref(msg), "ATA"))
		goto failed;

	return NULL;

failed:
	return btd_error_failed(msg, "Answer command failed");
}

static DBusMessage *call_hangup(DBusConnection *conn, DBusMessage *msg,
	void *user_data)
{
	struct call *call = user_data;
	struct hfp_ag_device *dev = call->device;

	DBG("");

	if (!hfp_hf_send_command(dev->hf, cmd_complete_cb,
			dbus_message_ref(msg), "AT+CHUP"))
		goto failed;

	return NULL;

failed:
	return btd_error_failed(msg, "Hangup command failed");
}

static gboolean call_line_id_exists(const GDBusPropertyTable *property,
	void *data)
{
	struct call *call = data;

	return call->line_id != NULL;
}

static gboolean call_property_get_line_id(
	const GDBusPropertyTable *property,
	DBusMessageIter *iter, void *data)
{
	struct call *call = data;

	if (call->line_id == NULL)
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &call->line_id);

	return TRUE;
}

static gboolean call_incoming_line_exists(const GDBusPropertyTable *property,
	void *data)
{
	struct call *call = data;

	return call->incoming_line != NULL;
}

static gboolean call_property_get_incoming_line(
	const GDBusPropertyTable *property,
	DBusMessageIter *iter, void *data)
{
	struct call *call = data;

	if (call->incoming_line == NULL)
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING,
		&call->incoming_line);

	return TRUE;
}

static gboolean call_name_exists(const GDBusPropertyTable *property,
	void *data)
{
	struct call *call = data;

	return call->name != NULL;
}

static gboolean call_property_get_name(const GDBusPropertyTable *property,
	DBusMessageIter *iter, void *data)
{
	struct call *call = data;

	if (call->name == NULL)
		return FALSE;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &call->name);

	return TRUE;
}

static gboolean call_property_get_multiparty(
	const GDBusPropertyTable *property,
	DBusMessageIter *iter, void *data)
{
	struct call *call = data;
	dbus_bool_t value;

	value = call->multiparty;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);

	return TRUE;
}

static gboolean call_property_get_state(const GDBusPropertyTable *property,
	DBusMessageIter *iter, void *data)
{
	struct call *call = data;
	const char *string;

	string = call_state_to_string(call->state);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &string);

	return TRUE;
}

static const GDBusMethodTable hfp_call_methods[] = {
	{ GDBUS_ASYNC_METHOD("Answer", NULL, NULL, call_answer) },
	{ GDBUS_ASYNC_METHOD("Hangup", NULL, NULL, call_hangup) },
	{ }
};

static const GDBusPropertyTable hfp_call_properties[] = {
	{ "LineIdentification", "s", call_property_get_line_id, NULL,
			call_line_id_exists },
	{ "IncomingLine", "s", call_property_get_incoming_line, NULL,
			call_incoming_line_exists },
	{ "Name", "s", call_property_get_name, NULL, call_name_exists },
	{ "Multiparty", "b", call_property_get_multiparty },
	{ "State", "s", call_property_get_state },
	{ }
};

static void call_path_unregister(void *user_data)
{
	struct call *call = user_data;
	struct hfp_ag_device *dev = call->device;

	DBG("Unregistered interface %s on path %s",  TELEPHONY_CALL_INTERFACE,
			device_get_path(dev->device));

	call_delete(call);
}

static void ciev_cb(struct hfp_context *context, void *user_data)
{
	struct hfp_ag_device *dev = user_data;
	unsigned int index, val;
	int i;

	DBG("");

	if (!hfp_context_get_number(context, &index))
		return;

	if (!hfp_context_get_number(context, &val))
		return;

	for (i = 0; i < HFP_INDICATOR_LAST; i++) {
		if (dev->ag_ind[i].index != index)
			continue;

		if (dev->ag_ind[i].cb) {
			dev->ag_ind[i].val = val;
			dev->ag_ind[i].cb(val, dev);
			return;
		}
	}
}

static void slc_completed(struct hfp_ag_device *dev)
{
	int i;
	struct indicator *ag_ind;

	DBG("");

	ag_ind = dev->ag_ind;

	device_set_state(dev, CONNECTED);

	/* Notify Android with indicators */
	for (i = 0; i < HFP_INDICATOR_LAST; i++) {
		if (!ag_ind[i].cb)
			continue;

		ag_ind[i].cb(ag_ind[i].val, dev);
	}

	/* TODO: register unsolicited results handlers */

	// hfp_hf_register(dev->hf, bvra_cb, "+BVRA", dev, NULL);
	// hfp_hf_register(dev->hf, vgm_cb, "+VGM", dev, NULL);
	// hfp_hf_register(dev->hf, vgs_cb, "+VGS", dev, NULL);
	// hfp_hf_register(dev->hf, brth_cb, "+BTRH", dev, NULL);
	// hfp_hf_register(dev->hf, clcc_cb, "+CLCC", dev, NULL);
	hfp_hf_register(dev->hf, ciev_cb, "+CIEV", dev, NULL);
	// hfp_hf_register(dev->hf, cops_cb, "+COPS", dev, NULL);
	// hfp_hf_register(dev->hf, cnum_cb, "+CNUM", dev, NULL);
	// hfp_hf_register(dev->hf, binp_cb, "+BINP", dev, NULL);
	// hfp_hf_register(dev->hf, bcs_cb, "+BCS", dev, NULL);

	if (!hfp_hf_send_command(dev->hf, cmd_complete_cb, NULL, "AT+COPS=3,0"))
		info("hf-client: Could not send AT+COPS=3,0");
}

static void slc_chld_resp(enum hfp_result result, enum hfp_error cme_err,
							void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	hfp_hf_unregister(dev->hf, "+CHLD");

	if (result != HFP_RESULT_OK) {
		error("hf-client: CHLD error: %d", result);
		slc_error(dev);
		return;
	}

	slc_completed(dev);
}

static void slc_chld_cb(struct hfp_context *context, void *user_data)
{
	struct hfp_ag_device *dev = user_data;
	char feat[3];

	if (!hfp_context_open_container(context))
		goto failed;

	while (hfp_context_get_unquoted_string(context, feat, sizeof(feat)))
		set_chld_feat(dev, feat);

	if (!hfp_context_close_container(context))
		goto failed;

	return;

failed:
	error("hf-client: Error on CHLD response");
	slc_error(dev);
}

static void slc_cmer_resp(enum hfp_result result, enum hfp_error cme_err,
	void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (result != HFP_RESULT_OK) {
		error("hf-client: CMER error: %d", result);
		goto failed;
	}

	/* Continue with SLC creation */
	if (!(dev->features & HFP_AG_FEAT_3WAY)) {
		slc_completed(dev);
		return;
	}

	if (!hfp_hf_register(dev->hf, slc_chld_cb, "+CHLD", dev, NULL)) {
		error("hf-client: Could not register +CHLD");
		goto failed;
	}

	if (!hfp_hf_send_command(dev->hf, slc_chld_resp, dev, "AT+CHLD=?")) {
		error("hf-client: Could not send AT+CHLD");
		goto failed;
	}

	return;

failed:
	slc_error(dev);
}

static void slc_cind_status_resp(enum hfp_result result,
	enum hfp_error cme_err,
	void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	hfp_hf_unregister(dev->hf, "+CIND");

	if (result != HFP_RESULT_OK) {
		error("hf-client: CIND error: %d", result);
		goto failed;
	}

	/* Continue with SLC creation */
	if (!hfp_hf_send_command(dev->hf, slc_cmer_resp, dev,
		"AT+CMER=3,0,0,1")) {
		error("hf-client: Counld not send AT+CMER");
		goto failed;
	}

	return;

failed:
	slc_error(dev);
}

static void set_indicator_value(uint8_t index, unsigned int val,
	struct indicator *ag_ind, struct hfp_ag_device *dev)
{
	int i;

	for (i = 0; i < HFP_INDICATOR_LAST; i++) {
		if (index != ag_ind[i].index)
			continue;

		ag_ind[i].val = val;
		ag_ind[i].cb(val, dev);
		return;
	}
}

static void slc_cind_status_cb(struct hfp_context *context,
	void *user_data)
{
	struct hfp_ag_device *dev = user_data;
	uint8_t index = 1;

	DBG("");

	while (hfp_context_has_next(context)) {
		uint32_t val;

		if (!hfp_context_get_number(context, &val)) {
			error("hf-client: Error on CIND status response");
			return;
		}

		set_indicator_value(index++, val, dev->ag_ind, dev);
	}
}

static void slc_cind_resp(enum hfp_result result, enum hfp_error cme_err,
	void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	hfp_hf_unregister(dev->hf, "+CIND");

	if (result != HFP_RESULT_OK) {
		error("hf-client: CIND error: %d", result);
		goto failed;
	}

	/* Continue with SLC creation */
	if (!hfp_hf_register(dev->hf, slc_cind_status_cb, "+CIND", dev,
			NULL)) {
		error("hf-client: Counld not register +CIND");
		goto failed;
	}

	if (!hfp_hf_send_command(dev->hf, slc_cind_status_resp, dev,
			"AT+CIND?")) {
		error("hf-client: Counld not send AT+CIND?");
		goto failed;
	}

	return;

failed:
	slc_error(dev);
}

static void ciev_service_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > 1) {
		error("hf-client: Incorrect state %u:", val);
		return;
	}

	dev->network_service = val;
	g_dbus_emit_property_changed(btd_get_dbus_connection(), dev->path,
				TELEPHONY_AG_INTERFACE, "Service");
}

static void activate_calls(gpointer data, gpointer user_data)
{
	struct call *call = data;

	if (call->state == CALL_STATE_DIALING ||
			call->state == CALL_STATE_ALERTING ||
			call->state == CALL_STATE_INCOMING) {
		call->state = CALL_STATE_ACTIVE;
		g_dbus_emit_property_changed(
			btd_get_dbus_connection(), call->path,
			TELEPHONY_CALL_INTERFACE, "State");
		}
}

static void deactivate_active_calls(gpointer data, gpointer user_data)
{
	struct call *call = data;
	struct hfp_ag_device *dev = user_data;

	if (call->state == CALL_STATE_ACTIVE) {
		call->state = CALL_STATE_DISCONNECTED;
		g_dbus_emit_property_changed(
			btd_get_dbus_connection(), call->path,
			TELEPHONY_CALL_INTERFACE, "State");
		dev->calls = g_slist_remove(dev->calls, call);
		g_dbus_unregister_interface(
			btd_get_dbus_connection(), call->path,
			TELEPHONY_CALL_INTERFACE);
	}
}

static void ciev_call_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > HAL_HF_CLIENT_CALL_IND_CALL_IN_PROGRESS) {
		error("hf-client: Incorrect call state %u:", val);
		return;
	}

	if (dev->call == val)
		return;

	dev->call = !!val;

	if (dev->call == TRUE)
		g_slist_foreach(dev->calls, activate_calls, dev);
	else
		g_slist_foreach(dev->calls, deactivate_active_calls, dev);
}

static void callsetup_deactivate(gpointer data, gpointer user_data)
{
	struct call *call = data;
	struct hfp_ag_device *dev = user_data;

	if (call->state == CALL_STATE_DIALING ||
			call->state == CALL_STATE_ALERTING ||
			call->state == CALL_STATE_INCOMING ||
			call->state == CALL_STATE_WAITING) {
		call->state = CALL_STATE_DISCONNECTED;
		g_dbus_emit_property_changed(
			btd_get_dbus_connection(), call->path,
			TELEPHONY_CALL_INTERFACE, "State");
		dev->calls = g_slist_remove(dev->calls, call);
		g_dbus_unregister_interface(
			btd_get_dbus_connection(), call->path,
			TELEPHONY_CALL_INTERFACE);
	}
}

static void callsetup_alerting(gpointer data, gpointer user_data)
{
	struct call *call = data;

	if (call->state == CALL_STATE_DIALING) {
		call->state = CALL_STATE_ALERTING;
		g_dbus_emit_property_changed(
			btd_get_dbus_connection(), call->path,
			TELEPHONY_CALL_INTERFACE, "State");
	}
}

static void ciev_callsetup_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > CIND_CALLSETUP_ALERTING) {
		error("hf-client: Incorrect call setup state %u:", val);
		return;
	}

	if (dev->call_setup == val)
		return;

	dev->call_setup = val;

	if (dev->call_setup == CIND_CALLSETUP_NONE) {
		g_slist_foreach(dev->calls, callsetup_deactivate, dev);
	} else if (dev->call_setup == CIND_CALLSETUP_INCOMING) {
		bool found = FALSE;
		GSList *l;

		for (l = dev->calls; l; l = l->next) {
			struct call *call = l->data;

			if (call->state == CALL_STATE_INCOMING ||
				call->state == CALL_STATE_WAITING) {
				DBG("incoming call already in progress (%d)",
								 call->state);
				found = TRUE;
				break;
			}
		}

		if (!found) {
			struct call *call;

			call = call_new(dev, CALL_STATE_INCOMING);
			if (!g_dbus_register_interface(
						btd_get_dbus_connection(),
						call->path,
						TELEPHONY_CALL_INTERFACE,
						hfp_call_methods, NULL,
						hfp_call_properties, call,
						call_path_unregister)) {
				error("Failed to create new incoming call");
				call_delete(call);
				return;
			}
			dev->calls = g_slist_append(dev->calls, call);

			DBG("Registered interface %s on path %s",
				TELEPHONY_CALL_INTERFACE,
				call->path);
		}
	} else if (dev->call_setup == CIND_CALLSETUP_DIALING) {
		bool found = FALSE;
		GSList *l;

		for (l = dev->calls; l; l = l->next) {
			struct call *call = l->data;

			if (call->state == CALL_STATE_DIALING ||
				call->state == CALL_STATE_ALERTING) {
				DBG("dialing call already in progress (%d)",
								call->state);
				found = TRUE;
				break;
			}
		}

		if (!found) {
			struct call *call;

			call = call_new(dev, CALL_STATE_DIALING);
			if (!g_dbus_register_interface(
						btd_get_dbus_connection(),
						call->path,
						TELEPHONY_CALL_INTERFACE,
						hfp_call_methods, NULL,
						hfp_call_properties, call,
						call_path_unregister)) {
				error("Failed to create new dialing call");
				call_delete(call);
				return;
			}
			dev->calls = g_slist_append(dev->calls, call);

			DBG("Registered interface %s on path %s",
				TELEPHONY_CALL_INTERFACE,
				call->path);
		}
	} else if (dev->call_setup == CIND_CALLSETUP_ALERTING) {
		g_slist_foreach(dev->calls, callsetup_alerting, dev);
	}
}

static void ciev_callheld_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > CIND_CALLHELD_HOLD) {
		error("hf-client: Incorrect call held state %u:", val);
		return;
	}

	dev->call_held = val;

	if (dev->call_held == CIND_CALLHELD_NONE) {
		GSList *l;
		bool found_waiting = FALSE;

		for (l = dev->calls; l; l = l->next) {
			struct call *call = l->data;

			if (call->state == CALL_STATE_WAITING) {
				call->state = CALL_STATE_DISCONNECTED;
				g_dbus_emit_property_changed(
					btd_get_dbus_connection(), call->path,
					TELEPHONY_CALL_INTERFACE, "State");
				found_waiting = TRUE;
				dev->calls = g_slist_remove(dev->calls, call);
				g_dbus_unregister_interface(
					btd_get_dbus_connection(), call->path,
					TELEPHONY_CALL_INTERFACE);
					}
		}

		if (!found_waiting) {
			for (l = dev->calls; l; l = l->next) {
				struct call *call = l->data;

				if (call->state == CALL_STATE_HELD) {
					call->state = CALL_STATE_DISCONNECTED;
					g_dbus_emit_property_changed(
						btd_get_dbus_connection(),
						call->path,
						TELEPHONY_CALL_INTERFACE,
						"State");
					dev->calls = g_slist_remove(dev->calls,
									call);
					g_dbus_unregister_interface(
						btd_get_dbus_connection(),
						call->path,
						TELEPHONY_CALL_INTERFACE);
						}
			}
		}
	} else if (dev->call_held == CIND_CALLHELD_HOLD_AND_ACTIVE) {
		GSList *l;

		for (l = dev->calls; l; l = l->next) {
			struct call *call = l->data;

			if (call->state == CALL_STATE_ACTIVE) {
				call->state = CALL_STATE_HELD;
				g_dbus_emit_property_changed(
					btd_get_dbus_connection(), call->path,
					TELEPHONY_CALL_INTERFACE, "State");
			} else if (call->state == CALL_STATE_HELD) {
				call->state = CALL_STATE_ACTIVE;
				g_dbus_emit_property_changed(
					btd_get_dbus_connection(), call->path,
					TELEPHONY_CALL_INTERFACE, "State");
			}
		}
	} else if (dev->call_held == CIND_CALLHELD_HOLD) {
		GSList *l;

		for (l = dev->calls; l; l = l->next) {
			struct call *call = l->data;

			if (call->state == CALL_STATE_ACTIVE ||
				call->state == CALL_STATE_WAITING) {
				call->state = CALL_STATE_HELD;
				g_dbus_emit_property_changed(
					btd_get_dbus_connection(), call->path,
					TELEPHONY_CALL_INTERFACE, "State");
			}
		}
	}
}

static void ciev_signal_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > 5) {
		error("hf-client: Incorrect signal value %u:", val);
		return;
	}

	dev->signal = val;
	g_dbus_emit_property_changed(btd_get_dbus_connection(), dev->path,
				TELEPHONY_AG_INTERFACE, "Signal");
}

static void ciev_roam_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > 1) {
		error("hf-client: Incorrect roaming state %u:", val);
		return;
	}

	dev->roaming = val;
	g_dbus_emit_property_changed(btd_get_dbus_connection(), dev->path,
				TELEPHONY_AG_INTERFACE, "Roaming");
}

static void ciev_battchg_cb(uint8_t val, void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (val > 5) {
		error("hf-client: Incorrect battery charge value %u:", val);
		return;
	}

	dev->battchg = val;
	g_dbus_emit_property_changed(btd_get_dbus_connection(), dev->path,
				TELEPHONY_AG_INTERFACE, "BattChg");
}

static void set_indicator_parameters(uint8_t index, const char *indicator,
	unsigned int min,
	unsigned int max,
	struct indicator *ag_ind)
{
	DBG("%s, %i", indicator, index);

	/* TODO: Verify min/max values ? */

	if (strcmp("service", indicator) == 0) {
		ag_ind[HFP_INDICATOR_SERVICE].index = index;
		ag_ind[HFP_INDICATOR_SERVICE].min = min;
		ag_ind[HFP_INDICATOR_SERVICE].max = max;
		ag_ind[HFP_INDICATOR_SERVICE].cb = ciev_service_cb;
		return;
	}

	if (strcmp("call", indicator) == 0) {
		ag_ind[HFP_INDICATOR_CALL].index = index;
		ag_ind[HFP_INDICATOR_CALL].min = min;
		ag_ind[HFP_INDICATOR_CALL].max = max;
		ag_ind[HFP_INDICATOR_CALL].cb = ciev_call_cb;
		return;
	}

	if (strcmp("callsetup", indicator) == 0) {
		ag_ind[HFP_INDICATOR_CALLSETUP].index = index;
		ag_ind[HFP_INDICATOR_CALLSETUP].min = min;
		ag_ind[HFP_INDICATOR_CALLSETUP].max = max;
		ag_ind[HFP_INDICATOR_CALLSETUP].cb = ciev_callsetup_cb;
		return;
	}

	if (strcmp("callheld", indicator) == 0) {
		ag_ind[HFP_INDICATOR_CALLHELD].index = index;
		ag_ind[HFP_INDICATOR_CALLHELD].min = min;
		ag_ind[HFP_INDICATOR_CALLHELD].max = max;
		ag_ind[HFP_INDICATOR_CALLHELD].cb = ciev_callheld_cb;
		return;
	}

	if (strcmp("signal", indicator) == 0) {
		ag_ind[HFP_INDICATOR_SIGNAL].index = index;
		ag_ind[HFP_INDICATOR_SIGNAL].min = min;
		ag_ind[HFP_INDICATOR_SIGNAL].max = max;
		ag_ind[HFP_INDICATOR_SIGNAL].cb = ciev_signal_cb;
		return;
	}

	if (strcmp("roam", indicator) == 0) {
		ag_ind[HFP_INDICATOR_ROAM].index = index;
		ag_ind[HFP_INDICATOR_ROAM].min = min;
		ag_ind[HFP_INDICATOR_ROAM].max = max;
		ag_ind[HFP_INDICATOR_ROAM].cb = ciev_roam_cb;
		return;
	}

	if (strcmp("battchg", indicator) == 0) {
		ag_ind[HFP_INDICATOR_BATTCHG].index = index;
		ag_ind[HFP_INDICATOR_BATTCHG].min = min;
		ag_ind[HFP_INDICATOR_BATTCHG].max = max;
		ag_ind[HFP_INDICATOR_BATTCHG].cb = ciev_battchg_cb;
		return;
	}

	error("hf-client: Unknown indicator: %s", indicator);
}

static void slc_cind_cb(struct hfp_context *context, void *user_data)
{
	struct hfp_ag_device *dev = user_data;
	int index = 1;

	DBG("");

	while (hfp_context_has_next(context)) {
		char name[255];
		unsigned int min, max;

		/* e.g ("callsetup",(0-3)) */
		if (!hfp_context_open_container(context))
			break;

		if (!hfp_context_get_string(context, name, sizeof(name))) {
			error("hf-client: Could not get string");
			goto failed;
		}

		if (!hfp_context_open_container(context)) {
			error("hf-client: Could not open container");
			goto failed;
		}

		if (!hfp_context_get_range(context, &min, &max)) {
			if (!hfp_context_get_number(context, &min)) {
				error("hf-client: Could not get number");
				goto failed;
			}

			if (!hfp_context_get_number(context, &max)) {
				error("hf-client: Could not get number");
				goto failed;
			}
		}

		if (!hfp_context_close_container(context)) {
			error("hf-client: Could not close container");
			goto failed;
		}

		if (!hfp_context_close_container(context)) {
			error("hf-client: Could not close container");
			goto failed;
		}

		set_indicator_parameters(index, name, min, max, dev->ag_ind);
		index++;
	}

	return;

failed:
	error("hf-client: Error on CIND response");
	slc_error(dev);
}

static void slc_brsf_cb(struct hfp_context *context, void *user_data)
{
	unsigned int feat;
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (hfp_context_get_number(context, &feat))
		dev->features = feat;
}

static void slc_brsf_resp(enum hfp_result result, enum hfp_error cme_err,
	void *user_data)
{
	struct hfp_ag_device *dev = user_data;

	hfp_hf_unregister(dev->hf, "+BRSF");

	if (result != HFP_RESULT_OK) {
		error("BRSF error: %d", result);
		goto failed;
	}

	if (!hfp_hf_register(dev->hf, slc_cind_cb, "+CIND", dev, NULL)) {
		error("hf-client: Could not register for +CIND");
		goto failed;
	}

	if (!hfp_hf_send_command(dev->hf, slc_cind_resp, dev, "AT+CIND=?")) {
		error("hf-client: Could not send AT+CIND command");
		goto failed;
	}

	return;

failed:
	slc_error(dev);
}

static bool create_slc(struct hfp_ag_device *dev)
{
	DBG("");

	if (!hfp_hf_register(dev->hf, slc_brsf_cb, "+BRSF", dev, NULL))
		return false;

	return hfp_hf_send_command(dev->hf, slc_brsf_resp, dev, "AT+BRSF=%u",
							dev->hfp_hf_features);
}

static void hfp_ag_disconnect_watch(void *user_data)
{
	DBG("");

	device_destroy(user_data);
}

static void connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	struct hfp_ag_device *dev = user_data;

	DBG("");

	if (err) {
		error("%s", err->message);
		goto failed;
	}

	dev->hf = hfp_hf_new(g_io_channel_unix_get_fd(chan));
	if (!dev->hf) {
		error("Could not create hfp io");
		goto failed;
	}

	g_io_channel_set_close_on_unref(chan, FALSE);

	hfp_hf_set_close_on_unref(dev->hf, true);
	hfp_hf_set_disconnect_handler(dev->hf, hfp_ag_disconnect_watch,
					dev, NULL);

	if (!create_slc(dev)) {
		error("Could not start SLC creation");
		hfp_hf_disconnect(dev->hf);
		goto failed;
	}

	device_set_state(dev, SLC_CONNECTING);
	btd_service_connecting_complete(dev->service, 0);

	return;

failed:
	g_io_channel_shutdown(chan, TRUE, NULL);
	device_destroy(dev);
}

static void ag_dial_cb(enum hfp_result result, enum hfp_error cme_err,
							void *user_data)
{
	struct call *call = user_data;
	DBusMessage *msg = call->pending_msg;
	DBusMessage *reply;

	DBG("");

	call->pending_msg = NULL;

	if (result != HFP_RESULT_OK) {
		error("Dialing error: %d", result);
		reply = g_dbus_create_error(msg, ERROR_INTERFACE
					".Failed",
					"Dial command failed: %d", result);
		g_dbus_send_message(btd_get_dbus_connection(), reply);
		call_delete(call);
		return;
	}

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					call->path,
					TELEPHONY_CALL_INTERFACE,
					hfp_call_methods, NULL,
					hfp_call_properties, call,
					call_path_unregister)) {
		reply = g_dbus_create_error(msg, ERROR_INTERFACE
					".Failed",
					"Failed to create new call: %s",
					call->line_id);
		g_dbus_send_message(btd_get_dbus_connection(), reply);
		call_delete(call);
		return;
	}

	call->device->calls = g_slist_append(call->device->calls, call);

	DBG("Registered interface %s on path %s", TELEPHONY_CALL_INTERFACE,
							call->path);

	g_dbus_send_reply(btd_get_dbus_connection(), msg, DBUS_TYPE_INVALID);
}

static DBusMessage *ag_dial(DBusConnection *conn, DBusMessage *msg,
					void *user_data)
{
	struct hfp_ag_device *dev = user_data;
	const char *number;
	struct call *call;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &number,
					DBUS_TYPE_INVALID)) {
		return btd_error_invalid_args(msg);
	}

	call = call_new(dev, CALL_STATE_DIALING);
	call->pending_msg = dbus_message_ref(msg);

	if (number != NULL && number[0] != '\0') {
		DBG("Dialing %s", number);

		call->line_id = g_strdup(number);

		if (!hfp_hf_send_command(dev->hf, ag_dial_cb, call,
							"ATD%s;", number))
			goto failed;
	} else {
		DBG("Redialing");

		if (!hfp_hf_send_command(dev->hf, ag_dial_cb, call,
							"AT+BLDN"))
			goto failed;
	}

	return NULL;

failed:
	call_delete(call);
	return btd_error_failed(msg, "Dial command failed");
}

static DBusMessage *ag_hangup_all(DBusConnection *conn, DBusMessage *msg,
					void *user_data)
{
	struct hfp_ag_device *dev = user_data;
	bool found_active = FALSE;
	bool found_held = FALSE;
	GSList *l;

	DBG("");

	for (l = dev->calls; l; l = l->next) {
		struct call *call = l->data;

		switch (call->state) {
		case CALL_STATE_ACTIVE:
		case CALL_STATE_DIALING:
		case CALL_STATE_ALERTING:
		case CALL_STATE_INCOMING:
			found_active = TRUE;
			break;
		case CALL_STATE_HELD:
		case CALL_STATE_WAITING:
			found_held = TRUE;
			break;
		case CALL_STATE_DISCONNECTED:
			break;
		}
	}

	if (!found_active && !found_held)
		return btd_error_failed(msg, "No call to hang up");

	if (found_held) {
		if (!hfp_hf_send_command(dev->hf, cmd_complete_cb,
				found_active ? NULL : dbus_message_ref(msg),
				"AT+CHLD=0")) {
			warn("Failed to hangup held calls");
			goto failed;
		}
	}

	if (found_active) {
		if (!hfp_hf_send_command(dev->hf, cmd_complete_cb,
				dbus_message_ref(msg),
				"AT+CHUP")) {
			warn("Failed to hangup active calls");
			goto failed;
		}
	}

	return NULL;

failed:
	return btd_error_failed(msg, "Hang up all command failed");
}

static gboolean hfp_ag_property_get_state(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct hfp_ag_device *dev = data;
	const char *string;

	string = state_to_string(dev->state);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &string);

	return TRUE;
}

static gboolean hfp_ag_property_get_service(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct hfp_ag_device *dev = data;
	dbus_bool_t value;

	value = dev->network_service;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);

	return TRUE;
}

static gboolean hfp_ag_property_get_signal(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct hfp_ag_device *dev = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE,
					&dev->signal);

	return TRUE;
}

static gboolean hfp_ag_property_get_roaming(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct hfp_ag_device *dev = data;
	dbus_bool_t value;

	value = dev->roaming;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);

	return TRUE;
}

static gboolean hfp_ag_property_get_battchg(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct hfp_ag_device *dev = data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE,
					&dev->battchg);

	return TRUE;
}

static const GDBusMethodTable hfp_ag_methods[] = {
	{ GDBUS_ASYNC_METHOD("Dial", GDBUS_ARGS({"number", "s"}), NULL,
						ag_dial) },
	// { GDBUS_ASYNC_METHOD("SwapCalls", NULL, NULL, ag_swap_calls) },
	// { GDBUS_ASYNC_METHOD("ReleaseAndAnswer", NULL, NULL,
	//					ag_release_and_answer) },
	// { GDBUS_ASYNC_METHOD("ReleaseAndSwap", NULL, NULL,
	//					ag_release_and_swap) },
	// { GDBUS_ASYNC_METHOD("HoldAndAnswer", NULL, NULL,
	//					ag_hold_and_answer) },
	{ GDBUS_ASYNC_METHOD("HangupAll", NULL, NULL, ag_hangup_all) },
	// { GDBUS_ASYNC_METHOD("CreateMultiparty", NULL,
	//					GDBUS_ARGS({ "calls", "ao" }),
	//					ag_create_multiparty) },
	// { GDBUS_ASYNC_METHOD("SendTones", GDBUS_ARGS({"number", "s"}), NULL,
	//					ag_send_tones) },
	{ }
};

static const GDBusPropertyTable hfp_ag_properties[] = {
	{ "State", "s", hfp_ag_property_get_state },
	{ "Service", "b", hfp_ag_property_get_service },
	{ "Signal", "y", hfp_ag_property_get_signal },
	{ "Roaming", "b", hfp_ag_property_get_roaming },
	{ "BattChg", "y", hfp_ag_property_get_battchg },
	{ }
};

static void path_unregister(void *data)
{
	struct hfp_ag_device *hf_ag_dev = data;

	DBG("Unregistered interface %s on path %s",  TELEPHONY_AG_INTERFACE,
						hf_ag_dev->path);
}

static int hf_connect(struct btd_service *service)
{
	struct hfp_ag_device *hf_ag_dev;
	struct btd_profile *p;
	const sdp_record_t *rec;
	sdp_list_t *list, *protos;
	sdp_profile_desc_t *desc;
	int channel;
	GError *err = NULL;

	DBG("");

	hf_ag_dev = btd_service_get_user_data(service);

	p = btd_service_get_profile(hf_ag_dev->service);
	rec = btd_device_get_record(hf_ag_dev->device, p->remote_uuid);
	if (!rec)
		return -EIO;

	if (sdp_get_profile_descs(rec, &list) == 0) {
		desc = list->data;
		hf_ag_dev->version = desc->version;
	}
	sdp_list_free(list, free);

	if (sdp_get_access_protos(rec, &protos) < 0) {
		error("unable to get access protocols from record");
		return -EIO;
	}

	channel = sdp_get_proto_port(protos, RFCOMM_UUID);
	sdp_list_foreach(protos, (sdp_list_func_t) sdp_list_free, NULL);
	sdp_list_free(protos, NULL);
	if (channel <= 0) {
		error("unable to get RFCOMM channel from record");
		return -EIO;
	}

	hf_ag_dev->io = bt_io_connect(connect_cb, hf_ag_dev,
		NULL, &err,
		BT_IO_OPT_SOURCE_BDADDR, &hf_ag_dev->src,
		BT_IO_OPT_DEST_BDADDR, &hf_ag_dev->dst,
		BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
		BT_IO_OPT_CHANNEL, channel,
		BT_IO_OPT_INVALID);
	if (hf_ag_dev->io == NULL) {
		error("unable to get start connection");
		return -EIO;
	}

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					hf_ag_dev->path,
					TELEPHONY_AG_INTERFACE,
					hfp_ag_methods, NULL,
					hfp_ag_properties, hf_ag_dev,
					path_unregister)) {
		g_free(hf_ag_dev->path);
		g_free(hf_ag_dev);
		return -EINVAL; // TODO: Check
	}

	DBG("Registered interface %s on path %s", TELEPHONY_AG_INTERFACE,
							hf_ag_dev->path);

	return 0;
}

static int hf_disconnect(struct btd_service *service)
{
	struct hfp_ag_device *hf_ag_dev;

	DBG("");

	hf_ag_dev = btd_service_get_user_data(service);

	if (hf_ag_dev->hf)
		hfp_hf_disconnect(hf_ag_dev->hf);

	btd_service_disconnecting_complete(hf_ag_dev->service, 0);

	return 0;
}

static int hf_probe(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	const char *path = device_get_path(device);
	struct hfp_ag_device *hf_ag_dev;

	DBG("%s", path);

	hf_ag_dev = hfp_ag_device_new(service);
	if (!hf_ag_dev)
		return -EINVAL;

	btd_service_set_user_data(service, hf_ag_dev);

	return 0;
}

static void hf_remove(struct btd_service *service)
{
	struct btd_device *device = btd_service_get_device(service);
	const char *path = device_get_path(device);
	struct hfp_ag_device *hf_ag_dev;

	DBG("%s", path);

	hf_ag_dev = btd_service_get_user_data(service);

	g_free(hf_ag_dev->path);
	g_free(hf_ag_dev);

	btd_service_unref(service);
}

static struct btd_profile hfp_ag_profile = {
	.name		= "hfp-hf",
	.priority	= BTD_PROFILE_PRIORITY_MEDIUM,

	.remote_uuid	= HFP_AG_UUID,
	.device_probe	= hf_probe,
	.device_remove	= hf_remove,

	.auto_connect	= true,
	.connect	= hf_connect,
	.disconnect	= hf_disconnect,

	// .adapter_probe	= server_probe,
	// .adapter_remove	= server_remove,

	.experimental	= true,
};

static int hfp_init(void)
{
	btd_profile_register(&hfp_ag_profile);

	return 0;
}

static void hfp_exit(void)
{
	btd_profile_unregister(&hfp_ag_profile);
}

BLUETOOTH_PLUGIN_DEFINE(hfp, VERSION, BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
		hfp_init, hfp_exit)
