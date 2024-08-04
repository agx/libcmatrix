/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-room-event"

#include "cm-config.h"

#include "cm-room.h"
#include "cm-room-private.h"
#include "cm-utils-private.h"
#include "cm-event-private.h"
#include "cm-room-message-event-private.h"
#include "cm-room-event-private.h"
#include "cm-room-event.h"

/**
 * CmRoomEvent:
 *
 * An event that is associated with a matrix room like e.g. a
 * topic change or member event. Basically Matrix message of type
 * `m.room.*`.
 *
 * This class somewhat confusingly represents different room event
 * types and is *also* used as base class for more specific room events
 * like `CmRoomMessageEvent`.
 */
typedef struct
{
  CmRoom        *room;
  char          *room_name;
  char          *encryption;
  GRefString    *member_id;
  GPtrArray     *users;
  JsonObject    *json;
  CmStatus       member_status;

  /* Content fetched for different events */
  union {
    char        *replacement_room_id;
    char        *topic;
  } c;

  guint          enc_rotation_count;
  guint          enc_rotation_time;
} CmRoomEventPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (CmRoomEvent, cm_room_event, CM_TYPE_EVENT)

#define ret_val_if_fail(_event, _val1, _val2, _ret) do {                \
    CmEventType type, val;                                              \
                                                                        \
    val = _val1 ?: _val2;                                               \
    type = cm_event_get_m_type (CM_EVENT (_event));                     \
    g_return_val_if_fail (type == _val1 || type == val, _ret);          \
  } while (0)

#define ret_if_fail(_event, _expected) do {             \
    CmEventType type;                                   \
                                                        \
    type = cm_event_get_m_type (CM_EVENT (_event));     \
    g_return_if_fail (type == _expected);               \
  } while (0)

static void
cm_room_event_finalize (GObject *object)
{
  CmRoomEvent *self = (CmRoomEvent *)object;
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_clear_object (&priv->room);
  g_free (priv->room_name);
  g_free (priv->encryption);
  g_clear_pointer (&priv->member_id, g_ref_string_release);
  g_clear_pointer (&priv->users, g_ptr_array_unref);
  g_clear_pointer (&priv->json, json_object_unref);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
  switch (cm_event_get_m_type (CM_EVENT (self)))
    {
      case CM_M_ROOM_TOMBSTONE:
        g_clear_pointer (&priv->c.replacement_room_id, g_free);
        break;
      case CM_M_ROOM_TOPIC:
        g_clear_pointer (&priv->c.topic, g_free);
        break;
      default:
        break;
    }
#pragma GCC diagnostic pop

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

CmRoomEvent *
cm_room_event_new_from_json (gpointer    room,
                             JsonObject *root,
                             JsonObject *encrypted)
{
  CmRoomEventPrivate *priv;
  CmRoomEvent *self = NULL;
  JsonObject *child;
  const char *value, *event_type;
  CmEventType type;

  g_return_val_if_fail (CM_IS_ROOM (room), NULL);
  g_return_val_if_fail (root || encrypted, NULL);

  event_type = cm_utils_json_object_get_string (root, "type");

  /* currently, only room messages are encrypted */
  if (encrypted && root)
    self = cm_room_message_event_new_from_json (root);

  if (!self)
    {
      if (g_strcmp0 (event_type, "m.room.message") == 0)
        self = cm_room_message_event_new_from_json (root);
      else
        self = g_object_new (CM_TYPE_ROOM_EVENT, NULL);
    }

  priv = cm_room_event_get_instance_private (self);
  priv->room = g_object_ref (room);
  cm_event_set_json (CM_EVENT (self), root, encrypted);

  if (!root)
    return self;

  priv->json = json_object_ref (root);

  if (CM_IS_ROOM_MESSAGE_EVENT (self))
    return self;

  type = cm_event_get_m_type (CM_EVENT (self));
  child = cm_utils_json_object_get_object (root, "content");

  if (type == CM_M_ROOM_NAME)
    {
      value = cm_utils_json_object_get_string (child, "name");
      g_free (priv->room_name);
      priv->room_name = g_strdup (value);
    }
  else if (type == CM_M_ROOM_ENCRYPTION)
    {
      value = cm_utils_json_object_get_string (child, "algorithm");
      priv->encryption = g_strdup (value);
      priv->enc_rotation_count = cm_utils_json_object_get_int (child, "rotation_period_msgs");
      priv->enc_rotation_time = cm_utils_json_object_get_int (child, "rotation_period_ms");

      /* Set recommended defaults if not set */
      if (!priv->enc_rotation_count)
        priv->enc_rotation_count = 100;

      if (!priv->enc_rotation_time)
        priv->enc_rotation_time = 60 * 60 * 24 * 7; /* One week */
    }
  else if (type == CM_M_ROOM_MEMBER)
    {
      const char *membership;

      membership = cm_utils_json_object_get_string (child, "membership");
      priv->member_status = CM_STATUS_UNKNOWN;

      if (g_strcmp0 (membership, "join") == 0)
        priv->member_status = CM_STATUS_JOIN;
      else if (g_strcmp0 (membership, "invite") == 0)
        priv->member_status = CM_STATUS_INVITE;
      else if (g_strcmp0 (membership, "leave") == 0)
        priv->member_status = CM_STATUS_LEAVE;
      else if (g_strcmp0 (membership, "ban") == 0)
        priv->member_status = CM_STATUS_BAN;
      else if (g_strcmp0 (membership, "knock") == 0)
        priv->member_status = CM_STATUS_KNOCK;

      if (priv->member_status == CM_STATUS_INVITE)
        priv->member_id = g_ref_string_new_intern (cm_event_get_state_key (CM_EVENT (self)));
      else
        priv->member_id = g_ref_string_new_intern (cm_event_get_sender_id (CM_EVENT (self)));
    }
  else if (type == CM_M_ROOM_TOMBSTONE)
    {
      value = cm_utils_json_object_get_string (child, "replacement_room");
      priv->c.replacement_room_id = g_strdup (value);
    }
  else if (type == CM_M_ROOM_TOPIC)
    {
      value = cm_utils_json_object_get_string (child, "topic");
      priv->c.topic = g_strdup (value);
    }

  return self;
}

/**
 * cm_room_event_get_room:
 * @self: The room event
 *
 * Get the room this event belongs to
 *
 * Returns:(transfer none): The room
 */
CmRoom *
cm_room_event_get_room (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);

  return priv->room;
}

