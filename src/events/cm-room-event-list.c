/* cm-client.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-room-event-list"

#include "cm-config.h"

#include "cm-utils-private.h"
#include "cm-event-private.h"
#include "cm-client-private.h"
#include "users/cm-user-private.h"
#include "users/cm-user-list-private.h"
#include "users/cm-room-member-private.h"
#include "cm-enc-private.h"
#include "cm-room-event-private.h"
#include "cm-room-message-event-private.h"
#include "cm-room-private.h"
#include "cm-room-event-list-private.h"

struct _CmRoomEventList
{
  GObject       parent_instance;

  CmRoom       *room;
  CmClient     *client;

  GListStore   *events_list;
  CmEvent      *canonical_alias_event;
  CmEvent      *encryption_event;
  CmEvent      *guest_access_event;
  CmEvent      *history_visibility_event;
  CmEvent      *join_rules_event;
  CmEvent      *power_level_event;
  CmEvent      *room_avatar_event;
  CmEvent      *room_create_event;
  CmEvent      *room_name_event;
  CmEvent      *room_topic_event;
  CmEvent      *tombstone_event;

  JsonObject   *local_json;

  gboolean      save_pending;
};

G_DEFINE_TYPE (CmRoomEventList, cm_room_event_list, G_TYPE_OBJECT)

#define event_m_type_str(_type) (cm_utils_get_event_type_str (_type))
#define set_json_from_event(_event, _json) do {                 \
  CmEventType _type;                                            \
                                                                \
  if (!_event)                                                  \
    break;                                                      \
                                                                \
  _type = cm_event_get_m_type(_event);                          \
  json_object_set_object_member (_json,                         \
                                 event_m_type_str (_type),      \
                                 cm_event_get_json (_event));   \
} while (0)

#define set_event_from_json(_room, _event, _json, _m_type) do { \
  JsonObject *_child;                                           \
  const char *_type;                                            \
                                                                \
  _type = cm_utils_get_event_type_str (_m_type);                \
  _child = cm_utils_json_object_get_object (_json, _type);      \
  if (!_child)                                                  \
    break;                                                      \
                                                                \
  _event = (gpointer)cm_room_event_new_from_json (_room,        \
                                                  _child,       \
                                                  NULL);        \
} while (0)

#define set_local_json_event(_json, _event) do {                \
  JsonObject *_local;                                           \
  CmEventType _type;                                            \
                                                                \
  if (!cm_utils_json_object_get_object (_json, "local"))        \
    break;                                                      \
                                                                \
  _local = cm_utils_json_object_get_object (_json, "local");    \
  _type = cm_event_get_m_type(_event);                          \
  json_object_set_object_member (_local,                        \
                                 event_m_type_str (_type),      \
                                 cm_event_get_json (event));    \
} while (0)

static void
remove_event_with_txn_id (CmRoomEventList *self,
                          CmEvent         *event)
{
  GListModel *events;
  guint n_items;

  g_assert (CM_IS_ROOM_EVENT_LIST (self));
  g_assert (CM_IS_EVENT (event));

  if (!cm_event_get_txn_id (event))
    return;

  events = G_LIST_MODEL (self->events_list);
  n_items = g_list_model_get_n_items (events);

  /* i and n_items are unsigned */
  for (guint i = n_items - 1; i + 1 > 0; i--)
    {
      g_autoptr(CmEvent) item = NULL;

      item = g_list_model_get_item (events, i);

      if (!cm_event_get_txn_id (item))
        continue;

      if (g_strcmp0 (cm_event_get_txn_id (event),
                     cm_event_get_txn_id (item)) == 0)
        {
          cm_utils_remove_list_item (self->events_list, item);
          break;
        }
    }
}

static void
room_event_list_generate_json (CmRoomEventList *self)
{
  JsonObject *json, *child;

  g_assert (CM_IS_ROOM_EVENT_LIST (self));
  g_assert (!self->local_json);

  json = json_object_new ();
  child = json_object_new ();
  self->local_json = json;

  json_object_set_object_member (json, "local", child);

  json_object_set_string_member (child, "alias", cm_room_get_name (self->room));
  /* Alias set before the current one, may be used if current one is NULL (eg: was x) */
  json_object_set_string_member (child, "last_alias", cm_room_get_name (self->room));
  json_object_set_boolean_member (child, "direct", cm_room_is_direct (self->room));
  json_object_set_int_member (child, "encryption", cm_room_is_encrypted (self->room));

  set_json_from_event (self->canonical_alias_event, child);
  set_json_from_event (self->encryption_event, child);
  set_json_from_event (self->guest_access_event, child);
  set_json_from_event (self->history_visibility_event, child);
  set_json_from_event (self->join_rules_event, child);
  set_json_from_event (self->power_level_event, child);
  set_json_from_event (self->room_avatar_event, child);
  set_json_from_event (self->room_create_event, child);
  set_json_from_event (self->room_name_event, child);
  set_json_from_event (self->room_topic_event, child);
  set_json_from_event (self->tombstone_event, child);

  /* todo */
  /* Set only if there is only one member in the room */
  /* json_object_set_string_member (child, "m.room.member", "bad"); */
  /* json_object_set_object_member (child, "summary", "bad"); */
  /* json_object_set_object_member (child, "unread_notifications", "bad"); */
}

