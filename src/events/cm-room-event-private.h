/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <json-glib/json-glib.h>

#include "cm-types.h"
#include "cm-room-event.h"

G_BEGIN_DECLS

CmRoomEvent  *cm_room_event_new_from_json           (gpointer             room,
                                                     JsonObject          *root,
                                                     JsonObject          *encrypted);
const char   *cm_room_event_get_room_name           (CmRoomEvent         *self);
const char   *cm_room_event_get_encryption          (CmRoomEvent         *self);
JsonObject   *cm_room_event_get_room_member_json    (CmRoomEvent         *self,
                                                     const char         **user_id);
void          cm_room_event_set_room_member         (CmRoomEvent         *self,
                                                     CmUser              *user);
GRefString   *cm_room_event_get_room_member_id      (CmRoomEvent         *self);
gboolean      cm_room_event_user_has_power          (CmRoomEvent         *self,
                                                     const char          *user_id,
                                                     CmEventType          event);

GPtrArray    *cm_room_event_get_admin_ids           (CmRoomEvent         *self);
void          cm_room_event_set_admin_users         (CmRoomEvent         *self,
                                                     GPtrArray           *users);
CmStatus      cm_room_event_get_status              (CmRoomEvent         *self);
const char   *cm_room_event_get_replacement_room_id (CmRoomEvent         *self);
const char   *cm_room_event_get_topic               (CmRoomEvent         *self);
guint         cm_room_event_get_rotation_count      (CmRoomEvent         *self);
gint64        cm_room_event_get_rotation_time       (CmRoomEvent         *self);

G_END_DECLS
