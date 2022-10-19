/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-event"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-room-private.h"
#include "users/cm-account.h"
#include "cm-utils-private.h"
#include "cm-event-private.h"

typedef struct
{
  CmUser        *sender;
  GRefString    *sender_id;
  char          *sender_device_id;
  char          *event_id;
  char          *replaces_event_id;
  char          *reply_to_event_id;
  /* Transaction id generated/recived for every event */
  char          *txn_id;

  /* Transaction id received in events (like key verification) */
  char          *transaction_id;
  char          *verification_key;

  char          *state_key;
  JsonObject    *json;
  /* The JSON source if the event was encrypted */
  JsonObject    *encrypted_json;
  gint64         time_stamp;
  CmEventType    event_type;
  CmEventState   event_state;
} CmEventPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (CmEvent, cm_event, G_TYPE_OBJECT)

enum {
  UPDATED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

#define event_type_string(_event_type) (cm_utils_get_event_type_str(_event_type))

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

  if (!priv->replaces_event_id)
    {
      type = cm_utils_json_object_get_string (root, "type");

      if (g_strcmp0 (event_type_string (CM_M_ROOM_REDACTION), type) == 0)
        priv->replaces_event_id = cm_utils_json_object_dup_string (root, "redacts");
    }
}

static gpointer
cm_event_real_generate_json (CmEvent  *self,
                             gpointer  room)
{
  /* todo */
  g_assert_not_reached ();

  return NULL;
}

static char *
cm_event_real_get_api_url (CmEvent  *self,
                           gpointer  room)
{
  /* todo */
  g_assert_not_reached ();

  return NULL;
}

static void
cm_event_finalize (GObject *object)
{
  CmEvent *self = (CmEvent *)object;
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_clear_object (&priv->sender);
  g_clear_pointer (&priv->sender_id, g_ref_string_release);
  g_free (priv->sender_device_id);
  g_free (priv->event_id);
  g_free (priv->replaces_event_id);
  g_free (priv->reply_to_event_id);
  g_free (priv->txn_id);
  g_free (priv->transaction_id);
  g_free (priv->verification_key);
  g_free (priv->state_key);
  g_clear_pointer (&priv->encrypted_json, json_object_unref);
  g_clear_pointer (&priv->json, json_object_unref);

  G_OBJECT_CLASS (cm_event_parent_class)->finalize (object);
}