static void
cm_room_event_list_finalize (GObject *object)
{
  CmRoomEventList *self = (CmRoomEventList *)object;

  g_clear_object (&self->events_list);

  g_clear_object (&self->canonical_alias_event);
  g_clear_object (&self->encryption_event);
  g_clear_object (&self->guest_access_event);
  g_clear_object (&self->history_visibility_event);
  g_clear_object (&self->join_rules_event);
  g_clear_object (&self->power_level_event);
  g_clear_object (&self->room_avatar_event);
  g_clear_object (&self->room_create_event);
  g_clear_object (&self->room_name_event);
  g_clear_object (&self->room_topic_event);
  g_clear_object (&self->tombstone_event);

  g_clear_pointer (&self->local_json, json_object_unref);

  g_clear_weak_pointer (&self->room);
  g_clear_weak_pointer (&self->client);

  G_OBJECT_CLASS (cm_room_event_list_parent_class)->finalize (object);
}

static void
cm_room_event_list_class_init (CmRoomEventListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_room_event_list_finalize;
}

static void
cm_room_event_list_init (CmRoomEventList *self)
{
  self->events_list = g_list_store_new (CM_TYPE_EVENT);
}

CmRoomEventList *
cm_room_event_list_new (CmRoom *room)
{
  CmRoomEventList *self;

  self = g_object_new (CM_TYPE_ROOM_EVENT_LIST, NULL);
  g_set_weak_pointer (&self->room, room);

  g_debug ("(%p) New event list for room %p", self, room);

  return self;
}

void
cm_room_event_list_set_client (CmRoomEventList *self,
                               CmClient        *client)
{
  guint n_items;

  g_return_if_fail (CM_IS_ROOM_EVENT_LIST (self));
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (!self->client);

  g_set_weak_pointer (&self->client, client);

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->events_list));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmEvent) event = NULL;
      CmUser *user;

      event = g_list_model_get_item (G_LIST_MODEL (self->events_list), i);
      user = cm_event_get_sender (event);

      if (!user)
        {
          user = cm_room_find_user (self->room, cm_event_get_sender_id (event), TRUE);
          cm_event_set_sender (event, user);
        }

      cm_user_set_client (user, client);
    }
}

CmEvent *
cm_room_event_list_get_event (CmRoomEventList *self,
                              CmEventType      type)
{
  g_return_val_if_fail (CM_IS_ROOM_EVENT_LIST (self), NULL);

  if (type == CM_M_ROOM_CANONICAL_ALIAS)
    return self->canonical_alias_event;

  if (type == CM_M_ROOM_ENCRYPTION)
    return self->encryption_event;

  if (type == CM_M_ROOM_GUEST_ACCESS)
    return self->guest_access_event;

  if (type == CM_M_ROOM_HISTORY_VISIBILITY)
    return self->history_visibility_event;

  if (type == CM_M_ROOM_JOIN_RULES)
    return self->join_rules_event;

  if (type == CM_M_ROOM_POWER_LEVELS)
    return self->power_level_event;

  if (type == CM_M_ROOM_AVATAR)
    return self->room_avatar_event;

  if (type == CM_M_ROOM_CREATE)
    return self->room_create_event;

  if (type == CM_M_ROOM_NAME)
    return self->tombstone_event;

  if (type == CM_M_ROOM_TOPIC)
    return self->room_topic_event;

  if (type == CM_M_ROOM_TOMBSTONE)
    return self->tombstone_event;


  return NULL;
}

GListModel *
cm_room_event_list_get_events (CmRoomEventList *self)
{
  g_return_val_if_fail (self, NULL);
  g_return_val_if_fail (self->room, NULL);

  return G_LIST_MODEL (self->events_list);
}

void
cm_room_event_list_set_save_pending (CmRoomEventList *self,
                                     gboolean         save_pending)
{
  g_return_if_fail (CM_IS_ROOM_EVENT_LIST (self));

  self->save_pending = !!save_pending;
}

gboolean
cm_room_event_list_save_pending (CmRoomEventList *self)
{
  g_return_val_if_fail (CM_IS_ROOM_EVENT_LIST (self), FALSE);

  return self->save_pending;
}

