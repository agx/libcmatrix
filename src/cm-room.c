/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-room"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-input-stream-private.h"
#include "cm-client-private.h"
#include "cm-utils-private.h"
#include "cm-net-private.h"
#include "cm-enc-private.h"
#include "cm-common.h"
#include "events/cm-event-private.h"
#include "events/cm-room-event-list-private.h"
#include "events/cm-room-message-event-private.h"
#include "users/cm-room-member-private.h"
#include "users/cm-room-member.h"
#include "users/cm-user.h"
#include "users/cm-user-private.h"
#include "users/cm-user-list-private.h"
#include "cm-room-private.h"
#include "cm-room.h"

#define KEY_TIMEOUT         10000 /* milliseconds */
#define TYPING_TIMEOUT      4     /* seconds */

struct _CmRoom
{
  GObject parent_instance;

  CmRoomEventList *room_event;
  GListStore *joined_members;
  GHashTable *joined_members_table;
  GListStore *invited_members;
  GHashTable *invited_members_table;

  /* key: GRefString (user_id), value: #GPtrArray of #CmDevice */
  /* Shall store only devices that are added */
  /* Reset if any device/user is removed/left */
  GHashTable   *changed_devices;
  /* array of #CmUser */
  GPtrArray    *changed_users;
  /* array of #CmUserKey */
  GPtrArray    *one_time_keys;
  GCancellable *enc_cancellable;
  JsonObject *local_json;
  CmClient   *client;
  char       *name;
  char       *generated_name;
  char       *past_name;
  char       *room_id;
  char       *replacement_room;
  char       *encryption;
  char       *prev_batch;

  GQueue     *message_queue;
  guint       retry_timeout_id;

  CmStatus    room_status;
  gboolean    has_prev_batch;
  gboolean    is_direct;

  /* Use g_get_monotonic_time(), we only need the interval */
  gint64     typing_set_time;
  /* set doesn't mean the user has typing state set,
   * also compare with typing_set_time and TYPING_TIMEOUT */
  gboolean   typing;

  gboolean    loading_initial_sync;
  gboolean    loading_past_events;
  gboolean    db_save_pending;
  gboolean    is_sending_message;
  gboolean    name_loaded;
  gboolean    name_loading;
  gboolean    joined_members_loading;
  gboolean    joined_members_loaded;
  gboolean    querying_keys;
  gboolean    keys_queried;
  gboolean    claiming_keys;
  gboolean    keys_claimed;
  gboolean    uploading_keys;
  gboolean    keys_uploaded;
  gboolean    initial_sync_done;
};

G_DEFINE_TYPE (CmRoom, cm_room, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_ROOM_ID,
  PROP_ENCRYPTED,
  PROP_NAME,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* static gboolean room_resend_message          (gpointer user_data); */
static void     room_send_message_from_queue (CmRoom *self);

static CmUser *
room_find_user (CmRoom     *self,
                GRefString *matrix_id,
                gboolean    add_if_missing)
{
  CmUserList *user_list;
  GListModel *model;
  CmUser *user = NULL;

  g_assert (CM_IS_ROOM (self));
  g_assert (matrix_id && *matrix_id == '@');
  g_return_val_if_fail (self->client, NULL);

  user_list = cm_client_get_user_list (self->client);
  user = cm_user_list_find_user (user_list, matrix_id, add_if_missing);
  model = G_LIST_MODEL (self->joined_members);

  if (user &&
      !cm_utils_get_item_position (model, user, NULL))
    {
      g_list_store_append (self->joined_members, user);
      g_hash_table_insert (self->joined_members_table,
                           g_ref_string_acquire (matrix_id),
                           g_object_ref (user));
    }

  return user;
}

static char *
cm_room_generate_name (CmRoom *self)
{
  GListModel *model;
  const char *name_a = NULL, *name_b = NULL;
  guint n_items, count;

  g_assert (CM_IS_ROOM (self));

  model = G_LIST_MODEL (self->joined_members);
  count = n_items = g_list_model_get_n_items (model);

  if (n_items == 1)
    {
      g_autoptr(CmUser) user = NULL;

      user = g_list_model_get_item (model, 0);

      /* Don't add self to create room name */
      if (cm_user_get_id (user) == cm_client_get_user_id (self->client))
        count = n_items = 0;
    }

  if (!n_items)
    {
      model = G_LIST_MODEL (self->invited_members);
      count = n_items = g_list_model_get_n_items (model);
    }
  for (guint i = 0; i < MIN (3, n_items); i++) {
    g_autoptr(CmUser) user = NULL;

    user = g_list_model_get_item (model, i);

    /* Don't add self to create room name */
    if (cm_user_get_id (user) == cm_client_get_user_id (self->client))
      {
        count--;
        continue;
      }

    if (!name_a) {
      name_a = cm_user_get_display_name (user);

      if (!name_a || !*name_a)
        name_a = cm_user_get_id (user);
    } else {
      name_b = cm_user_get_display_name (user);

      if (!name_b || !*name_b)
        name_b = cm_user_get_id (user);
    }
  }

  /* fixme: Depend on gettext and make these strings translatable */
  if (count == 0)
    return g_strdup ("Empty room");

  if (count == 1)
    return g_strdup (name_a);

  if (count == 2)
    return g_strdup_printf ("%s and %s", name_a ?: "", name_b ?: "");

  return g_strdup_printf ("%s and %u other(s)", name_a ?: "", count - 1);
}

static void
room_add_event_to_db (CmRoom  *self,
                      CmEvent *event)
{
  g_autoptr(GPtrArray) events = NULL;

  g_assert (CM_IS_ROOM (self));
  g_assert (CM_IS_EVENT (event));

  events = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (events, g_object_ref (event));
  cm_db_add_room_events (cm_client_get_db (self->client),
                         self, events, FALSE);
}

static void
room_load_device_keys_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(CmRoom) self = user_data;
  g_autoptr(GPtrArray) users = NULL;
  g_autoptr(GError) error = NULL;

  users = cm_user_list_load_devices_finish (CM_USER_LIST (object), result, &error);
  self->querying_keys = FALSE;

  if (error)
    {
      g_debug ("Error loading devices: %s", error->message);
    }
  else
    {
      self->keys_queried = TRUE;
      room_send_message_from_queue (self);
    }
}

static void
room_claim_keys_cb (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr(CmRoom) self = user_data;
  g_autoptr(GError) error = NULL;
  GPtrArray *keys;

  g_clear_pointer (&self->one_time_keys, g_ptr_array_unref);
  keys = cm_user_list_claim_keys_finish (CM_USER_LIST (object), result, &error);
  self->one_time_keys = keys;
  self->claiming_keys = FALSE;

  if (error)
    {
      g_debug ("Error claiming keys: %s", error->message);
    }
  else
    {
      self->keys_claimed = TRUE;
      room_send_message_from_queue (self);
    }
}

