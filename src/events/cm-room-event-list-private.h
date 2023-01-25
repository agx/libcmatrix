/* cm-client.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <json-glib/json-glib.h>
#include <gio/gio.h>

#include "cm-types.h"

G_BEGIN_DECLS

#define CM_TYPE_ROOM_EVENT_LIST (cm_room_event_list_get_type ())

G_DECLARE_FINAL_TYPE (CmRoomEventList, cm_room_event_list, CM, ROOM_EVENT_LIST, GObject)

CmRoomEventList *cm_room_event_list_new              (CmRoom          *room);
void             cm_room_event_list_set_client       (CmRoomEventList *self,
                                                      CmClient        *client);
CmEvent         *cm_room_event_list_get_event        (CmRoomEventList *self,
                                                      CmEventType      type);
GListModel      *cm_room_event_list_get_events       (CmRoomEventList *self);
void             cm_room_event_list_set_save_pending (CmRoomEventList *self,
                                                      gboolean         save_pending);
gboolean         cm_room_event_list_save_pending     (CmRoomEventList *self);
JsonObject      *cm_room_event_list_get_local_json   (CmRoomEventList *self);
void             cm_room_event_list_append_event     (CmRoomEventList *self,
                                                      CmEvent         *event);
void             cm_room_event_list_add_events       (CmRoomEventList *self,
                                                      GPtrArray       *events,
                                                      gboolean         append);
void             cm_room_event_list_set_local_json   (CmRoomEventList *self,
                                                      JsonObject      *root,
                                                      CmEvent         *last_event);
void             cm_room_event_list_parse_events     (CmRoomEventList *self,
                                                      JsonObject      *root,
                                                      GPtrArray       *events,
                                                      gboolean         past);

G_END_DECLS
