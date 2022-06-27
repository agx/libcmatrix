/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-common.h"
#include "cm-enums.h"
#include "cm-room-message-event-private.h"

struct _CmRoomMessageEvent
{
  CmRoomEvent     parent_instance;

  CmMessageType   type;

  char           *plain_text;
  GFile          *file;
};

G_DEFINE_TYPE (CmRoomMessageEvent, cm_room_message_event, CM_TYPE_ROOM_EVENT)

static void
cm_room_message_event_finalize (GObject *object)
{
  CmRoomMessageEvent *self = (CmRoomMessageEvent *)object;

  g_free (self->plain_text);

  G_OBJECT_CLASS (cm_room_message_event_parent_class)->finalize (object);
}

static void
cm_room_message_event_class_init (CmRoomMessageEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_room_message_event_finalize;
}

static void
cm_room_message_event_init (CmRoomMessageEvent *self)
{
}

CmRoomMessageEvent *
cm_room_message_event_new (CmMessageType type)
{
  CmRoomMessageEvent *self;

  self = g_object_new (CM_TYPE_ROOM_MESSAGE_EVENT, NULL);
  self->type = type;

  return self;
}

CmMessageType
cm_room_message_event_get_msg_type (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), CM_MESSAGE_TYPE_UNKNOWN);

  return self->type;
}

void
cm_room_message_event_set_plain (CmRoomMessageEvent *self,
                                 const char    *text)
{
  g_return_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_if_fail (self->type == CM_MESSAGE_TYPE_TEXT);

  g_free (self->plain_text);
  self->plain_text = g_strdup (text);
}

const char *
cm_room_message_event_get_plain (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), NULL);

  return self->plain_text;
}

void
cm_room_message_event_set_file (CmRoomMessageEvent *self,
                                const char         *body,
                                GFile              *file)
{
  g_return_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_if_fail (self->type == CM_MESSAGE_TYPE_FILE);
  g_return_if_fail (!self->file);

  self->file = g_object_ref (file);
  self->plain_text = g_strdup (body);
}

GFile *
cm_room_message_event_get_file (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), NULL);

  return self->file;
}