static void
room_upload_keys_cb (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr(CmRoom) self = user_data;
  g_autoptr(GError) error = NULL;

  cm_user_list_upload_keys_finish (CM_USER_LIST (object), result, &error);
  self->uploading_keys = FALSE;

  if (error)
    {
      g_debug ("Error uploading group keys: %s", error->message);
    }
  else
    {
      self->keys_uploaded = TRUE;
      g_ptr_array_set_size (self->one_time_keys, 0);
      room_send_message_from_queue (self);
    }
}

static void
ensure_encryption_keys (CmRoom *self)
{
  CmUserList *user_list;

  g_return_if_fail (cm_room_is_encrypted (self));

  if (self->enc_cancellable &&
      g_cancellable_is_cancelled (self->enc_cancellable))
    g_clear_object (&self->enc_cancellable);

  if (!self->enc_cancellable)
    self->enc_cancellable = g_cancellable_new ();

  if (self->joined_members_loading || self->querying_keys ||
      self->claiming_keys || self->uploading_keys)
    return;

  user_list = cm_client_get_user_list (self->client);

  if (!self->joined_members_loaded)
    cm_room_load_joined_members_async (self, self->enc_cancellable, NULL, NULL);
  else if (!self->keys_queried)
    {
      self->querying_keys = TRUE;
      cm_user_list_load_devices_async (user_list, self->changed_users,
                                       room_load_device_keys_cb,
                                       g_object_ref (self));
    }
  else if (!self->keys_claimed)
    {
      g_autoptr(GHashTable) users = NULL;
      GListModel *members;

      self->claiming_keys = TRUE;
      members = G_LIST_MODEL (self->joined_members);
      users = g_hash_table_new_full (g_direct_hash,
                                     g_direct_equal,
                                     (GDestroyNotify)g_ref_string_release,
                                     (GDestroyNotify)g_ptr_array_unref);

      for (guint i = 0; i < g_list_model_get_n_items (members); i++)
        {
          g_autoptr(CmUser) user = NULL;
          GListModel *device_list;
          GRefString *user_id;
          GPtrArray *devices;

          user = g_list_model_get_item (members, i);
          devices = g_ptr_array_new_full (32, g_object_unref);
          device_list = cm_user_get_devices (user);

          for (guint j = 0; j < g_list_model_get_n_items (device_list); j++)
            g_ptr_array_add (devices, g_list_model_get_item (device_list, j));

          user_id = g_ref_string_acquire (cm_user_get_id (user));
          g_hash_table_insert (users, user_id, devices);
        }

      cm_user_list_claim_keys_async (user_list, self, users,
                                     room_claim_keys_cb,
                                     g_object_ref (self));
    }
  else if (!self->keys_uploaded)
    {
      if (!self->one_time_keys || !self->one_time_keys->len)
        {
          g_warning ("no keys uploaded, and no keys left to upload");
          return;
        }

      self->uploading_keys = TRUE;
      cm_user_list_upload_keys_async (user_list, self,
                                      self->one_time_keys,
                                      room_upload_keys_cb,
                                      g_object_ref (self));
    }
  else
    g_return_if_reached ();
}

