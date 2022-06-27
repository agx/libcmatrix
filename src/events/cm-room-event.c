/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-room-event-private.h"

typedef struct
{
  char *event_id;
  char *sender;
} CmRoomEventPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (CmRoomEvent, cm_room_event, CM_TYPE_EVENT)

static char *
create_event_id (guint id)
{
  /* xxx: Should we use a different one so that we won't conflict with element client? */
  return g_strdup_printf ("mc%"G_GINT64_FORMAT".%d",
                          g_get_real_time () / G_TIME_SPAN_MILLISECOND, id);
}

static void
cm_room_event_finalize (GObject *object)
{
  CmRoomEvent *self = (CmRoomEvent *)object;
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_free (priv->sender);
  g_free (priv->event_id);

  G_OBJECT_CLASS (cm_room_event_parent_class)->finalize (object);
}

static void
cm_room_event_class_init (CmRoomEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_room_event_finalize;
}

static void
cm_room_event_init (CmRoomEvent *self)
{
}

const char *
cm_room_event_get_id (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);

  return priv->event_id;
}

void
cm_room_event_create_id (CmRoomEvent *self,
                         guint          id)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_if_fail (CM_IS_ROOM_EVENT (self));
  g_return_if_fail (!priv->event_id);

  priv->event_id = create_event_id (id);
}

const char *
cm_room_event_get_sender (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);

  return priv->sender;
}

void
cm_room_event_set_sender (CmRoomEvent *self,
                          const char  *sender)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_if_fail (CM_IS_ROOM_EVENT (self));
  g_return_if_fail (!priv->sender);

  priv->sender = g_strdup (sender);
}