const char *
cm_room_event_get_room_name (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_NAME, 0, NULL);

  return priv->room_name;
}

const char *
cm_room_event_get_encryption (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_ENCRYPTION, 0, NULL);

  return priv->encryption;
}

JsonObject *
cm_room_event_get_room_member_json (CmRoomEvent  *self,
                                    const char  **user_id)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);
  JsonObject *child;

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_MEMBER, 0, NULL);

  child = cm_utils_json_object_get_object (priv->json, "content");

  if (user_id)
    {
      if (g_strcmp0 (cm_utils_json_object_get_string (child, "membership"), "join") == 0)
        *user_id = cm_utils_json_object_get_string (priv->json, "sender");
      else
        *user_id = cm_utils_json_object_get_string (priv->json, "state_key");

      if (G_UNLIKELY (!*user_id || !**user_id))
        *user_id = cm_utils_json_object_get_string (priv->json, "sender");
    }

  return child;
}

void
cm_room_event_set_room_member (CmRoomEvent *self,
                               CmUser      *user)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);
  const char *user_id;

  g_return_if_fail (CM_IS_ROOM_EVENT (self));
  g_return_if_fail (CM_IS_USER (user));
  g_return_if_fail (!priv->users);
  ret_if_fail (self, CM_M_ROOM_MEMBER);

  cm_room_event_get_room_member_json (self, &user_id);
  g_return_if_fail (g_strcmp0 (cm_user_get_id (user), user_id) == 0);

  priv->users = g_ptr_array_new_full (1, g_object_unref);
  g_ptr_array_add (priv->users, g_object_ref (user));
}

/**
 * cm_room_event_get_room_member:
 * @self: The room event
 *
 * Get the room member of this room event
 *
 * Returns:(transfer none):The room member
 */
CmUser *
cm_room_event_get_room_member (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_MEMBER, 0, NULL);
  g_return_val_if_fail (priv->users, NULL);

  return priv->users->pdata[0];
}

GRefString *
cm_room_event_get_room_member_id (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_MEMBER, 0, NULL);

  return priv->member_id;
}

