/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "cm-room-member.h"

G_BEGIN_DECLS

CmRoomMember *cm_room_member_new                  (gpointer               room,
                                                   gpointer               client,
                                                   const char            *user_id);
void          cm_room_member_set_device_changed   (CmRoomMember          *self,
                                                   gboolean               changed);
gboolean      cm_room_member_get_device_changed   (CmRoomMember          *self);
void          cm_room_member_set_json_data        (CmRoomMember          *self,
                                                   JsonObject            *object);
void          cm_room_member_set_devices          (CmRoomMember          *self,
                                                   JsonObject            *root);
GListModel   *cm_room_member_get_devices          (CmRoomMember          *self);
JsonObject   *cm_room_member_get_device_key_json  (CmRoomMember          *self);
void          cm_room_member_add_one_time_keys    (CmRoomMember          *self,
                                                   JsonObject            *root);

G_END_DECLS