static void
cm_room_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
  CmRoom *self = (CmRoom *)object;

  switch (prop_id)
    {
    case PROP_ROOM_ID:
      g_value_set_string (value, self->room_id);
      break;

    case PROP_ENCRYPTED:
      g_value_set_boolean (value, cm_room_is_encrypted (self));
      break;

    case PROP_NAME:
      g_value_set_string (value, cm_room_get_name (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
cm_room_set_property (GObject      *object,
                      guint         prop_id,
                      const GValue *value,
                      GParamSpec   *pspec)
{
  CmRoom *self = (CmRoom *)object;

  switch (prop_id)
    {
    case PROP_ROOM_ID:
      self->room_id = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cm_room_finalize (GObject *object)
{
  CmRoom *self = (CmRoom *)object;

  if (self->enc_cancellable)
    g_cancellable_cancel (self->enc_cancellable);
  g_clear_object (&self->enc_cancellable);

  g_hash_table_unref (self->joined_members_table);
  g_clear_object (&self->joined_members);

  g_hash_table_unref (self->invited_members_table);
  g_clear_object (&self->invited_members);

  g_clear_pointer (&self->one_time_keys, g_ptr_array_unref);

  g_clear_object (&self->client);
  g_free (self->room_id);

  g_free (self->name);
  g_free (self->generated_name);

  g_queue_free_full (self->message_queue, g_object_unref);

  G_OBJECT_CLASS (cm_room_parent_class)->finalize (object);
}

static void
cm_room_class_init (CmRoomClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = cm_room_get_property;
  object_class->set_property = cm_room_set_property;
  object_class->finalize = cm_room_finalize;

  properties[PROP_ROOM_ID] =
    g_param_spec_string ("room-id",
                         "room id",
                         "The room id",
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  properties[PROP_ENCRYPTED] =
    g_param_spec_string ("encrypted",
                         "encrypted",
                         "Whether room is encrypted",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_NAME] =
    g_param_spec_string ("name",
                         "Name",
                         "The room name",
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
cm_room_init (CmRoom *self)
{
  self->room_event = cm_room_event_list_new (self);
  self->one_time_keys = g_ptr_array_new_full (32, g_free);
  self->changed_users = g_ptr_array_new_full (32, g_object_unref);
  self->joined_members = g_list_store_new (CM_TYPE_ROOM_MEMBER);
  self->changed_devices = g_hash_table_new_full (g_direct_hash,
                                                 g_direct_equal,
                                                 (GDestroyNotify)g_ref_string_release,
                                                 (GDestroyNotify)g_ptr_array_unref);
  self->joined_members_table = g_hash_table_new_full (g_direct_hash,
                                                      g_direct_equal,
                                                      (GDestroyNotify)g_ref_string_release,
                                                      g_object_unref);
  self->invited_members = g_list_store_new (CM_TYPE_ROOM_MEMBER);
  self->invited_members_table = g_hash_table_new_full (g_direct_hash,
                                                       g_direct_equal,
                                                       (GDestroyNotify)g_ref_string_release,
                                                       g_object_unref);
  self->message_queue = g_queue_new ();
}

CmRoom *
cm_room_new (const char *room_id)
{
  CmRoom *self;

  g_return_val_if_fail (room_id && *room_id, NULL);

  self = g_object_new (CM_TYPE_ROOM, NULL);
  self->room_id = g_strdup (room_id);

  return self;
}

/*
 * cm_room_new_from_json:
 * @room_id: room id string
 * @root: (nullable): A JsonObject with "local" object
 * last_event: (nullable) (transfer full): A #CmEvent
 *
 * Create a new #CmRoom. @last_event is the
 * last sync event in the room.
 */
CmRoom *
cm_room_new_from_json (const char *room_id,
                       JsonObject *root,
                       CmEvent    *last_event)
{
  CmRoom *self;

  self = cm_room_new (room_id);

  if (root)
    {
      JsonObject *local;

      self->local_json = root;
      self->initial_sync_done = TRUE;
      local = cm_utils_json_object_get_object (root, "local");
      self->name = g_strdup (cm_utils_json_object_get_string (local, "alias"));
      self->generated_name = cm_utils_json_object_dup_string (local, "generated_alias");
      self->past_name = cm_utils_json_object_dup_string (local, "past_alias");
      cm_room_set_is_direct (self, cm_utils_json_object_get_bool (local, "direct"));
      self->encryption = cm_utils_json_object_dup_string (local, "encryption");

      cm_room_event_list_set_local_json (self->room_event, root, last_event);
    }

  return self;
}

static JsonObject *
room_generate_json (CmRoom *self)
{
  JsonObject *json, *child;

  g_assert (CM_IS_ROOM (self));

  json = cm_room_event_list_get_local_json (self->room_event);

  child = cm_utils_json_object_get_object (json, "local");

  json_object_set_string_member (child, "generated_alias", self->generated_name);
  if (self->past_name)
    json_object_set_string_member (child, "past_alias", self->past_name);
  json_object_set_string_member (child, "alias", cm_room_get_name (self));
  /* Alias set before the current one, may be used if current one is NULL (eg: was x) */
  json_object_set_string_member (child, "last_alias", cm_room_get_name (self));
  json_object_set_boolean_member (child, "direct", cm_room_is_direct (self));
  json_object_set_int_member (child, "encryption", cm_room_is_encrypted (self));

  return json;
}

/*
 * cm_room_get_json:
 *
 * Get the json which is to be stored
 * in db as such
 */
char *
cm_room_get_json (CmRoom *self)
{
  JsonObject *json;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  json = room_generate_json (self);

  return cm_utils_json_object_to_string (json, FALSE);
}

/*
 * cm_room_get_replacement_room:
 * @self: A #CmRoom
 *
 * Get the id of the room this room has been
 * replaced with, which means that this room
 * has been obsolete and should no longer
 * be used for conversations.
 *
 * Returns: (nullable): The replacement room
 * if this room has tombstone
 */
const char *
cm_room_get_replacement_room (CmRoom *self)
{
  CmEvent *event;

  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);

  event = cm_room_event_list_get_event (self->room_event, CM_M_ROOM_TOMBSTONE);
  if (!event)
    return NULL;

  return cm_room_event_get_replacement_room_id ((gpointer)event);
}

static void
room_user_changed (CmRoom     *self,
                   CmUser     *user,
                   GPtrArray  *added,
                   GPtrArray  *removed,
                   CmUserList *user_list)
{
  GRefString *user_id;

  g_assert (CM_IS_ROOM (self));
  g_assert (CM_IS_USER (user));
  g_assert (CM_IS_USER_LIST (user_list));

  /* User changes has to be tracked only if the room is encrypted */
  if (!cm_room_is_encrypted (self))
    return;

  user_id = cm_user_get_id (user);

  if (!g_hash_table_contains (self->joined_members_table, user_id))
    return;

  /* If any device got removed, create a new key and invalidate old */
  if (removed && removed->len)
    {
      g_hash_table_remove_all (self->changed_devices);
      self->keys_queried = FALSE;
      self->keys_claimed = FALSE;
      self->keys_uploaded = FALSE;
      return;
    }

  if (added && added->len)
    {
      GPtrArray *devices;

      devices = g_hash_table_lookup (self->changed_devices, user);

      if (devices)
        {
          g_ptr_array_extend (devices, added, (gpointer)g_object_ref, NULL);
        }
      else
        {
          g_ref_string_acquire (user_id);
          devices = g_ptr_array_copy (added, (gpointer)g_object_ref, NULL);
          g_hash_table_insert (self->changed_devices, user_id, devices);
        }
    }
}

void
cm_room_set_client (CmRoom   *self,
                    CmClient *client)
{
  CmUserList *user_list;

  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (!self->client);

  self->client = g_object_ref (client);
  user_list = cm_client_get_user_list (client);

  g_signal_connect_object (user_list, "user-changed",
                           G_CALLBACK (room_user_changed),
                           self, G_CONNECT_SWAPPED);
  cm_room_event_list_set_client (self->room_event, client);
}

CmClient *
cm_room_get_client (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  return self->client;
}

gboolean
cm_room_has_state_sync (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);

  return self->initial_sync_done;
}

/**
 * cm_room_get_id:
 * @self: A #CmRoom
 *
 * Get the matrix room id.
 *
 * Returns: The room id
 */
const char *
cm_room_get_id (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  return self->room_id;
}

gboolean
cm_room_self_has_power_for_event (CmRoom      *self,
                                  CmEventType  type)
{
  CmEvent *event;

  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);

  event = cm_room_event_list_get_event (self->room_event, CM_M_ROOM_POWER_LEVELS);
  if (!event)
    return FALSE;

  return cm_room_event_user_has_power (CM_ROOM_EVENT (event),
                                       cm_client_get_user_id (self->client),
                                       type);
}

/**
 * cm_room_get_name:
 * @self: A #CmRoom
 *
 * Get the matrix room name.  Can be %NULL
 * if the room name is not set, or the name
 * is not yet loaded.
 *
 * Returns: (nullable): The room name
 */
const char *
cm_room_get_name (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  if (!self->generated_name && !self->name)
    {
      g_autofree char *name = NULL;

      name = cm_room_generate_name (self);
      cm_room_set_generated_name (self, name);
      cm_room_save (self);
    }

  if (self->name)
    return self->name;

  return self->generated_name;
}

/**
 * cm_room_get_past_name:
 * @self: A #CmRoom
 *
 * Get the matrix room name set before the current one.
 * The past name is set only if the room is a direct
 * room and current room name is empty (in the case
 * where cm_room_get_name() shall return "Empty room")
 *
 * Returns: (nullable): The past room name
 */
const char *
cm_room_get_past_name (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);
  /* g_warning ("%s %s", self->past_name, self->name); */

  if (!self->name &&
      g_strcmp0 (self->generated_name, "Empty room") == 0)
    return self->past_name;

  return NULL;
}

/**
 * cm_room_is_encrypted:
 * @self: A #CmRoom
 *
 * Get if the matrix room @self is encrypted
 * or not.
 *
 * Returns: %TRUE if @self is encrypted.
 * %FALSE otherwise.
 */
gboolean
cm_room_is_encrypted (CmRoom *self)
{
  CmEvent *event;

  g_return_val_if_fail (CM_IS_ROOM (self), TRUE);

  if (self->encryption)
    return TRUE;

  event = cm_room_event_list_get_event (self->room_event, CM_M_ROOM_ENCRYPTION);

  return !!event;
}

GListModel *
cm_room_get_joined_members (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  return G_LIST_MODEL (self->joined_members);
}

GListModel *
cm_room_get_events_list (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  return cm_room_event_list_get_events (self->room_event);
}

CmStatus
cm_room_get_status (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), CM_STATUS_UNKNOWN);

  return self->room_status;
}

void
cm_room_set_status (CmRoom   *self,
                    CmStatus  status)
{
  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (status == CM_STATUS_INVITE ||
                    status == CM_STATUS_JOIN ||
                    status == CM_STATUS_LEAVE);

  if (self->room_status == status)
    return;

  self->room_status = status;

  if (self->client)
    {
      self->db_save_pending = TRUE;
      cm_room_save (self);
    }
}

JsonObject *
cm_room_decrypt (CmRoom     *self,
                 JsonObject *root)
{
  char *plain_text = NULL;
  JsonObject *content;
  CmEnc *enc;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  enc = cm_client_get_enc (self->client);

  if (!enc || !root)
    return NULL;

  content = cm_utils_json_object_get_object (root, "content");
  plain_text = cm_enc_handle_join_room_encrypted (enc, self, content);

  return cm_utils_string_to_json_object (plain_text);
}

void
cm_room_add_events (CmRoom    *self,
                    GPtrArray *events,
                    gboolean   append)
{
  g_return_if_fail (CM_IS_ROOM (self));

  cm_room_event_list_add_events (self->room_event, events, append);
}

GPtrArray *
cm_room_set_data (CmRoom     *self,
                  JsonObject *object)
{
  g_autoptr(GPtrArray) events = NULL;
  JsonObject *child;
  JsonArray *array;
  guint length = 0;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);
  g_return_val_if_fail (object, NULL);

  events = g_ptr_array_new_full (100, g_object_unref);
  child = cm_utils_json_object_get_object (object, "state");
  cm_room_event_list_parse_events (self->room_event, child, NULL, FALSE);

  child = cm_utils_json_object_get_object (object, "timeline");
  cm_room_event_list_parse_events (self->room_event, child, events, FALSE);

  if (cm_utils_json_object_get_bool (child, "limited"))
    {
      const char *prev;

      prev = cm_utils_json_object_get_string (child, "prev_batch");
      cm_room_set_prev_batch (self, prev);
    }
  self->db_save_pending = TRUE;

  length = 0;
  array = cm_utils_json_object_get_array (object, "left");

  if (array)
    length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      g_autoptr(GRefString) user_id = NULL;
      CmRoomMember *member;
      const char *member_id;

      member_id = json_array_get_string_element (array, i);
      user_id = g_ref_string_new_intern (member_id);
      member = g_hash_table_lookup (self->joined_members_table, user_id);

      if (member)
        {
          g_hash_table_remove (self->joined_members_table, user_id);
          cm_utils_remove_list_item (self->joined_members, member);
          self->keys_queried = FALSE;
          self->keys_claimed = FALSE;
          self->keys_uploaded = FALSE;
        }
    }

  self->initial_sync_done = TRUE;
  cm_room_save (self);

  return g_steal_pointer (&events);
}

