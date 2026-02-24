// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026  Collabora Ltd.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>

#include <ell/ell.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/uuid.h"
#include "src/shared/btp.h"
#include "src/shared/util.h"
#include "src/shared/lc3.h"
#include "btpclient.h"
#include "ascs.h"

#define BAP_SINK_PATH "/org/bluez/btpclient/bap_sink"
#define BAP_SOURCE_PATH "/org/bluez/btpclient/bap_source"
#define ENDPOINT_IFACE "org.bluez.MediaEndpoint1"

/* PAC LC3 Source:
 *
 * Frequencies: 8Khz 11Khz 16Khz 22Khz 24Khz 32Khz 44.1Khz 48Khz
 * Duration: 7.5 ms 10 ms
 * Channel count: 3
 * Frame length: 26-240
 */
uint8_t source_capa[] = {
	0x03, LC3_FREQ, LC3_FREQ_ANY & 0xFF, LC3_FREQ_ANY >> 8,
	0x02, LC3_DURATION, LC3_DURATION_ANY,
	0x05, LC3_FRAME_LEN, 26, 0, 240, 0,
	0x02, LC3_CHAN_COUNT, 3
};

/* PAC LC3 Sink:
 *
 * Frequencies: 8Khz 11Khz 16Khz 22Khz 24Khz 32Khz 44.1Khz 48Khz
 * Duration: 7.5 ms 10 ms
 * Frame length: 26-240
 */
uint8_t sink_capa[] = {
	0x03, LC3_FREQ, LC3_FREQ_ANY & 0xFF, LC3_FREQ_ANY >> 8,
	0x02, LC3_DURATION, LC3_DURATION_ANY,
	0x05, LC3_FRAME_LEN, 26, 0, 240, 0
};

static struct btp *btp;
static struct l_dbus *dbus;
static bool ascs_service_registered;

static bool match_ase(const void *entry, const void *data)
{
	const struct btp_ase *ase = entry;
	uint8_t id = L_PTR_TO_UINT(data);

	return ase->ase_id == id;
}

static bool match_cig(const void *entry, const void *data)
{
	const struct btp_ase *ase = entry;
	uint8_t id = L_PTR_TO_UINT(data);

	return ase->cig_id == id;
}

static void btp_ascs_read_commands(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint16_t commands = 0;

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_ASCS_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	commands |= (1 << BTP_OP_ASCS_READ_SUPPORTED_COMMANDS);
	commands |= (1 << BTP_OP_ASCS_CONFIGURE_CODEC);
	commands |= (1 << BTP_OP_ASCS_CONFIGURE_QOS);
	commands |= (1 << BTP_OP_ASCS_ADD_ASE_TO_CIS);
	commands |= (1 << BTP_OP_ASCS_PRECONFIGURE_QOS);

	commands = L_CPU_TO_LE16(commands);

	btp_send(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_READ_SUPPORTED_COMMANDS,
			BTP_INDEX_NON_CONTROLLER, sizeof(commands), &commands);
}

static void ascs_register_endpoint_setup(struct l_dbus_message *message,
							void *user_data)
{
	uint uuid = L_PTR_TO_UINT(user_data);
	struct l_dbus_message_builder *builder;
	const char *path, *uuid_str;
	uint8_t *capa;
	unsigned int capa_size, val, i;

	if (uuid == PAC_SOURCE_CHRC_UUID) {
		path = BAP_SOURCE_PATH;
		uuid_str = PAC_SOURCE_UUID;
		capa = source_capa;
		capa_size = sizeof(source_capa);
	} else {
		path = BAP_SINK_PATH;
		uuid_str = PAC_SINK_UUID;
		capa = sink_capa;
		capa_size = sizeof(sink_capa);
	}

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_append_basic(builder, 'o', path);

	l_dbus_message_builder_enter_array(builder, "{sv}");

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "UUID");
	l_dbus_message_builder_enter_variant(builder, "s");
	l_dbus_message_builder_append_basic(builder, 's', uuid_str);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "Codec");
	l_dbus_message_builder_enter_variant(builder, "y");
	val = 0x06;
	l_dbus_message_builder_append_basic(builder, 'y', &val);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "Capabilities");
	l_dbus_message_builder_enter_variant(builder, "ay");
	l_dbus_message_builder_enter_array(builder, "y");
	for (i = 0; i < capa_size; i++)
		l_dbus_message_builder_append_basic(builder, 'y', &(capa[i]));
	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "Locations");
	l_dbus_message_builder_enter_variant(builder, "u");
	val = 1;
	l_dbus_message_builder_append_basic(builder, 'u', &val);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "SupportedContext");
	l_dbus_message_builder_enter_variant(builder, "q");
	val = 3;
	l_dbus_message_builder_append_basic(builder, 'q', &val);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "Context");
	l_dbus_message_builder_enter_variant(builder, "q");
	val = 3;
	l_dbus_message_builder_append_basic(builder, 'q', &val);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_leave_array(builder);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void ascs_register_endpoint_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	uint uuid = L_PTR_TO_UINT(user_data);
	const char *path;

	if (uuid == PAC_SOURCE_CHRC_UUID)
		path = BAP_SOURCE_PATH;
	else
		path = BAP_SINK_PATH;

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to set register endpoint (%s), %s", name, desc);

		goto failed;
	}

	l_info("Media enpoint %s registered", path);

	return;

