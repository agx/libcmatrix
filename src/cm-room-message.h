/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

/* TODO */
#error "This class is experimental and shouldn't be used"

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define CM_TYPE_ROOM_MESSAGE (cm_room_message_get_type ())

/* Do this? cm-event -> cm-room-event -> cm-room-message-event? */
G_DECLARE_FINAL_TYPE (CmRoomMessage, cm_room_message, CM, ROOM_MESSAGE, GObject)

CmRoomMessage *cm_room_message_new       (CmMessageType  type);
void           cm_room_message_set_plain (CmRoomMessage *self,
                                          const char    *text);
const char    *cm_room_message_get_plain (CmRoomMessage *self);

G_END_DECLS