/*
 * cm_room_user_changed:
 * @self: A #CmRoom
 * @changed_users: An array of #CmUser
 *
 * Inform that the user devices for @changed_users has
 * has changed. The function simply returns if any of
 * the user in @changed_users is not in the room
 *
 * This is useful for encrypted rooms where the user
 * devices has to be updated before sending anything
 * encrypted.
 */
void
cm_room_user_changed (CmRoom     *self,
                      GPtrArray  *changed_users)
{
  GRefString *user_id;
  CmUser *user;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (changed_users);

  /* We need to track the user changes only if room is encrypted */
  if (!cm_room_is_encrypted (self))
    return;

  for (guint i = 0; i < changed_users->len; i++)
    {
      user = changed_users->pdata[i];
      user_id = cm_user_get_id (user);

      if (g_hash_table_contains (self->joined_members_table, user_id) &&
          !g_ptr_array_find (self->changed_users, user, NULL))
        g_ptr_array_add (self->changed_users, g_object_ref (user));
    }
}

const char *
cm_room_get_prev_batch (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  return self->prev_batch;
}

void
cm_room_set_prev_batch (CmRoom     *self,
                        const char *prev_batch)
{
  g_return_if_fail (CM_IS_ROOM (self));

  g_free (self->prev_batch);
  self->prev_batch = g_strdup (prev_batch);
}

void
cm_room_set_name (CmRoom     *self,
                  const char *name)
{
  JsonObject *json, *child;

  g_return_if_fail (CM_IS_ROOM (self));

  if (g_strcmp0 (name, self->name) == 0)
    return;

  g_free (self->name);
  self->name = g_strdup (name);

  json = room_generate_json (self);
  child = cm_utils_json_object_get_object (json, "local");

  if (name)
    json_object_set_string_member (child, "alias", name);
  else
    json_object_remove_member (child, "alias");

  self->db_save_pending = TRUE;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
}

void
cm_room_set_generated_name (CmRoom     *self,
                            const char *name)
{
  g_return_if_fail (CM_IS_ROOM (self));

  if (g_strcmp0 (name, self->generated_name) == 0)
    return;

  g_free (self->generated_name);
  self->generated_name = g_strdup (name);

  if (self->local_json)
    {
      JsonObject *child;

      child = cm_utils_json_object_get_object (self->local_json, "local");
      json_object_set_string_member (child, "generated_alias", name);
    }

  self->db_save_pending = TRUE;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
}

/* cm_room_get_encryption_rotation_time:
 *
 * Return the time in milliseconds after which the key
 * should be rotated after the first key use.
 *
 * The key should be regenerated regardless of whether
 * the maximum message count for the rotation has reached
 * or not.
 */
gint64
cm_room_get_encryption_rotation_time (CmRoom *self)
{
  CmEvent *event;

  g_return_val_if_fail (CM_IS_ROOM (self), 60 * 60 * 24 * 7);

  event = cm_room_event_list_get_event (self->room_event, CM_M_ROOM_ENCRYPTION);
  if (!event)
    return 60 * 60 * 24 * 7;

  return cm_room_event_get_rotation_time ((gpointer)event);
}

/* cm_room_get_encryption_msg_count:
 *
 * Return the number of messages after which the encryption
 * key for sending messages in the room should be rotated.
 */
guint
cm_room_get_encryption_msg_count (CmRoom *self)
{
  CmEvent *event;

  g_return_val_if_fail (CM_IS_ROOM (self), 100);

  event = cm_room_event_list_get_event (self->room_event, CM_M_ROOM_ENCRYPTION);
  if (!event)
    return 100;

  return cm_room_event_get_rotation_count ((gpointer)event);
}

gboolean
cm_room_is_direct (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);

  return self->is_direct;
}

void
cm_room_set_is_direct (CmRoom   *self,
                       gboolean  is_direct)
{
  g_return_if_fail (CM_IS_ROOM (self));

  self->is_direct = !!is_direct;
}