failed:
	if (!l_dbus_object_remove_interface(dbus, path, ENDPOINT_IFACE))
		l_info("Unable to remove endpoint instance");
}

static void btp_ascs_configure_codec(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_ascs_configure_codec_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct btp_ascs_operation_completed_ev ev;

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	memcpy(adapter->codec_conf, cp->cc_ltvs, cp->cc_ltvs_len);
	adapter->codec_conf_len = cp->cc_ltvs_len;

	btp_send(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_CONFIGURE_CODEC, index, 0,
									NULL);

	memcpy(&ev.address, &cp->address, sizeof(ev.address));
	ev.address_type = cp->address_type;
	ev.ase_id = cp->ase_id;
	ev.opcode = 0;
	ev.status = 0;
	ev.flags = 0;

	btp_send(btp, BTP_ASCS_SERVICE, BTP_EV_ASCS_OPERATION_COMPLETED,
			adapter->index, sizeof(ev), &ev);

	return;

failed:
	btp_send_error(btp, BTP_ASCS_SERVICE, index, status);
}

static bool register_endpoint(struct btp_adapter *adapter, struct btp_ase *ase)
{
	uint16_t uuid;
	const char *path;

	if (bt_uuid16_cmp(&ase->uuid, ASE_SINK_UUID)) {
		uuid = PAC_SOURCE_CHRC_UUID;
		path = BAP_SOURCE_PATH;
	} else {
		uuid = PAC_SINK_CHRC_UUID;
		path = BAP_SINK_PATH;
	}

	if (!l_dbus_object_add_interface(dbus, path,
						ENDPOINT_IFACE, adapter)) {
		l_info("Unable to instantiate endpoint interface");
		return false;
	}

	if (!l_dbus_object_add_interface(dbus, path,
						L_DBUS_INTERFACE_PROPERTIES,
						adapter)) {
		l_info("Unable to instantiate the endpoint properties interface");
		return false;
	}

	l_dbus_proxy_method_call(adapter->media_proxy,
			"RegisterEndpoint",
			ascs_register_endpoint_setup,
			ascs_register_endpoint_reply,
			L_UINT_TO_PTR(uuid), NULL);

	return true;
}

static bool register_endpoints(struct btp_adapter *adapter,
						struct btp_device * device,
						uint8_t ase_id,
						uint8_t cig_id)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(device->ases); entry;
							entry = entry->next) {
		struct btp_ase *ase = entry->data;

		if (ase->ase_id == ase_id || ase->cig_id == cig_id) {
			if (!register_endpoint(adapter, ase))
				return false;
		}
	}

	return true;
}

static void btp_ascs_configure_qos(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_ascs_configure_qos_cp *cp = param;
	struct btp_device *device;
	struct btp_ase *ase;
	uint8_t status = BTP_ERROR_FAIL;
	struct btp_ascs_operation_completed_ev ev;

	device = find_device_by_address(adapter, &cp->address, cp->address_type);

	if (cp->ase_id)
		ase = l_queue_find(device->ases, match_ase, L_UINT_TO_PTR(cp->ase_id));
	else
		ase = l_queue_find(device->ases, match_cig, L_UINT_TO_PTR(cp->cig_id));
	if (!ase)
		goto failed;

	/* Register endpoints once properties has been provided by PTS */
	if (adapter->media_proxy) {
		if (!register_endpoints(adapter, device, cp->ase_id, cp->cig_id))
			goto failed;
	}

	btp_send(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_CONFIGURE_QOS, index, 0,
									NULL);

	memcpy(&ev.address, &cp->address, sizeof(ev.address));
	ev.address_type = cp->address_type;
	ev.ase_id = cp->ase_id;
	ev.opcode = 0;
	ev.status = 0;
	ev.flags = 0;

	btp_send(btp, BTP_ASCS_SERVICE, BTP_EV_ASCS_OPERATION_COMPLETED,
			adapter->index, sizeof(ev), &ev);

	return;

failed:
	btp_send_error(btp, BTP_ASCS_SERVICE, index, status);
}

