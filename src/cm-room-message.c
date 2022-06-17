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
#include "cm-room-message.h"

struct _CmRoomMessage
{
  GRoomMessage    parent_instance;

  CmMessageType   type;
  char           *transaction_id;

  char           *plain_text;
};

G_DEFINE_TYPE (CmRoomMessage, cm_room_message, G_TYPE_ROOM_MESSAGE)

static void
cm_room_message_finalize (GObject *object)
{
  CmRoomMessage *self = (CmRoomMessage *)object;

  g_free (self->transaction_id);
  g_free (self->plain_text);

  G_OBJECT_CLASS (cm_room_message_parent_class)->finalize (object);
}

static void
cm_room_message_class_init (CmRoomMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_room_message_finalize;
}

static void
cm_room_message_init (CmRoomMessage *self)
{
}

CmRoomMessage *
cm_room_message_new (CmMessageType type)
{
  CmRoomMessage *self;

  self = g_object_new (CM_TYPE_ROOM_MESSAGE, NULL);
  self->type = type;

  return self;
}

void
cm_room_message_set_transaction_id (CmRoomMessage *self,
                                    const char    *transaction_id)
{
  g_return_if_fail (CM_IS_ROOM_MESSAGE (self));

  g_free (self->transaction_id);
  self->transaction_id = g_strdup (transaction_id);
}

void
cm_room_message_set_plain (CmRoomMessage *self,
                           const char    *text)
{
  g_return_if_fail (CM_IS_ROOM_MESSAGE (self));
  g_return_if_fail (self->type == CM_MESSAGE_TYPE_TEXT);

  g_free (self->plain_text);
  self->plain_text = g_strdup (text);
}

const char *
cm_room_message_get_plain (CmRoomMessage *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE (self), NULL);

  return self->plain_text;
}

const char *
cm_room_message_get_transaction_id (CmRoomMessage *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE (self), NULL);

  return self->transaction_id;
}