static void
send_cb (GObject      *obj,
         GAsyncResult *result,
         gpointer      user_data)
{
  CmRoom *self;
  GTask *message_task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;
  const char *event_id = NULL;
  CmEvent *event;

  g_assert (G_IS_TASK (message_task));

  self = g_task_get_source_object (message_task);
  event = g_task_get_task_data (message_task);
  object = g_task_propagate_pointer (G_TASK (result), &error);

  event_id = cm_utils_json_object_get_string (object, "event_id");
  g_debug ("Sending message, has-error: %d. event-id: %s",
           !!error, event_id);

  self->is_sending_message = FALSE;

  if (error)
    {
      cm_event_set_state (event, CM_EVENT_STATE_SENDING_FAILED);
      g_debug ("Error sending message: %s", error->message);
      g_task_return_error (message_task, error);
    }
  else
    {
      cm_event_set_state (event, CM_EVENT_STATE_SENT);
      room_add_event_to_db (self, event);

      /* Set event after saving to db so that event id is not stored in db
       * and we replace id less events when we sync, so that the event is
       * placed in the right order.
       */
      cm_event_set_id (event, event_id);

      g_task_return_pointer (message_task, g_strdup (event_id), g_free);
    }
}

static void
room_send_file_cb (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;
  CmInputStream *stream;
  char *mxc_uri = NULL;
  GTask *message_task;
  GFile *message_file;
  CmRoomMessageEvent *message;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  message_task = g_task_get_task_data (task);

  g_assert (CM_IS_ROOM (self));
  g_assert (G_TASK (message_task));

  mxc_uri = cm_net_put_file_finish (CM_NET (object), result, &error);

  if (!mxc_uri)
    {
      self->is_sending_message = FALSE;

      g_task_return_new_error (message_task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to upload file: %s", error->message ?: "");
      room_send_message_from_queue (self);

      return;
    }

  stream = g_object_get_data (G_OBJECT (result), "stream");
  g_assert (stream);
  g_object_ref (stream);

  message = g_task_get_task_data (message_task);
  g_assert (CM_IS_ROOM_MESSAGE_EVENT (message));

  message_file = cm_room_message_event_get_file (message);
  g_assert (G_IS_FILE (message_file));

  g_object_set_data_full (G_OBJECT (message_file), "uri", mxc_uri, g_free);
  g_object_set_data_full (G_OBJECT (message_file), "stream", stream, g_object_unref);
  g_object_set_data_full (G_OBJECT (stream), "uri", g_strdup (mxc_uri), g_free);

  uri = cm_event_get_api_url (CM_EVENT (message), self);

  cm_net_send_json_async (cm_client_get_net (self->client), 0,
                          cm_event_generate_json (CM_EVENT (message), self),
                          uri, SOUP_METHOD_PUT, NULL, g_task_get_cancellable (message_task),
                          send_cb, message_task);
}

static void
room_send_message_from_queue (CmRoom *self)
{
  CmRoomMessageEvent *message;
  GTask *message_task;
  g_autofree char *uri = NULL;

  g_assert (CM_IS_ROOM (self));

  message_task = g_queue_peek_head (self->message_queue);

  if (!message_task)
    return;

  if (self->is_sending_message || self->retry_timeout_id)
    return;

  if (cm_room_is_encrypted (self) && !self->keys_uploaded)
    {
      ensure_encryption_keys (self);
      return;
    }

  self->is_sending_message = TRUE;
  message_task = g_queue_pop_head (self->message_queue);
  message = g_task_get_task_data (message_task);
  g_assert (CM_IS_ROOM_MESSAGE_EVENT (message));

  if (cm_room_message_event_get_msg_type (message) == CM_CONTENT_TYPE_FILE)
    {
      GFileProgressCallback progress_cb;
      gpointer progress_user_data;
      GTask *task;

      progress_cb = g_object_get_data (G_OBJECT (message_task), "progress-cb");
      progress_user_data = g_object_get_data (G_OBJECT (message_task), "progress-cb-data");

      task = g_task_new (self, NULL, NULL, NULL);
      g_task_set_task_data (task, g_object_ref (message_task), g_object_unref);
      cm_net_put_file_async (cm_client_get_net (self->client),
                             cm_room_message_event_get_file (message),
                             cm_room_is_encrypted (self),
                             progress_cb, progress_user_data,
                             g_task_get_cancellable (message_task),
                             room_send_file_cb, task);
      return;
    }

  uri = cm_event_get_api_url (CM_EVENT (message), self);

  cm_event_set_state (CM_EVENT (message), CM_EVENT_STATE_SENDING);
  cm_net_send_json_async (cm_client_get_net (self->client), 0,
                          cm_event_generate_json (CM_EVENT (message), self),
                          uri, SOUP_METHOD_PUT, NULL, g_task_get_cancellable (message_task),
                          send_cb, message_task);
}

/* todo */
#if 0
static gboolean
room_resend_message (gpointer user_data)
{
  CmRoom *self = user_data;

  g_assert (CM_IS_ROOM (self));

  self->retry_timeout_id = 0;
  room_send_message_from_queue (self);

  return G_SOURCE_REMOVE;
}
#endif

/**
 * cm_room_send_text_async:
 * @self: A #CmRoom
 * @text: The text message to send
 * @cancellable: (nullable): A #Gcancellable
 * @callback: A #GasyncReadyCallback
 * @user_data: The user data for @callback.
 *
 * Send @text as a text message to the room
 * @self.  If the room is encrypted, the text
 * shall be encrypted and sent.
 *
 * Returns: The event id string used for the event.
 * This can be used to track the event when received
 * via the /sync callback or so.
 */
const char *
cm_room_send_text_async (CmRoom              *self,
                         const char          *text,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  CmRoomMessageEvent *message;
  CmUser *user;
  GTask *task;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  task = g_task_new (self, cancellable, callback, user_data);
  message = cm_room_message_event_new (CM_CONTENT_TYPE_TEXT);
  cm_event_set_state (CM_EVENT (message), CM_EVENT_STATE_WAITING);
  cm_room_message_event_set_body (message, text);
  cm_event_create_txn_id (CM_EVENT (message),
                          cm_client_pop_event_id (self->client));
  g_task_set_task_data (task, message, g_object_unref);

  user = room_find_user (self, cm_client_get_user_id (self->client), TRUE);
  cm_event_set_sender (CM_EVENT (message), user);

  cm_room_event_list_append_event (self->room_event, CM_EVENT (message));
  room_add_event_to_db (self, CM_EVENT (message));

  g_queue_push_tail (self->message_queue, task);

  room_send_message_from_queue (self);

  return cm_event_get_txn_id (CM_EVENT (message));
}

/**
 * cm_room_send_text_finish:
 * @self: A #CmRoom
 * @result: A #GAsyncResult
 * @error: (nullable): A #GError
 *
 * Finish the call to cm_room_send_text_async().
 *
 * Returns: The event id string used for the event
 * on success. %NULL on error.  This is the same
 * as the one you get from cm_room_send_text_async().
 * Free with g_free().
 */
char *
cm_room_send_text_finish (CmRoom       *self,
                          GAsyncResult *result,
                          GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

const char *
cm_room_send_file_async (CmRoom                *self,
                         GFile                 *file,
                         const char            *body,
                         GFileProgressCallback  progress_callback,
                         gpointer               progress_user_data,
                         GCancellable          *cancellable,
                         GAsyncReadyCallback    callback,
                         gpointer               user_data)
{
  CmRoomMessageEvent *message;
  CmUser *user;
  GTask *task;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data (G_OBJECT (task), "progress-cb", progress_callback);
  g_object_set_data (G_OBJECT (task), "progress-cb-data", progress_user_data);

  message = cm_room_message_event_new (CM_CONTENT_TYPE_FILE);
  cm_event_set_state (CM_EVENT (message), CM_EVENT_STATE_WAITING);
  cm_room_message_event_set_file (message, body, file);
  cm_event_create_txn_id (CM_EVENT (message),
                          cm_client_pop_event_id (self->client));
  g_task_set_task_data (task, message, g_object_unref);

  user = room_find_user (self, cm_client_get_user_id (self->client), TRUE);
  cm_event_set_sender (CM_EVENT (message), user);
  cm_room_event_list_append_event (self->room_event, CM_EVENT (message));

  g_queue_push_tail (self->message_queue, task);

  room_send_message_from_queue (self);

  return cm_event_get_txn_id (CM_EVENT (message));
}

char *
cm_room_send_file_finish (CmRoom        *self,
                          GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
send_typing_cb (GObject      *obj,
                GAsyncResult *result,
                gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_TASK (task));
  self = g_task_get_source_object (task);

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      self->typing = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "was-typing"));
      self->typing_set_time = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (task), "was-typing-time"));
      g_warning ("Error set typing: %s", error->message);
    }
}