static void btp_ascs_add_ase_to_cis(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	struct btp_device *dev;
	const struct btp_ascs_add_ase_to_cis_cp *cp = param;
	struct btp_ase *ase;

	dev = find_device_by_address(adapter, &cp->address, cp->address_type);
	if (!dev)
		goto failed;

	ase = l_queue_find(dev->ases, match_ase, L_UINT_TO_PTR(cp->ase_id));
	if (!ase)
		goto failed;

	ase->cig_id = cp->cig_id;
	ase->cis_id = cp->cis_id;

	btp_send(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_ADD_ASE_TO_CIS, index, 0,
									NULL);

	return;

failed:
	btp_send_error(btp, BTP_ASCS_SERVICE, index, BTP_ERROR_FAIL);
}

static void btp_ascs_preconfigure_qos(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_ascs_preconfigure_qos_cp *cp = param;

	adapter->presentation_delay = cp->presentation_delay[0] +
					(cp->presentation_delay[1] << 8) +
					(cp->presentation_delay[2] << 16);
	adapter->target_latency = 2;
	adapter->sdu_interval = cp->sdu_interval[0] +
					(cp->sdu_interval[1] << 8) +
					(cp->sdu_interval[2] << 16);
	adapter->phy = 2;
	adapter->max_sdu = cp->max_sdu;
	adapter->retransmissions = cp->retransmission_num;
	adapter->max_transport_latency = cp->max_transport_latency;

	adapter->qos_preconfigured = true;

	btp_send(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_PRECONFIGURE_QOS, index, 0,
									NULL);
}

static struct l_dbus_message *ep_set_conf(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(0);
	struct btp_device *dev = NULL;
	const char *path, *key, *dev_path, *uuid = NULL;
	struct l_dbus_message_iter options, var;
	uint8_t dir;
	struct btp_ase *ase;
	struct btp_ascs_operation_completed_ev ev;

	l_info("Set configuration");

	if (!l_dbus_message_get_arguments(message, "oa{sv}", &path, &options))
		return l_dbus_message_new_error(message, "org.bluez.Error.Invalid", NULL);

	while (l_dbus_message_iter_next_entry(&options, &key, &var)) {
		if (!strcmp(key, "Device")) {
			if (l_dbus_message_iter_get_variant(&var, "o", &dev_path))
				dev = find_device_by_path(dev_path);
		} else if (!strcmp(key, "UUID")) {
			l_dbus_message_iter_get_variant(&var, "s", &uuid);
		}
	}

	if (!dev || !uuid)
		return l_dbus_message_new_error(message, "org.bluez.Error.Invalid",
							"Invalid arguments");

	if (!bt_uuid_strcmp(uuid, PAC_SINK_UUID))
		dir = BTP_BAP_DIR_SOURCE;
	else
		dir = BTP_BAP_DIR_SINK;

	ase = find_ase_by_dir(dev, dir);
	if (!ase)
		return l_dbus_message_new_error(message, "org.bluez.Error.Invalid",
							"No ASE");

	memcpy(&ev.address, &dev->address, sizeof(ev.address));
	ev.address_type = dev->address_type;
	ev.ase_id = ase->ase_id;
	ev.opcode = 0;
	ev.status = 0;
	ev.flags = 0;

	btp_send(btp, BTP_ASCS_SERVICE, BTP_EV_ASCS_OPERATION_COMPLETED,
			adapter->index, sizeof(ev), &ev);

	return l_dbus_message_new_method_return(message);
}

static struct l_dbus_message *ep_select_conf(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	l_info("Select configuration");

	return l_dbus_message_new_error(message,
					"org.bluez.Error.NotSupported", "NotSupported");
}

