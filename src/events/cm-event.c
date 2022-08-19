/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-utils-private.h"
#include "cm-event-private.h"

typedef struct
{
  CmUser        *sender;
  char          *sender_id;
  char          *event_id;
  char          *replaces_event_id;
  char          *reply_to_event_id;
  char          *txn_id;
  char          *state_key;
  JsonObject    *json;
  /* The JSON source if the event was encrypted */
  JsonObject    *encrypted_json;
  gint64         time_stamp;
  CmEventType    event_type;
  CmEventState   event_state;
} CmEventPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (CmEvent, cm_event, G_TYPE_OBJECT)

static char *
create_txn_id (guint id)
{
  return g_strdup_printf ("cm%"G_GINT64_FORMAT".%d",
                          g_get_real_time () / G_TIME_SPAN_MILLISECOND, id);
}

static void
event_parse_relations (CmEvent    *self,
                       JsonObject *root)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);
  JsonObject *child;
  const char *type, *value;

  g_assert (CM_IS_EVENT (self));

  child = cm_utils_json_object_get_object (root, "content");
  child = cm_utils_json_object_get_object (child, "m.relates_to");

  type = cm_utils_json_object_get_string (child, "rel_type");
  value = cm_utils_json_object_get_string (child, "event_id");

  if (g_strcmp0 (type, "m.replace") == 0)
    priv->replaces_event_id = g_strdup (value);

  if (!priv->replaces_event_id)
    {
      child = cm_utils_json_object_get_object (root, "unsigned");
      priv->replaces_event_id = g_strdup (cm_utils_json_object_get_string (child, "replaces_state"));
    }

  if (!priv->replaces_event_id)
    {
      child = cm_utils_json_object_get_object (root, "unsigned");
      child = cm_utils_json_object_get_object (child, "m.relations");
      child = cm_utils_json_object_get_object (child, "m.replace");
      priv->replaces_event_id = g_strdup (cm_utils_json_object_get_string (child, "event_id"));
    }
}

static void
cm_event_finalize (GObject *object)
{
  CmEvent *self = (CmEvent *)object;
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_clear_object (&priv->sender);
  g_free (priv->sender_id);
  g_free (priv->event_id);
  g_free (priv->replaces_event_id);
  g_free (priv->reply_to_event_id);
  g_free (priv->txn_id);
  g_free (priv->state_key);
  g_clear_pointer (&priv->encrypted_json, json_object_unref);
  g_clear_pointer (&priv->json, json_object_unref);

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

const char *
cm_event_get_replaces_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->replaces_event_id;
}

const char *
cm_event_get_reply_to_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->reply_to_event_id;
}

const char *
cm_event_get_txn_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->txn_id;
}

void
cm_event_create_txn_id (CmEvent *self,
                        guint    id)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_if_fail (CM_IS_EVENT (self));
  g_return_if_fail (!priv->event_id);

  priv->txn_id = create_txn_id (id);
}

const char *
cm_event_get_state_key (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  if (priv->state_key && *priv->state_key)
    return priv->state_key;

  return NULL;
}

CmEventType
cm_event_get_m_type (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), CM_M_UNKNOWN);

  return priv->event_type;
}

void
cm_event_set_m_type (CmEvent     *self,
                     CmEventType  type)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_if_fail (CM_IS_EVENT (self));
  g_return_if_fail (!priv->event_type);
  g_return_if_fail (type != CM_M_UNKNOWN);

  priv->event_type = type;
}

CmEventState
cm_event_get_state (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), CM_EVENT_STATE_UNKNOWN);

  return priv->event_state;
}