/**
 * cm_room_set_typing_notice_async:
 * @self: A #CmRoom
 * @typing: set/unset typing
 * @cancellable: (nullable): A #Gcancellable
 * @callback: A #GasyncReadyCallback
 * @user_data: The user data for @callback.
 *
 * Set/Unset if the self user is typing or not.
 * The typing set is timeout after 4 seconds.
 */
void
cm_room_set_typing_notice_async (CmRoom              *self,
                                 gboolean             typing,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_autofree char *uri = NULL;
  JsonObject *object;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data (G_OBJECT (task), "was-typing", GINT_TO_POINTER (self->typing));
  g_object_set_data (G_OBJECT (task), "was-typing-time", GSIZE_TO_POINTER (self->typing_set_time));

  if (typing == self->typing &&
      g_get_monotonic_time () - self->typing_set_time < TYPING_TIMEOUT * G_USEC_PER_SEC)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  self->typing_set_time = g_get_monotonic_time ();
  self->typing = !!typing;

  /* https://matrix.org/docs/spec/client_server/r0.6.1#put-matrix-client-r0-rooms-roomid-typing-userid */
  object = json_object_new ();
  json_object_set_boolean_member (object, "typing", !!typing);
  if (typing)
    json_object_set_int_member (object, "timeout", TYPING_TIMEOUT);

  uri = g_strconcat ("/_matrix/client/r0/rooms/", self->room_id,
                     "/typing/", cm_client_get_user_id (self->client), NULL);

  cm_net_send_json_async (cm_client_get_net (self->client), 0, object,
                          uri, SOUP_METHOD_PUT,
                          NULL, cancellable, send_typing_cb, task);
}

/**
 * cm_room_set_typing_notice_finish:
 * @self: A #CmRoom
 * @result: A #GAsyncResult
 * @error: (nullable): A #GError
 *
 * Finish the call to cm_room_set_typing_notice_async().
 *
 * Returns: %TRUE if typing notice was successfully
 * set, %FALSE otherwise.
 */
gboolean
cm_room_set_typing_notice_finish (CmRoom        *self,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
room_set_encryption_cb (GObject      *obj,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  self = g_task_get_source_object (task);
  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_task_return_error (task, error);
    }
  else
    {
      const char *event;

      event = cm_utils_json_object_get_string (object, "event_id");
      self->encryption = g_strdup ("encrypted");
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ENCRYPTED]);
      self->db_save_pending = TRUE;
      cm_room_save (self);
      g_task_return_boolean (task, !!event);
    }
}

/**
 * cm_room_enable_encryption_async:
 * @self: A #CmRoom
 * @cancellable: (nullable): A #GCancellable
 * @callback: A #GAsyncReadyCallback
 * @user_data: user data passed to @callback
 *
 * Enable encryption for @self.  You can't disable
 * encryption once enabled.  Also, it's a noop
 * if @self has already enabled encryption.  The
 * @callback shall run in any case.
 *
 * To get the result, finish the call with
 * cm_room_enable_encryption_finish().
 */
void
cm_room_enable_encryption_async (CmRoom              *self,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    g_autofree char *uri = NULL;
    JsonObject *object;
    GTask *task;

    g_return_if_fail (CM_IS_ROOM (self));

    task = g_task_new (self, cancellable, callback, user_data);

    if (cm_room_is_encrypted (self))
      {
        g_task_return_boolean (task, TRUE);
        return;
      }

    object = json_object_new ();
    json_object_set_string_member (object, "algorithm", ALGORITHM_MEGOLM);
    uri = g_strconcat ("/_matrix/client/r0/rooms/", self->room_id, "/state/m.room.encryption", NULL);
    cm_net_send_json_async (cm_client_get_net (self->client), 2, object, uri, SOUP_METHOD_PUT,
                            NULL, cancellable, room_set_encryption_cb, task);
}

/**
 * cm_room_enable_encryption_finish:
 * @self: A #CmRoom
 * @result: A #GAsyncResult
 * @error: (nullable): A #GError
 *
 * Finish the call to cm_room_enable_encryption_async().
 *
 * Returns: %TRUE if encryption was enabled,
 * %FALSE otherwise.
 */