static struct l_dbus_message *ep_select_properties(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct btp_adapter *adapter = user_data;
	struct l_dbus_message *reply;
	struct l_dbus_message_builder *builder;
	uint8_t i;

	l_info("Select properties");

	reply = l_dbus_message_new_method_return(message);

	builder = l_dbus_message_builder_new(reply);
	l_dbus_message_builder_enter_array(builder, "{sv}");

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "Capabilities");
	l_dbus_message_builder_enter_variant(builder, "ay");
	l_dbus_message_builder_enter_array(builder, "y");
	for (i = 0; i < adapter->codec_conf_len; i++)
		l_dbus_message_builder_append_basic(builder, 'y',
						&(adapter->codec_conf[i]));
	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	if (adapter->qos_preconfigured) {
		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's', "QoS");
		l_dbus_message_builder_enter_variant(builder, "a{sv}");
		l_dbus_message_builder_enter_array(builder, "{sv}");

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's',
						"PresentationDelay");
		l_dbus_message_builder_enter_variant(builder, "u");
		l_dbus_message_builder_append_basic(builder, 'u',
						&adapter->presentation_delay);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's',
						"TargetLatency");
		l_dbus_message_builder_enter_variant(builder, "y");
		l_dbus_message_builder_append_basic(builder, 'y',
						&adapter->target_latency);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's', "Interval");
		l_dbus_message_builder_enter_variant(builder, "u");
		l_dbus_message_builder_append_basic(builder, 'u',
						&adapter->sdu_interval);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's', "PHY");
		l_dbus_message_builder_enter_variant(builder, "y");
		l_dbus_message_builder_append_basic(builder, 'y',
						&adapter->phy);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's', "SDU");
		l_dbus_message_builder_enter_variant(builder, "q");
		l_dbus_message_builder_append_basic(builder, 'q',
						&adapter->max_sdu);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's',
						"Retransmissions");
		l_dbus_message_builder_enter_variant(builder, "y");
		l_dbus_message_builder_append_basic(builder, 'y',
						&adapter->retransmissions);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's', "Latency");
		l_dbus_message_builder_enter_variant(builder, "q");
		l_dbus_message_builder_append_basic(builder, 'q',
						&adapter->max_transport_latency);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);

		l_dbus_message_builder_leave_array(builder);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);
	}

	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	return reply;
}

static struct l_dbus_message *ep_clear_conf(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	l_info("Clear configuration");

	return l_dbus_message_new_error(message,
					"org.bluez.Error.NotSupported", "NotSupported");
}

static void setup_endpoint_interface(struct l_dbus_interface *iface)
{
	l_dbus_interface_method(iface, "SetConfiguration", 0,
					ep_set_conf, "", "oa{sv}", "transport", "properties");
	l_dbus_interface_method(iface, "SelectConfiguration", 0,
					ep_select_conf, "ay", "ay", "capabilities");
	l_dbus_interface_method(iface, "SelectProperties", 0,
					ep_select_properties, "a{sv}", "a{sv}", "capabilities");
	l_dbus_interface_method(iface, "ClearConfiguration", 0,
					ep_clear_conf, "", "o", "transport");
}

bool ascs_register_service(struct btp *btp_, struct l_dbus *dbus_,
					struct l_dbus_client *client)
{
	btp = btp_;
	dbus = dbus_;

	btp_register(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_READ_SUPPORTED_COMMANDS,
					btp_ascs_read_commands, NULL, NULL);

	btp_register(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_CONFIGURE_CODEC,
					btp_ascs_configure_codec, NULL, NULL);

	btp_register(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_CONFIGURE_QOS,
					btp_ascs_configure_qos, NULL, NULL);

	btp_register(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_ADD_ASE_TO_CIS,
					btp_ascs_add_ase_to_cis, NULL, NULL);

	btp_register(btp, BTP_ASCS_SERVICE, BTP_OP_ASCS_PRECONFIGURE_QOS,
					btp_ascs_preconfigure_qos, NULL, NULL);

	if (!l_dbus_register_interface(dbus, ENDPOINT_IFACE,
						setup_endpoint_interface,
						NULL, false)) {
		l_info("Unable to register endpoint interface");
		return false;
	}

	ascs_service_registered = true;

	return true;
}

void ascs_unregister_service(struct btp *btp)
{
	if (!l_dbus_unregister_interface(dbus, ENDPOINT_IFACE))
		l_info("Unable to unregister endpoint interface");

	btp_unregister_service(btp, BTP_ASCS_SERVICE);
	ascs_service_registered = false;
}

bool ascs_is_service_registered()
{
	return ascs_service_registered;
}
