// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2026  Collabora Ltd.
 *
 */

bool ascs_register_service(struct btp *btp_, struct l_dbus *dbus_,
					struct l_dbus_client *client);
void ascs_unregister_service(struct btp *btp);
bool ascs_is_service_registered();
