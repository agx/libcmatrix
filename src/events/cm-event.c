/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-event-private.h"

typedef struct
{
  char *event_id;

} CmEventPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (CmEvent, cm_event, G_TYPE_OBJECT)

static void
cm_event_finalize (GObject *object)
{
  CmEvent *self = (CmEvent *)object;
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_free (priv->event_id);

  G_OBJECT_CLASS (cm_event_parent_class)->finalize (object);
}

static void
cm_event_class_init (CmEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_event_finalize;
}

static void
cm_event_init (CmEvent *self)
{
}

const char *
cm_event_get_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->event_id;
}

void
cm_event_set_id (CmEvent    *self,
                 const char *id)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_if_fail (CM_IS_EVENT (self));
  g_return_if_fail (!priv->event_id);

  priv->event_id = g_strdup (id);
}