JsonObject *
cm_room_event_list_get_local_json (CmRoomEventList *self)
{
  g_return_val_if_fail (CM_IS_ROOM_EVENT_LIST (self), NULL);
  g_return_val_if_fail (self->room, NULL);

  if (!self->local_json)
    room_event_list_generate_json (self);

  return self->local_json;
}

void
cm_room_event_list_append_event (CmRoomEventList *self,
                                 CmEvent         *event)
{
  g_assert (CM_IS_ROOM_EVENT_LIST (self));
  g_assert (CM_IS_ROOM (self->room));

  g_list_store_append (self->events_list, event);
}

void
cm_room_event_list_add_events (CmRoomEventList *self,
                               GPtrArray       *events,
                               gboolean         append)
{
  g_autoptr(CmEvent) last_event = NULL;
  CmClient *client;
  guint position = 0, index;

  g_assert (CM_IS_ROOM_EVENT_LIST (self));
  g_assert (CM_IS_ROOM (self->room));

  if (!events || !events->len)
    return;

  client = cm_room_get_client (self->room);

  for (guint i = 0; i < events->len; i++)
    {
      CmEvent *event = events->pdata[i];
      CmUser *user;

      if (cm_event_get_sender (event) || !client)
        continue;

      user = cm_room_find_user (self->room, cm_event_get_sender_id (event), TRUE);
      cm_event_set_sender (event, user);
    }

  /* Get the last item index */
  index = g_list_model_get_n_items (G_LIST_MODEL (self->events_list));
  if (index)
    index--;

  last_event = g_list_model_get_item (G_LIST_MODEL (self->events_list), index);

  /* Remove events that matches the last event so as to avoid duplicates. */
  for (guint i = 0; last_event && i < events->len;)
    {
      CmEvent *event;

      event = events->pdata[i];

      if (g_strcmp0 (cm_event_get_id (event), cm_event_get_id (last_event)) == 0)
        g_ptr_array_remove_index (events, i);
      else
        i++;
    }

  if (append)
    {
      position = g_list_model_get_n_items (G_LIST_MODEL (self->events_list));
      g_list_store_splice (self->events_list,
                           position, 0, events->pdata, events->len);
    }
  else
    {
      g_autoptr(GPtrArray) reversed = NULL;

      reversed = g_ptr_array_sized_new (events->len);

      for (guint i = 0; i < events->len; i++)
        g_ptr_array_insert (reversed, 0, events->pdata[i]);
      g_list_store_splice (self->events_list,
                           0, 0, reversed->pdata, reversed->len);
    }
}

void
cm_room_event_list_set_local_json (CmRoomEventList *self,
                                   JsonObject      *root,
                                   CmEvent         *last_event)
{
  GListModel *model;
  JsonObject *local;
  CmRoom *room;

  g_return_if_fail (CM_IS_ROOM_EVENT_LIST (self));
  g_return_if_fail (!self->local_json);

  model = G_LIST_MODEL (self->events_list);
  /* Local json should be set only before the events are loaded */
  g_return_if_fail (g_list_model_get_n_items (model) == 0);

  if (last_event)
    g_list_store_append (self->events_list, last_event);

  if (!root)
    return;

  room = self->room;
  self->local_json = json_object_ref (root);
  local = cm_utils_json_object_get_object (root, "local");

  set_event_from_json (room, self->canonical_alias_event, local, CM_M_ROOM_CANONICAL_ALIAS);
  set_event_from_json (room, self->encryption_event, local, CM_M_ROOM_ENCRYPTION);
  set_event_from_json (room, self->guest_access_event, local, CM_M_ROOM_GUEST_ACCESS);
  set_event_from_json (room, self->history_visibility_event, local, CM_M_ROOM_HISTORY_VISIBILITY);
  set_event_from_json (room, self->join_rules_event, local, CM_M_ROOM_JOIN_RULES);
  set_event_from_json (room, self->power_level_event, local, CM_M_ROOM_POWER_LEVELS);
  set_event_from_json (room, self->room_avatar_event, local, CM_M_ROOM_AVATAR);
  set_event_from_json (room, self->room_create_event, local, CM_M_ROOM_CREATE);
  set_event_from_json (room, self->room_name_event, local, CM_M_ROOM_NAME);
  set_event_from_json (room, self->room_topic_event, local, CM_M_ROOM_TOPIC);
  set_event_from_json (room, self->tombstone_event, local, CM_M_ROOM_TOMBSTONE);
}

static JsonObject *
event_list_decrypt (CmRoomEventList *self,
                    JsonObject      *root)
{
  char *plain_text = NULL;
  JsonObject *content;
  CmClient *client;
  CmEnc *enc;

  g_assert (CM_IS_ROOM_EVENT_LIST (self));

  client = cm_room_get_client (self->room);
  enc = cm_client_get_enc (client);

  if (!enc || !root)
    return NULL;

  content = cm_utils_json_object_get_object (root, "content");
  plain_text = cm_enc_handle_join_room_encrypted (enc, self->room, content);

  return cm_utils_string_to_json_object (plain_text);
}