void
cm_event_set_json (CmEvent    *self,
                   JsonObject *root,
                   JsonObject *encrypted)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);
  const char *type;

  g_return_if_fail (CM_IS_EVENT (self));

  if (!root && !encrypted)
    return;

  if (encrypted)
    priv->encrypted_json = json_object_ref (encrypted);

  priv->event_id = g_strdup (cm_utils_json_object_get_string (encrypted ?: root, "event_id"));
  priv->time_stamp = cm_utils_json_object_get_int (encrypted ?: root, "origin_server_ts");
  priv->sender_id = g_strdup (cm_utils_json_object_get_string (encrypted ?: root, "sender"));

  if (!root)
    return;

  event_parse_relations (self, root);
  priv->state_key = g_strdup (cm_utils_json_object_get_string (root, "state_key"));
  priv->json = json_object_ref (root);

  type = cm_utils_json_object_get_string (root, "type");

  if (g_strcmp0 (type, "m.room.message") == 0)
    priv->event_type = CM_M_ROOM_MESSAGE;
  else if (g_strcmp0 (type, "m.room.member") == 0)
    priv->event_type = CM_M_ROOM_MEMBER;
  else if (g_strcmp0 (type, "m.reaction") == 0)
    priv->event_type = CM_M_REACTION;
  else if (g_strcmp0 (type, "m.room.redaction") == 0)
    priv->event_type = CM_M_ROOM_REDACTION;
  else if (g_strcmp0 (type, "m.room.topic") == 0)
    priv->event_type = CM_M_ROOM_TOPIC;
  else if (g_strcmp0 (type, "m.room.avatar") == 0)
    priv->event_type = CM_M_ROOM_AVATAR;
  else if (g_strcmp0 (type, "m.room.canonical_alias") == 0)
    priv->event_type = CM_M_ROOM_CANONICAL_ALIAS;
  else if (g_strcmp0 (type, "m.room.name") == 0)
    priv->event_type = CM_M_ROOM_NAME;
  else if (g_strcmp0 (type, "m.room.create") == 0)
    priv->event_type = CM_M_ROOM_CREATE;
  else if (g_strcmp0 (type, "m.room.power_levels") == 0)
    priv->event_type = CM_M_ROOM_POWER_LEVELS;
  else if (g_strcmp0 (type, "m.room.guest_access") == 0)
    priv->event_type = CM_M_ROOM_GUEST_ACCESS;
  else if (g_strcmp0 (type, "m.room.history_visibility") == 0)
    priv->event_type = CM_M_ROOM_HISTORY_VISIBILITY;
  else if (g_strcmp0 (type, "m.room.history_visibility") == 0)
    priv->event_type = CM_M_ROOM_HISTORY_VISIBILITY;
  else if (g_strcmp0 (type, "m.room.join_rules") == 0)
    priv->event_type = CM_M_ROOM_JOIN_RULES;
  else if (g_strcmp0 (type, "m.room.server_acl") == 0)
    priv->event_type = CM_M_ROOM_SERVER_ACL;
  else if (g_strcmp0 (type, "m.room.encryption") == 0)
    priv->event_type = CM_M_ROOM_ENCRYPTION;
  else if (g_strcmp0 (type, "m.room.third_party_invite") == 0)
    priv->event_type = CM_M_ROOM_THIRD_PARTY_INVITE;
  else if (g_strcmp0 (type, "m.room.related_groups") == 0)
    priv->event_type = CM_M_ROOM_RELATED_GROUPS;
  else if (g_strcmp0 (type, "m.room.tombstone") == 0)
    priv->event_type = CM_M_ROOM_TOMBSTONE;
  else
    g_warning ("unhandled event type: %s", type);
}

const char *
cm_event_get_sender_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->sender_id;
}

CmUser *
cm_event_get_sender (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->sender;
}

void
cm_event_set_sender (CmEvent *self,
                     CmUser  *sender)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_if_fail (CM_IS_EVENT (self));
  g_return_if_fail (!priv->sender);

  priv->sender = g_object_ref (sender);
}

/*
 * cm_event_sender_is_self:
 *
 * Set Whether the sender of the event is same
 * as the account.  This only informs if the
 * sender id is same the account user id, and
 * the event necessarily doesn't have to
 * be originated from this very device.
 */
void
cm_event_sender_is_self (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_if_fail (CM_IS_EVENT (self));
  g_return_if_fail (priv->event_state == CM_EVENT_STATE_UNKNOWN);

  priv->event_state = CM_EVENT_STATE_SENT;
}

gboolean
cm_event_is_encrypted (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), FALSE);

  return !!priv->encrypted_json;
}

/**
 * cm_event_get_time_stamp:
 * @self: A #CmEvent
 *
 * Get the event time stamp in milliseconds
 *
 * Returns: The milliseconds since Unix epoc
 */
gint64
cm_event_get_time_stamp (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), FALSE);

  return priv->time_stamp;
}

char *
cm_event_get_json_str (CmEvent  *self,
                       gboolean  prettify)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  if (priv->json)
    return cm_utils_json_object_to_string (priv->json, prettify);

  return NULL;
}

/*
 * cm_event_get_json:
 *
 * Can return %NULL, eg: when the event is encrypted,
 * and was not able to decrypt.
 *
 * Returns: (transfer full) (nullable)
 */
JsonObject *
cm_event_get_json (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  if (priv->json)
    return json_object_ref (priv->json);

  return NULL;
}

JsonObject *
cm_event_get_encrypted_json (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  if (priv->encrypted_json)
    return json_object_ref (priv->encrypted_json);

  return NULL;
}