gboolean
cm_room_enable_encryption_finish (CmRoom        *self,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
room_leave_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("Error leaving room: %s", error->message);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

void
cm_room_leave_async (CmRoom              *self,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    GTask *task;
    g_autofree char *uri = NULL;

    g_return_if_fail (CM_IS_ROOM (self));

    task = g_task_new (self, cancellable, callback, user_data);
    uri = g_strdup_printf ("/_matrix/client/r0/rooms/%s/leave", self->room_id);
    cm_net_send_json_async (cm_client_get_net (self->client), 1, NULL,
                            uri, SOUP_METHOD_POST,
                            NULL, cancellable, room_leave_cb, task);
}

gboolean
cm_room_leave_finish (CmRoom        *self,
                      GAsyncResult  *result,
                      GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
room_set_read_marker_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

void
cm_room_set_read_marker_async (CmRoom              *self,
                               CmEvent             *fully_read_event,
                               CmEvent             *read_receipt_event,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  const char *fully_read_id, *read_receipt_id;
  g_autofree char *uri = NULL;
  JsonObject *root;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (CM_IS_EVENT (fully_read_event));
  g_return_if_fail (CM_IS_EVENT (read_receipt_event));

  fully_read_id = cm_event_get_id (fully_read_event);
  read_receipt_id = cm_event_get_id (read_receipt_event);

  root = json_object_new ();
  json_object_set_string_member (root, "m.fully_read", fully_read_id);
  json_object_set_string_member (root, "m.read", read_receipt_id);

  task = g_task_new (self, NULL, callback, user_data);

  uri = g_strdup_printf ("/_matrix/client/r0/rooms/%s/read_markers", self->room_id);
  cm_net_send_json_async (cm_client_get_net (self->client), 0, root,
                          uri, SOUP_METHOD_POST,
                          NULL, NULL, room_set_read_marker_cb, task);
}

gboolean
cm_room_set_read_marker_finish (CmRoom        *self,
                                GAsyncResult  *result,
                                GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
room_load_prev_batch_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GPtrArray *events = NULL;
  GError *error = NULL;
  const char *end;

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_warning ("Error getting members: %s", error->message);
      g_task_return_error (task, error);
      return;
    }

  end = cm_utils_json_object_get_string (object, "end");

  /* If start and end are same, we have reached the start of room history */
  if (g_strcmp0 (cm_utils_json_object_get_string (object, "end"),
                 cm_utils_json_object_get_string (object, "start")) == 0)
    end = NULL;

  cm_room_set_prev_batch (self, end);
  self->db_save_pending = TRUE;
  cm_room_save (self);

  events = g_ptr_array_new_full (64, g_object_unref);
  cm_room_event_list_parse_events (self->room_event, object, events, TRUE);
  cm_db_add_room_events (cm_client_get_db (self->client), self, events, TRUE);

  g_task_return_pointer (task, events, (GDestroyNotify)g_ptr_array_unref);
}

void
cm_room_load_prev_batch_async (CmRoom              *self,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autofree char *uri = NULL;
  const char *prev_batch;
  GHashTable *query;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM (self));

  task = g_task_new (self, cancellable, callback, user_data);
  prev_batch = cm_room_get_prev_batch (self);

  if (!prev_batch)
    {
      g_task_return_pointer (task, NULL, NULL);
      return;
    }

  /* Create a query to get past 30 messages */
  query = g_hash_table_new_full (g_str_hash, g_str_equal, free,
                                 (GDestroyNotify)cm_utils_free_buffer);
  g_hash_table_insert (query, g_strdup ("from"), g_strdup (prev_batch));
  g_hash_table_insert (query, g_strdup ("dir"), g_strdup ("b"));
  g_hash_table_insert (query, g_strdup ("limit"), g_strdup ("30"));
  /* if (upto_batch) */
  /*   g_hash_table_insert (query, g_strdup ("to"), g_strdup (upto_batch)); */

  /* https://matrix.org/docs/spec/client_server/r0.6.1#get-matrix-client-r0-rooms-roomid-messages */
  uri = g_strconcat ("/_matrix/client/r0/rooms/", self->room_id, "/messages", NULL);
  cm_net_send_json_async (cm_client_get_net (self->client), 0, NULL,
                          uri, SOUP_METHOD_GET,
                          query, cancellable, room_load_prev_batch_cb, task);
}

GPtrArray *
cm_room_load_prev_batch_finish (CmRoom        *self,
                                GAsyncResult  *result,
                                GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
room_prev_batch_cb (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  GPtrArray *events = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  events = cm_room_load_prev_batch_finish (self, result, &error);
  self->loading_past_events = FALSE;

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, !!events);
}

static void
room_get_past_db_events_cb (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GPtrArray) events = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  events = cm_db_get_past_events_finish (CM_DB (object), result, &error);
  self->loading_past_events = FALSE;

  if (events && events->len)
    {
      cm_room_add_events (self, events, FALSE);
      g_task_return_boolean (task, TRUE);
    }
  else if (self->prev_batch)
    {
      self->loading_past_events = TRUE;
      cm_room_load_prev_batch_async (self, NULL, room_prev_batch_cb,
                                     g_steal_pointer (&task));
    }
  else
    {
      g_task_return_boolean (task, FALSE);
    }
}

static void
room_load_sync_cb (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  GAsyncReadyCallback callback;
  gpointer cb_user_data;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  cm_room_load_finish (self, result, &error);
  g_debug ("Loading room initial sync before past events done, has-error: %d", !!error);

  if (error)
    {
      g_task_return_error (task, error);
      return;
    }

  callback = g_object_get_data (G_OBJECT (task), "callback");
  cb_user_data = g_object_get_data (G_OBJECT (task), "cb-user-data");
  cm_room_load_past_events_async (self, callback, cb_user_data);
}

void
cm_room_load_past_events_async (CmRoom              *self,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr(CmEvent) from = NULL;
  g_autoptr(GTask) task = NULL;
  GListModel *events;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (!from || CM_IS_ROOM_EVENT (from));

  task = g_task_new (self, NULL, callback, user_data);
  g_object_set_data (G_OBJECT (task), "callback", callback);
  g_object_set_data (G_OBJECT (task), "cb-user-data", user_data);

  if (self->loading_initial_sync || self->loading_past_events)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "Past events are being already loaded");
      return;
    }

  if (!self->initial_sync_done)
    {
      g_debug ("Loading room initial sync before loading past events");
      cm_room_load_async (self, NULL,
                          room_load_sync_cb,
                          g_steal_pointer (&task));
      return;
    }

  self->loading_past_events = TRUE;

  events = cm_room_event_list_get_events (self->room_event);
  from = g_list_model_get_item (events, 0);
  cm_db_get_past_events_async (cm_client_get_db (self->client),
                               self, from,
                               room_get_past_db_events_cb,
                               g_steal_pointer (&task));
}

gboolean
cm_room_load_past_events_finish (CmRoom        *self,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
get_joined_members_cb (GObject      *obj,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      self->joined_members_loading = FALSE;
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Error getting room members: %s", error->message);
      g_task_return_error (task, error);
    }
  else
    {
      g_autoptr(GList) members = NULL;
      JsonObject *joined;

      joined = cm_utils_json_object_get_object (object, "joined");
      members = json_object_get_members (joined);

      for (GList *member = members; member; member = member->next)
        {
          g_autoptr(GRefString) user_id = NULL;
          CmUser *user;
          JsonObject *data;

          user_id = g_ref_string_new_intern (member->data);
          user = g_hash_table_lookup (self->joined_members_table, user_id);

          if (!user)
            {
              CmUserList *user_list;

              user_list = cm_client_get_user_list (self->client);
              user = cm_user_list_find_user (user_list, user_id, TRUE);
              g_ptr_array_add (self->changed_users, g_object_ref (user));
              g_hash_table_insert (self->joined_members_table,
                                   g_ref_string_acquire (user_id),
                                   g_object_ref (user));
              g_list_store_append (self->joined_members, user);
            }

          data = json_object_get_object_member (joined, member->data);
          cm_room_member_set_json_data (CM_ROOM_MEMBER (user), data);
        }

      self->joined_members_loaded = TRUE;
      self->joined_members_loading = FALSE;
      g_task_return_pointer (task, self->joined_members, NULL);

      /* TODO: handle failures */
      room_send_message_from_queue (self);
    }
}