static void
cm_event_class_init (CmEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CmEventClass *event_class = CM_EVENT_CLASS (klass);

  object_class->finalize = cm_event_finalize;

  event_class->generate_json = cm_event_real_generate_json;
  event_class->get_api_url = cm_event_real_get_api_url;

  signals [UPDATED] =
    g_signal_new ("updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
cm_event_init (CmEvent *self)
{
}

CmEvent *
cm_event_new (CmEventType type)
{
  CmEventPrivate *priv;
  CmEvent *self;

  g_return_val_if_fail (type == CM_M_UNKNOWN ||
                        (type >= CM_M_KEY_VERIFICATION_ACCEPT &&
                         type <= CM_M_KEY_VERIFICATION_START), NULL);

  self = g_object_new (CM_TYPE_EVENT, NULL);
  priv = cm_event_get_instance_private (self);

  priv->event_type = type;

  return self;
}

CmEvent *
cm_event_new_from_json (JsonObject *root,
                        JsonObject *encrypted)
{
  CmEvent *self;

  g_return_val_if_fail (root || encrypted, NULL);

  self = g_object_new (CM_TYPE_EVENT, NULL);
  cm_event_set_json (self, root, encrypted);

  return self;
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
cm_event_get_transaction_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->transaction_id;
}

const char *
cm_event_get_verification_key (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->verification_key;
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

  if (priv->event_state == CM_EVENT_STATE_UNKNOWN &&
      CM_IS_ACCOUNT (priv->sender))
    return CM_EVENT_STATE_SENT;

  return priv->event_state;
}

void
cm_event_set_state (CmEvent      *self,
                    CmEventState  state)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  if (priv->event_state == state)
    return;

  priv->event_state = state;
  g_signal_emit (self, signals[UPDATED], 0);
}

void
cm_event_set_json (CmEvent    *self,
                   JsonObject *root,
                   JsonObject *encrypted)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);
  JsonObject *child;
  const char *type;

  g_return_if_fail (CM_IS_EVENT (self));

  if (!root && !encrypted)
    return;

  type = cm_utils_json_object_get_string (root, "type");

  if (!type)
    type = cm_utils_json_object_get_string (encrypted, "type");

  /* todo: Handle content less encrypted events (eg: redactions) */
  if (g_strcmp0 (type, event_type_string (CM_M_ROOM_ENCRYPTED)) == 0)
    {
      /* We got something encrypted */
      if (!encrypted)
        encrypted = g_steal_pointer (&root);
      priv->event_type = CM_M_ROOM_ENCRYPTED;
    }

  if (encrypted)
    priv->encrypted_json = json_object_ref (encrypted);

  priv->event_id = g_strdup (cm_utils_json_object_get_string (encrypted ?: root, "event_id"));
  priv->time_stamp = cm_utils_json_object_get_int (encrypted ?: root, "origin_server_ts");
  if (cm_utils_json_object_get_string (encrypted ?: root, "sender"))
    priv->sender_id = g_ref_string_new_intern (cm_utils_json_object_get_string (encrypted ?: root, "sender"));

  child = cm_utils_json_object_get_object (encrypted ?: root, "unsigned");
  if (cm_utils_json_object_has_member (child, "transaction_id"))
    {
      g_free (priv->txn_id);
      priv->txn_id = cm_utils_json_object_dup_string (child, "transaction_id");
    }

  if (encrypted)
    event_parse_relations (self, encrypted);

  if (!root)
    return;

  event_parse_relations (self, root);
  priv->state_key = g_strdup (cm_utils_json_object_get_string (root, "state_key"));
  priv->json = json_object_ref (root);

  type = cm_utils_json_object_get_string (root, "type");

  if (g_strcmp0 (type, event_type_string (CM_M_ROOM_MESSAGE)) == 0)
    priv->event_type = CM_M_ROOM_MESSAGE;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_MEMBER)) == 0)
    priv->event_type = CM_M_ROOM_MEMBER;
  else if (g_strcmp0 (type, event_type_string (CM_M_REACTION)) == 0)
    priv->event_type = CM_M_REACTION;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_REDACTION)) == 0)
    priv->event_type = CM_M_ROOM_REDACTION;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_TOPIC)) == 0)
    priv->event_type = CM_M_ROOM_TOPIC;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_AVATAR)) == 0)
    priv->event_type = CM_M_ROOM_AVATAR;
  else if (g_strcmp0 (type, event_type_string (CM_M_CALL_INVITE)) == 0)
    priv->event_type = CM_M_CALL_INVITE;
  else if (g_strcmp0 (type, event_type_string (CM_M_CALL_CANDIDATES)) == 0)
    priv->event_type = CM_M_CALL_CANDIDATES;
  else if (g_strcmp0 (type, event_type_string (CM_M_CALL_ANSWER)) == 0)
    priv->event_type = CM_M_CALL_ANSWER;
  else if (g_strcmp0 (type, event_type_string (CM_M_CALL_SELECT_ANSWER)) == 0)
    priv->event_type = CM_M_CALL_SELECT_ANSWER;
  else if (g_strcmp0 (type, event_type_string (CM_M_CALL_HANGUP)) == 0)
    priv->event_type = CM_M_CALL_HANGUP;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_CANONICAL_ALIAS)) == 0)
    priv->event_type = CM_M_ROOM_CANONICAL_ALIAS;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_NAME)) == 0)
    priv->event_type = CM_M_ROOM_NAME;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_CREATE)) == 0)
    priv->event_type = CM_M_ROOM_CREATE;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_POWER_LEVELS)) == 0)
    priv->event_type = CM_M_ROOM_POWER_LEVELS;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_GUEST_ACCESS)) == 0)
    priv->event_type = CM_M_ROOM_GUEST_ACCESS;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_HISTORY_VISIBILITY)) == 0)
    priv->event_type = CM_M_ROOM_HISTORY_VISIBILITY;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_JOIN_RULES)) == 0)
    priv->event_type = CM_M_ROOM_JOIN_RULES;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_SERVER_ACL)) == 0)
    priv->event_type = CM_M_ROOM_SERVER_ACL;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_ENCRYPTION)) == 0)
    priv->event_type = CM_M_ROOM_ENCRYPTION;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_THIRD_PARTY_INVITE)) == 0)
    priv->event_type = CM_M_ROOM_THIRD_PARTY_INVITE;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_RELATED_GROUPS)) == 0)
    priv->event_type = CM_M_ROOM_RELATED_GROUPS;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_TOMBSTONE)) == 0)
    priv->event_type = CM_M_ROOM_TOMBSTONE;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_PINNED_EVENTS)) == 0)
    priv->event_type = CM_M_ROOM_PINNED_EVENTS;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_PLUMBING)) == 0)
    priv->event_type = CM_M_ROOM_PLUMBING;
  else if (g_strcmp0 (type, event_type_string (CM_M_ROOM_BOT_OPTIONS)) == 0)
    priv->event_type = CM_M_ROOM_BOT_OPTIONS;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_ACCEPT)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_ACCEPT;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_CANCEL)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_CANCEL;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_DONE)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_DONE;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_KEY)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_KEY;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_MAC)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_MAC;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_READY)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_READY;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_REQUEST)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_REQUEST;
  else if (g_strcmp0 (type, event_type_string (CM_M_KEY_VERIFICATION_START)) == 0)
    priv->event_type = CM_M_KEY_VERIFICATION_START;
  else
    CM_TRACE ("unhandled event type: %s", type);

  if (priv->event_type == CM_M_KEY_VERIFICATION_REQUEST ||
      priv->event_type == CM_M_KEY_VERIFICATION_START)
    {
      child = cm_utils_json_object_get_object (root, "content");
      priv->sender_device_id = cm_utils_json_object_dup_string (child, "from_device");
      if (!priv->time_stamp)
        priv->time_stamp = cm_utils_json_object_get_int (child, "timestamp");
    }
}

