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

#include "cm-room-event-private.h"

G_BEGIN_DECLS

typedef enum {
  CM_MESSAGE_TYPE_UNKNOWN,
  CM_MESSAGE_TYPE_AUDIO,
  CM_MESSAGE_TYPE_EMOTE,
  CM_MESSAGE_TYPE_FILE,
  CM_MESSAGE_TYPE_IMAGE,
  CM_MESSAGE_TYPE_LOCATION,
  CM_MESSAGE_TYPE_TEXT
} CmMessageType;

#define CM_TYPE_ROOM_MESSAGE_EVENT (cm_room_message_event_get_type ())

G_DECLARE_FINAL_TYPE (CmRoomMessageEvent, cm_room_message_event, CM, ROOM_MESSAGE_EVENT, CmRoomEvent)

CmRoomMessageEvent *cm_room_message_event_new           (CmMessageType       type);
CmMessageType       cm_room_message_event_get_msg_type  (CmRoomMessageEvent *self);
/* void                cm_room_message_event_new_from_json (JsonObject         *json); */
void                cm_room_message_event_set_plain     (CmRoomMessageEvent *self,
                                                         const char         *text);
const char         *cm_room_message_event_get_plain     (CmRoomMessageEvent *self);
void                cm_room_message_event_set_file      (CmRoomMessageEvent *self,
                                                         const char         *body,
                                                         GFile              *file);
GFile              *cm_room_message_event_get_file      (CmRoomMessageEvent *self);


G_END_DECLS