gboolean
cm_room_event_user_has_power (CmRoomEvent *self,
                              const char  *user_id,
                              CmEventType  event)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);
  JsonObject *child, *content;
  int user_power = 0;

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), FALSE);
  g_return_val_if_fail (user_id && *user_id == '@', FALSE);
  g_return_val_if_fail (priv->json, FALSE);
  ret_val_if_fail (self, CM_M_ROOM_POWER_LEVELS, 0, FALSE);

  if (!priv->json)
    return FALSE;

  content = cm_utils_json_object_get_object (priv->json, "content");
  child = cm_utils_json_object_get_object (content, "users");
  user_power = cm_utils_json_object_get_int (child, user_id);

  if (!user_power)
    user_power = cm_utils_json_object_get_int (content, "users_default");

  child = cm_utils_json_object_get_object (content, "events");

  if (event == CM_M_ROOM_NAME)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.name");

  if (event == CM_M_ROOM_POWER_LEVELS)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.power_levels");

  if (event == CM_M_ROOM_HISTORY_VISIBILITY)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.history_visibility");

  if (event == CM_M_ROOM_CANONICAL_ALIAS)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.canonical_alias");

  if (event == CM_M_ROOM_AVATAR)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.avatar");

  if (event == CM_M_ROOM_TOMBSTONE)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.tombstone");

  if (event == CM_M_ROOM_SERVER_ACL)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.server_acl");

  if (event == CM_M_ROOM_ENCRYPTION)
    return user_power >= cm_utils_json_object_get_int (child, "m.room.encryption");

  if (event == CM_M_ROOM_INVITE)
    {
      if (!cm_utils_json_object_has_member (content, "invite"))
        return user_power >= 50;

      return user_power >= cm_utils_json_object_get_int (content, "invite");
    }

  if (event == CM_M_ROOM_BAN)
    {
      if (!cm_utils_json_object_has_member (content, "ban"))
        return user_power >= 50;

      return user_power >= cm_utils_json_object_get_int (content, "ban");
    }

  if (event == CM_M_ROOM_KICK)
    {
      if (!cm_utils_json_object_has_member (content, "kick"))
        return user_power >= 50;

      return user_power >= cm_utils_json_object_get_int (content, "kick");
    }

  return user_power >= cm_utils_json_object_get_int (content, "events_default");
}

/**
 * cm_room_event_get_power_level_admins:
 * @self: A #CmRoomEvent
 *
 * Get the list of users that have power greater than
 * the default room power level, as reported by the
 * server.
 *
 * Returns: (transfer full): An GPtrArray of strings
 */
GPtrArray *
cm_room_event_get_admin_ids (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);
  g_autoptr(GList) users = NULL;
  GPtrArray *admin_ids;
  JsonObject *child;

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  g_return_val_if_fail (priv->json, NULL);
  ret_val_if_fail (self, CM_M_ROOM_POWER_LEVELS, 0, NULL);

  child = cm_utils_json_object_get_object (priv->json, "content");
  child = cm_utils_json_object_get_object (child, "users");
  if (child)
    users = json_object_get_members (child);

  admin_ids = g_ptr_array_new_full (16, g_free);

  for (GList *node = users; node && node->data; node = node->next)
    g_ptr_array_add (admin_ids, g_strdup (node->data));

  return admin_ids;
}

/**
 * cm_room_event_set_admin_users:
 * @self: A #CmRoomEvent
 * @users: (transfer full): An array of #CmUser
 *
 * Set the list of admin users, the list of user id
 * should match cm_room_event_get_admin_ids()
 *
 * each member in @users should have a ref added.
 */
void
cm_room_event_set_admin_users (CmRoomEvent *self,
                               GPtrArray   *users)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);
  JsonObject *child;

  g_return_if_fail (CM_IS_ROOM_EVENT (self));
  g_return_if_fail (users);
  g_return_if_fail (priv->json);
  g_return_if_fail (!priv->users);
  ret_if_fail (self, CM_M_ROOM_POWER_LEVELS);

  child = cm_utils_json_object_get_object (priv->json, "content");
  child = cm_utils_json_object_get_object (child, "users");
  g_return_if_fail (child);
  g_return_if_fail (json_object_get_size (child) == users->len);

  for (guint i = 0; i < users->len; i++)
    {
      CmUser *user = users->pdata[i];

      /* Never supposed to happen */
      if (!json_object_has_member (child, cm_user_get_id (user)))
        {
          g_critical ("User '%s' not in list", cm_user_get_id (user));
          return;
        }
    }

  priv->users = users;
}

CmStatus
cm_room_event_get_status (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), CM_STATUS_UNKNOWN);
  ret_val_if_fail (self, CM_M_ROOM_MEMBER, 0, CM_STATUS_UNKNOWN);

  return priv->member_status;
}

const char *
cm_room_event_get_replacement_room_id (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_TOMBSTONE, 0, NULL);

  return priv->c.replacement_room_id;
}

guint
cm_room_event_get_rotation_count (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), 100);
  ret_val_if_fail (self, CM_M_ROOM_ENCRYPTION, 0, 100);

  return priv->enc_rotation_count;
}

gint64
cm_room_event_get_rotation_time (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), 100);
  ret_val_if_fail (self, CM_M_ROOM_ENCRYPTION, 0, 100);

  return priv->enc_rotation_time;
}

const char *
cm_room_event_get_topic (CmRoomEvent *self)
{
  CmRoomEventPrivate *priv = cm_room_event_get_instance_private (self);

  g_return_val_if_fail (CM_IS_ROOM_EVENT (self), NULL);
  ret_val_if_fail (self, CM_M_ROOM_TOPIC, 0, NULL);

  return priv->c.topic;
}