GRefString *
cm_event_get_sender_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  if (priv->sender)
    return cm_user_get_id (priv->sender);

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

  if (priv->sender_id &&
      priv->sender_id != cm_user_get_id (sender))
    g_critical ("user name '%s' and '%s' doesn't match",
                priv->sender_id, cm_user_get_id (sender));

  priv->sender = g_object_ref (sender);
}

const char *
cm_event_get_sender_device_id (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), NULL);

  return priv->sender_device_id;
}

gboolean
cm_event_is_encrypted (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), FALSE);

  return !!priv->encrypted_json;
}

gboolean
cm_event_has_encrypted_content (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);
  JsonObject *child;

  g_return_val_if_fail (CM_IS_EVENT (self), FALSE);

  if (!priv->encrypted_json)
    return FALSE;

  child = cm_utils_json_object_get_object (priv->encrypted_json, "content");

  return cm_utils_json_object_has_member (child, "ciphertext");
}

gboolean
cm_event_is_decrypted (CmEvent *self)
{
  CmEventPrivate *priv = cm_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_EVENT (self), FALSE);

  return !!priv->json;
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

  if (!priv->time_stamp)
    return time (NULL) * 1000;

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

JsonObject *
cm_event_generate_json (CmEvent  *self,
                        gpointer  room)
{
  g_return_val_if_fail (CM_IS_EVENT (self), NULL);
  g_return_val_if_fail (!room || CM_IS_ROOM (room), NULL);

  return CM_EVENT_GET_CLASS (self)->generate_json (self, room);
}

char *
cm_event_get_api_url (CmEvent  *self,
                      gpointer  room)
{
  g_return_val_if_fail (CM_IS_EVENT (self), NULL);
  g_return_val_if_fail (!room || CM_IS_ROOM (room), NULL);

  return CM_EVENT_GET_CLASS (self)->get_api_url (self, room);
}
