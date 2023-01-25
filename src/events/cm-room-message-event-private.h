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
#include <glib-object.h>
#include <gio/gio.h>

#include "cm-room-event-private.h"
#include "cm-room-message-event.h"

G_BEGIN_DECLS

CmRoomMessageEvent *cm_room_message_event_new           (CmContentType       type);
CmRoomEvent        *cm_room_message_event_new_from_json (JsonObject         *root);
void                cm_room_message_event_set_body      (CmRoomMessageEvent *self,
                                                         const char         *text);
void                cm_room_message_event_set_file      (CmRoomMessageEvent *self,
                                                         const char         *body,
                                                         GFile              *file);
GFile              *cm_room_message_event_get_file      (CmRoomMessageEvent *self);


G_END_DECLS