void
cm_room_event_list_parse_events (CmRoomEventList *self,
                                 JsonObject      *root,
                                 GPtrArray       *events,
                                 gboolean         past)
{
  JsonObject *child;
  JsonArray *array;
  guint length = 0;

  g_return_if_fail (CM_IS_ROOM_EVENT_LIST (self));
  g_return_if_fail (self->room);

  if (!root)
    return;

  /* If @events is NULL, they are considered to be state
   * events and thus it shouldn't be past events.
   */
  if (!events)
    g_return_if_fail (!past);

  g_debug ("(%p) Parsing events %p, state event: %s, past events: %s",
           self->room, root, CM_LOG_BOOL (!events), CM_LOG_BOOL (past));

  array = cm_utils_json_object_get_array (root, "events");

  if (!array)
    array = cm_utils_json_object_get_array (root, "chunk");

  if (array)
    length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      g_autoptr(CmEvent) event = NULL;
      JsonObject *decrypted = NULL;
      CmUser *user;
      const char *value;
      CmEventType type;
      gboolean encrypted = FALSE;

      child = json_array_get_object_element (array, i);

      if (g_strcmp0 (cm_utils_json_object_get_string (child, "type"),
                     "m.room.encrypted") == 0)
        {
          decrypted = event_list_decrypt (self, child);
          encrypted = TRUE;
        }

      event = (gpointer)cm_room_event_new_from_json (self->room, encrypted ? decrypted : child,
                                                     encrypted ? child : NULL);
      if (!event)
        {
          g_debug ("no event created from json");
          continue;
        }

      value = cm_event_get_sender_id (event);
      user = cm_room_find_user (self->room, cm_event_get_sender_id (event), TRUE);
      cm_event_set_sender (event, user);

      if (events)
        {
          if (CM_IS_ROOM_MESSAGE_EVENT (event) &&
              cm_event_get_txn_id (event))
            remove_event_with_txn_id (self, event);

          g_ptr_array_add (events, g_object_ref (event));
        }

      /* past events shouldn't alter room state, as they may be obsolete */
      if (past)
        continue;

      type = cm_event_get_m_type (event);

      if (type == CM_M_ROOM_AVATAR)
        g_set_object (&self->room_avatar_event, event);
      else if (type == CM_M_ROOM_CANONICAL_ALIAS)
        g_set_object (&self->canonical_alias_event, event);
      else if (type == CM_M_ROOM_ENCRYPTION)
        g_set_object (&self->encryption_event, event);
      else if (type == CM_M_ROOM_GUEST_ACCESS)
        g_set_object (&self->guest_access_event, event);
      else if (type == CM_M_ROOM_HISTORY_VISIBILITY)
        g_set_object (&self->history_visibility_event, event);
      else if (type == CM_M_ROOM_JOIN_RULES)
        g_set_object (&self->join_rules_event, event);
      else if (type == CM_M_ROOM_NAME)
        {
          g_set_object (&self->room_name_event, event);
          value = cm_room_event_get_room_name (CM_ROOM_EVENT (event));
          cm_room_set_name (self->room, value);
        }
      else if (type == CM_M_ROOM_POWER_LEVELS)
        g_set_object (&self->power_level_event, event);
      else if (type == CM_M_ROOM_MEMBER)
        cm_room_update_user (self->room, event);
      else if (type == CM_M_ROOM_TOPIC)
        g_set_object (&self->room_topic_event, event);
      else if (type == CM_M_ROOM_TOMBSTONE)
        g_set_object (&self->tombstone_event, event);

      if (type == CM_M_ROOM_AVATAR ||
          type == CM_M_ROOM_CANONICAL_ALIAS ||
          type == CM_M_ROOM_ENCRYPTION ||
          type == CM_M_ROOM_GUEST_ACCESS ||
          type == CM_M_ROOM_HISTORY_VISIBILITY ||
          type == CM_M_ROOM_JOIN_RULES ||
          type == CM_M_ROOM_NAME ||
          type == CM_M_ROOM_POWER_LEVELS ||
          type == CM_M_ROOM_TOPIC ||
          type == CM_M_ROOM_TOMBSTONE)
        {
          set_local_json_event (self->local_json, event);
          self->save_pending = TRUE;
        }

      if (type == CM_M_ROOM_AVATAR)
        g_object_notify (G_OBJECT (self->room), "name");

      if (type == CM_M_ROOM_ENCRYPTION)
        g_object_notify (G_OBJECT (self->room), "encrypted");
    }

  if (events && events->len)
    cm_room_event_list_add_events (self, events, !past);
}