void
cm_room_load_joined_members_async (CmRoom              *self,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_autofree char *uri = NULL;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM (self));

  task = g_task_new (self, cancellable, callback, user_data);

  if (self->joined_members_loaded)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  if (self->joined_members_loading)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Members list are already loading");
      return;
    }

  self->joined_members_loading = TRUE;

  uri = g_strconcat ("/_matrix/client/r0/rooms/", self->room_id, "/joined_members", NULL);
  cm_net_send_json_async (cm_client_get_net (self->client), -1, NULL, uri, SOUP_METHOD_GET,
                          NULL, cancellable, get_joined_members_cb, task);
}

gboolean
cm_room_load_joined_members_finish (CmRoom        *self,
                                    GAsyncResult  *result,
                                    GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
get_room_state_cb (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(CmRoom) self = user_data;
  g_autoptr(JsonObject) root = NULL;
  GError *error = NULL;
  JsonArray *array;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  array = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("Loading room initial sync done, has-error: %d", !!error);
  self->loading_initial_sync = FALSE;

  if (error)
    {
      g_task_return_error (task, error);
      return;
    }

  root = json_object_new ();
  json_object_set_array_member (root, "events", array);
  cm_room_event_list_parse_events (self->room_event, root, NULL, FALSE);
  self->initial_sync_done = TRUE;

  self->db_save_pending = TRUE;
  cm_room_save (self);
  g_task_return_boolean (task, TRUE);
}

void
cm_room_load_async (CmRoom              *self,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree char *uri = NULL;

  task = g_task_new (self, cancellable, callback, user_data);

  if (self->initial_sync_done)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  if (self->loading_initial_sync)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "room initial sync is already in progress");
      return;
    }

  g_debug ("Loading room initial sync");

  self->loading_initial_sync = TRUE;
  uri = g_strconcat ("/_matrix/client/r0/rooms/", self->room_id, "/state", NULL);
  cm_net_send_json_async (cm_client_get_net (self->client), 0, NULL,
                          uri, SOUP_METHOD_GET,
                          NULL, NULL, get_room_state_cb,
                          g_steal_pointer (&task));
}

gboolean
cm_room_load_finish (CmRoom        *self,
                     GAsyncResult  *result,
                     GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
save_room_cb (GObject      *object,
              GAsyncResult *result,
              gpointer      user_data)
{
  g_autoptr(CmRoom) self = user_data;
  g_autoptr(GError) error = NULL;

  if (!cm_db_save_room_finish (CM_DB (object), result, &error))
    {
      cm_room_event_list_set_save_pending (self->room_event, TRUE);
      self->db_save_pending = TRUE;
      g_warning ("Error saving room details: %s", error->message);
    }
}

void
cm_room_save (CmRoom *self)
{
  g_return_if_fail (CM_IS_ROOM (self));

  if (!self->db_save_pending)
    return;

  self->db_save_pending = FALSE;
  cm_room_event_list_set_save_pending (self->room_event, FALSE);
  cm_db_save_room_async (cm_client_get_db (self->client), self->client, self,
                         save_room_cb,
                         g_object_ref (self));
}

CmUser *
cm_room_find_user (CmRoom     *self,
                   GRefString *matrix_id,
                   gboolean    add_if_missing)
{
  return room_find_user (self, matrix_id, add_if_missing);
}

void
cm_room_update_user (CmRoom  *self,
                     CmEvent *event)
{
  g_autoptr(JsonObject) child = NULL;
  CmUserList *user_list;
  CmUser *member = NULL;
  GRefString *user_id;
  CmStatus member_status;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (CM_IS_EVENT (event));
  g_return_if_fail (cm_event_get_m_type (event) == CM_M_ROOM_MEMBER);

  member_status = cm_room_event_get_status (CM_ROOM_EVENT (event));

  if (member_status == CM_STATUS_JOIN &&
      cm_event_get_sender_id (event) ==
      cm_client_get_user_id (self->client))
    return;

  child = cm_event_get_json (event);
  g_return_if_fail (child);

  user_id = cm_room_event_get_room_member_id (CM_ROOM_EVENT (event));
  user_list = cm_client_get_user_list (self->client);
  member = cm_user_list_find_user (user_list, user_id, TRUE);
  cm_room_member_set_json_data (CM_ROOM_MEMBER (member), child);

  g_debug ("(%p) Updating user member status, user: %p, %s, status: %d",
           self, member, user_id, member_status);

  if (member_status == CM_STATUS_JOIN)
    {
      CmRoomMember *invite;

      invite = g_hash_table_lookup (self->invited_members_table, user_id);
      if (invite)
        {
          g_hash_table_remove (self->invited_members_table, user_id);
          cm_utils_remove_list_item (self->invited_members, invite);
        }

      if (g_hash_table_contains (self->joined_members_table, user_id))
        {
          CmRoomMember *cm_member;

          cm_member = g_hash_table_lookup (self->joined_members_table, user_id);
          cm_room_member_set_json_data (cm_member, child);

          g_free (self->past_name);
          self->past_name = g_steal_pointer (&self->generated_name);
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
          return;
        }

      g_list_store_append (self->joined_members, member);
      g_hash_table_insert (self->joined_members_table,
                           g_ref_string_acquire (user_id),
                           g_object_ref (member));

      /* Clear the name so that it will be regenerated when name is requested */
      g_free (self->past_name);
      self->past_name = g_steal_pointer (&self->generated_name);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
    }
  else if (member_status == CM_STATUS_INVITE)
    {
      if (g_hash_table_contains (self->invited_members_table, user_id))
        {
          CmRoomMember *cm_member;

          cm_member = g_hash_table_lookup (self->invited_members_table, user_id);
          cm_room_member_set_json_data (cm_member, child);

          /* Clear the name so that it will be regenerated when name is requested */
          g_free (self->past_name);
          self->past_name = g_steal_pointer (&self->generated_name);
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
          return;
        }

      g_list_store_append (self->invited_members, member);
      g_hash_table_insert (self->invited_members_table,
                           g_ref_string_acquire (user_id),
                           g_object_ref (member));
      self->db_save_pending = TRUE;

      /* Clear the name so that it will be regenerated when name is requested */
      g_free (self->past_name);
      self->past_name = g_steal_pointer (&self->generated_name);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
    }
  else if (member_status == CM_STATUS_LEAVE)
    {
      CmRoomMember *join;

      /* Generate a name if it doesn't exist so that we can
       * use it as the past name if the new name is empty
       */
      if (!self->name && !self->generated_name)
        self->generated_name = cm_room_generate_name (self);

      join = g_hash_table_lookup (self->joined_members_table, user_id);

      if (join)
        {
          g_hash_table_remove (self->joined_members_table, user_id);
          cm_utils_remove_list_item (self->joined_members, join);
        }

      /* Clear the name so that it will be regenerated when name is requested */
      g_free (self->past_name);
      self->past_name = g_steal_pointer (&self->generated_name);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
      self->db_save_pending = TRUE;
    }
}
