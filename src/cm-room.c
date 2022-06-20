/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-client-private.h"
#include "cm-utils-private.h"
#include "cm-net-private.h"
#include "cm-enc-private.h"
#include "cm-common.h"
#include "cm-room-member-private.h"
#include "cm-room-member.h"
#include "cm-room-private.h"
#include "cm-room.h"

typedef struct _Message
{
  GTask *task;
  char  *text;
  char  *event_id;
  GFile *file;
  JsonObject *object;
} Message;

#define KEY_TIMEOUT         10000 /* milliseconds */
#define TYPING_TIMEOUT      4     /* seconds */

struct _CmRoom
{
  GObject parent_instance;

  GListStore *joined_members;
  GHashTable *joined_members_table;
  CmClient   *client;
  char       *name;
  char       *room_id;
  char       *encryption;
  char       *prev_batch;

  GQueue     *message_queue;
  guint       retry_timeout_id;

  gboolean    is_direct;

  /* Use g_get_monotonic_time(), we only need the interval */
  gint64     typing_set_time;
  /* set doesn't mean the user has typing state set,
   * also compare with typing_set_time and TYPING_TIMEOUT */
  gboolean   typing;

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

enum {
  EVENT_RECEIVED,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

/* static gboolean room_resend_message          (gpointer user_data); */
static void     room_send_message_from_queue (CmRoom *self);

static CmRoomMember *
room_find_member (CmRoom     *self,
                  GListModel *model,
                  const char *matrix_id)
{
  guint n_items;

  g_assert (CM_IS_ROOM (self));
  g_assert (G_IS_LIST_MODEL (model));

  if (!matrix_id || *matrix_id != '@')
    return NULL;

  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmRoomMember) member = NULL;
      const char *user_id;

      member = g_list_model_get_item (model, i);
      user_id = cm_room_member_get_user_id (member);

      if (g_strcmp0 (user_id, matrix_id) == 0)
        return member;
    }

  return NULL;
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
      g_value_set_boolean (value, !!self->encryption);
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

  g_hash_table_unref (self->joined_members_table);
  g_clear_object (&self->joined_members);

  g_clear_object (&self->client);
  g_free (self->room_id);

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

  signals [EVENT_RECEIVED] =
    g_signal_new ("event-received",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
cm_room_init (CmRoom *self)
{
  self->joined_members = g_list_store_new (CM_TYPE_ROOM_MEMBER);
  self->joined_members_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, g_object_unref);
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

void
cm_room_set_client (CmRoom   *self,
                    CmClient *client)
{
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (!self->client);

  self->client = g_object_ref (client);
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

/**
 * cm_room_get_id:
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

  return self->name;
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
  g_return_val_if_fail (CM_IS_ROOM (self), TRUE);

  return !!self->encryption;
}

CmRoomType
cm_room_get_room_type (CmRoom *self)
{
  g_return_val_if_fail (CM_IS_ROOM (self), CM_ROOM_UNKNOWN);

  return CM_ROOM_UNKNOWN;
}

/**
 * cm_room_decrypt_content:
 * @self: A #CmRoom
 * @json_str: The JSON string to decrypt
 *
 * Decrypt a JSON string event received via /sync
 * callback.  The json string shall have the type
 * "m.room.encrypted".
 *
 * Returns: (transfer full): The decrypted string or
 *  %NULL if failed to decrypt.  Free with g_free()
 */
char *
cm_room_decrypt_content (CmRoom     *self,
                         const char *json_str)
{
  g_autoptr(JsonObject) root = NULL;
  char *plain_text = NULL;
  JsonObject *content;
  CmEnc *enc;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  enc = cm_client_get_enc (self->client);

  if (!enc || !json_str || !*json_str)
    return NULL;

  root = cm_utils_string_to_json_object (json_str);
  content = cm_utils_json_object_get_object (root, "content");
  plain_text = cm_enc_handle_join_room_encrypted (enc, self->room_id, content);

  return plain_text;
}

void
cm_room_set_data (CmRoom     *self,
                  JsonObject *object)
{
  JsonObject *child;
  JsonArray *array;
  guint length = 0;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (object);

  child = cm_utils_json_object_get_object (object, "state");
  array = cm_utils_json_object_get_array (child, "events");

  if (array)
    length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      const char *type, *value;

      child = json_array_get_object_element (array, i);
      type = cm_utils_json_object_get_string (child, "type");

      if (!self->name && g_strcmp0 (type, "m.room.name") == 0)
        {
          child = cm_utils_json_object_get_object (child, "content");
          value = cm_utils_json_object_get_string (child, "name");
          cm_room_set_name (self, value);
        }
      else if (!self->initial_sync_done &&
               g_strcmp0 (type, "m.room.encryption") == 0)
        {
          child = cm_utils_json_object_get_object (child, "content");
          value = cm_utils_json_object_get_string (child, "algorithm");
          g_free (self->encryption);
          self->encryption = g_strdup (value);
        }
    }

  /* todo: Implement this in a better less costly way */
  /* currently if a user changes the whole key list is reset */
  array = cm_utils_json_object_get_array (object, "changed");

  if (array && json_array_get_length (array) > 0)
    {
      self->keys_queried = FALSE;
      self->keys_claimed = FALSE;
      self->keys_uploaded = FALSE;
    }

  length = 0;
  array = cm_utils_json_object_get_array (object, "left");

  if (array)
    length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      CmRoomMember *member;
      const char *member_id;

      member_id = json_array_get_string_element (array, i);
      member = g_hash_table_lookup (self->joined_members_table, member_id);

      if (member)
        {
          g_hash_table_remove (self->joined_members_table, member_id);
          cm_utils_remove_list_item (self->joined_members, member);
          self->keys_queried = FALSE;
          self->keys_claimed = FALSE;
          self->keys_uploaded = FALSE;
        }
    }

  if (!self->initial_sync_done)
    cm_room_save (self);

  self->initial_sync_done = TRUE;
}

/*
 * cm_room_user_changed:
 * @self: A #CmRoom
 * @user_id: A fully qualified matrix user id
 *
 * Inform that the user devices for @user_id has
 * changed. The function simply returns if the
 * user @user_id is not in the room
 *
 * This is useful for encrypted rooms where the user
 * devices has to be updated before sending anything
 * encrypted.
 */
void
cm_room_user_changed (CmRoom     *self,
                      const char *user_id)
{
  CmRoomMember *member;

  g_return_if_fail (CM_IS_ROOM (self));

  /* We need to track the user changes only if room is encrypted */
  /* xxx: This will change once we expose list of user devices */
  if (!user_id && !cm_room_is_encrypted (self))
    return;

  member = g_hash_table_lookup (self->joined_members_table, user_id);

  if (member)
    cm_room_member_set_device_changed (member, TRUE);
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
  g_return_if_fail (CM_IS_ROOM (self));

  g_free (self->name);
  self->name = g_strdup (name);
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
  g_return_val_if_fail (CM_IS_ROOM (self), 0);

  return 0;
  /* return self->encryption_rotation_time; */
}

/* cm_room_get_encryption_msg_count:
 *
 * Return the number of messages after which the encryption
 * key for sending messages in the room should be rotated.
 */
guint
cm_room_get_encryption_msg_count (CmRoom *self)
{

  return 100;
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
  Message *message = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;
  const char *event_id = NULL;

  g_assert (G_IS_TASK (message->task));

  self = g_task_get_source_object (message->task);
  object = g_task_propagate_pointer (G_TASK (result), &error);

  event_id = cm_utils_json_object_get_string (object, "event_id");
  g_debug ("Sending message, has-error: %d. event-id: %s",
           !!error, event_id);

  self->is_sending_message = FALSE;

  if (error)
    {
      g_debug ("Error sending message: %s", error->message);
      g_task_return_error (message->task, error);
    }
  else
    {
      g_task_return_pointer (message->task, g_strdup (event_id), g_free);
    }
}

static void
upload_out_group_key_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  CmRoom *self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (CM_IS_ROOM (self));

  if (cm_client_upload_group_keys_finish (self->client, result, &error))
    self->keys_uploaded = TRUE;

  self->uploading_keys = FALSE;

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("error uploading group keys: %s", error->message);
      return;
    }
  else
    {
      /* TODO: Handle error */
      room_send_message_from_queue (self);
    }
}

static void
claim_key_cb (GObject      *obj,
              GAsyncResult *result,
              gpointer      user_data)
{
  CmRoom *self = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonObject) root = NULL;
  JsonObject *object;

  g_assert (CM_IS_ROOM (self));

  root = cm_client_claim_keys_finish (self->client, result, &error);

  if (root)
    {
      g_autoptr(GList) members = NULL;

      object = cm_utils_json_object_get_object (root, "one_time_keys");
      if (object)
        members = json_object_get_members (object);

      for (GList *member = members; member; member = member->next)
        {
          CmRoomMember *user;
          JsonObject *keys;

          user = room_find_member (self, G_LIST_MODEL (self->joined_members), member->data);

          if (!user)
            {
              g_warning ("‘%s’ not found in buddy list", (char *)member->data);
              continue;
            }

          keys = cm_utils_json_object_get_object (object, member->data);
          cm_room_member_add_one_time_keys (user, keys);
        }
    }

  if (root)
    self->keys_claimed = TRUE;
  self->claiming_keys = FALSE;

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("error claiming keys: %s", error->message);
      return;
    }
  else
    {
      /* TODO: Handle error */
      room_send_message_from_queue (self);
    }
}

static void
room_prepare_message (CmRoom  *self,
                      Message *message)
{
  g_autofree char *uri = NULL;
  JsonObject *root;

  g_assert (CM_IS_ROOM (self));
  g_assert (message);

  if (!message->event_id)
    {
      message->event_id = g_strdup_printf ("m%"G_GINT64_FORMAT".%d",
                                           g_get_real_time () / G_TIME_SPAN_MILLISECOND,
                                           cm_client_pop_event_id (self->client));
    }

  root = json_object_new ();
  json_object_set_string_member (root, "msgtype", "m.text");
  json_object_set_string_member (root, "body", message->text);

  if (self->encryption)
    {
      g_autofree char *text = NULL;
      JsonObject *object;

      object = json_object_new ();
      json_object_set_string_member (object, "type", "m.room.message");
      json_object_set_string_member (object, "room_id", self->room_id);
      json_object_set_object_member (object, "content", root);

      text = cm_utils_json_object_to_string (object, FALSE);
      message->object = cm_enc_encrypt_for_chat (cm_client_get_enc (self->client),
                                                 self->room_id, text);
    }
  else
    {
      message->object = root;
    }
}

static void
room_send_message_from_queue (CmRoom *self)
{
  Message *message;
  g_autofree char *uri = NULL;

  g_assert (CM_IS_ROOM (self));

  message = g_queue_peek_head (self->message_queue);

  if (!message)
    return;

  if (self->is_sending_message || self->retry_timeout_id)
    return;

  self->is_sending_message = TRUE;

  /* If encrypted ... */
  if (self->encryption)
    {
      if (self->joined_members_loading || self->querying_keys ||
          self->claiming_keys || self->uploading_keys)
        {
          self->is_sending_message = FALSE;
          return;
        }

      /* load the list of joined members  */
      if (!self->joined_members_loaded)
        {
          g_debug ("getting joined members");
          cm_room_get_joined_members_async (self, g_task_get_cancellable (message->task), NULL, NULL);
          self->is_sending_message = FALSE;
          return;
        }

      /* and then query their keys */
      if (!self->keys_queried)
        {
          cm_room_query_keys_async (self, g_task_get_cancellable (message->task), NULL, NULL);
          self->is_sending_message = FALSE;
          return;
        }

      /* and then claim them */
      if (!self->keys_claimed)
        {
          self->claiming_keys = TRUE;
          cm_client_claim_keys_async (self->client,
                                      G_LIST_MODEL (self->joined_members),
                                      claim_key_cb, self);
          self->is_sending_message = FALSE;
          return;
        }

      if (!self->keys_uploaded)
        {
          self->uploading_keys = TRUE;
          cm_client_upload_group_keys_async (self->client,
                                             self->room_id,
                                             G_LIST_MODEL (self->joined_members),
                                             upload_out_group_key_cb,
                                             self);
          self->is_sending_message = FALSE;
          return;
        }
    }

  message = g_queue_pop_head (self->message_queue);

  room_prepare_message (self, message);

  /* https://matrix.org/docs/spec/client_server/r0.6.1#put-matrix-client-r0-rooms-roomid-send-eventtype-txnid */
  if (self->encryption)
    uri = g_strdup_printf ("/_matrix/client/r0/rooms/%s/send/m.room.encrypted/%s",
                           self->room_id, message->event_id);
  else
    uri = g_strdup_printf ("/_matrix/client/r0/rooms/%s/send/m.room.message/%s",
                           self->room_id, message->event_id);

  cm_net_send_json_async (cm_client_get_net (self->client), 0,
                          json_object_ref (message->object),
                          uri, SOUP_METHOD_PUT, NULL, g_task_get_cancellable (message->task),
                          send_cb, message);
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
  Message *message;
  GTask *task;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);

  task = g_task_new (self, cancellable, callback, user_data);
  message = g_new0 (Message, 1);
  message->task = task;
  message->text = g_strdup (text);
  /* xxx: Should we use a different one so that we won't conflict with element client? */
  message->event_id = g_strdup_printf ("mc%"G_GINT64_FORMAT".%d",
                                       g_get_real_time () / G_TIME_SPAN_MILLISECOND,
                                       cm_client_pop_event_id (self->client));
  g_queue_push_tail (self->message_queue, message);

  room_send_message_from_queue (self);

  return message->event_id;
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
                         GAsyncReadyCallback    callback,
                         gpointer               user_data)
{
  Message *message;
  GTask *task;

  g_return_val_if_fail (CM_IS_ROOM (self), NULL);
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  task = g_task_new (self, NULL, callback, user_data);
  g_object_set_data (G_OBJECT (task), "progress-cb", progress_callback);
  g_object_set_data (G_OBJECT (task), "progress-cb-data", progress_user_data);

  message = g_new0 (Message, 1);
  message->task = task;
  message->text = g_strdup (body);
  message->file = g_object_ref (file);
  message->event_id = g_strdup_printf ("mc%"G_GINT64_FORMAT".%d",
                                       g_get_real_time () / G_TIME_SPAN_MILLISECOND,
                                       cm_client_pop_event_id (self->client));
  g_queue_push_tail (self->message_queue, message);

  room_send_message_from_queue (self);

  return message->event_id;
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
                               const char          *fully_read_id,
                               const char          *read_receipt_id,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autofree char *uri = NULL;
  JsonObject *root;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (fully_read_id && *fully_read_id);

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
  GError *error = NULL;
  char *json_str;
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
  cm_room_save (self);

  json_str = cm_utils_json_object_to_string (object, FALSE);
  g_task_return_pointer (task, json_str, g_free);
}

void
cm_room_load_prev_batch_async (CmRoom              *self,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autofree char *uri = NULL;
  const char *prev_batch;
  GHashTable *query;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM (self));

  task = g_task_new (self, NULL, callback, user_data);
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
                          query, NULL, room_load_prev_batch_cb, task);
}

char *
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
          CmRoomMember *cm_member;
          JsonObject *data;

          cm_member = g_hash_table_lookup (self->joined_members_table, member->data);

          if (!cm_member)
            {
              cm_member = cm_room_member_new (self, self->client, member->data);
              g_hash_table_insert (self->joined_members_table, g_strdup (member->data), cm_member);
              g_list_store_append (self->joined_members, cm_member);
              g_object_unref (cm_member);
            }

          data = json_object_get_object_member (joined, member->data);
          cm_room_member_set_json_data (cm_member, data);
        }

      self->joined_members_loaded = TRUE;
      self->joined_members_loading = FALSE;
      g_task_return_pointer (task, self->joined_members, NULL);

      /* TODO: handle failures */
      room_send_message_from_queue (self);
    }
}

void
cm_room_get_joined_members_async (CmRoom              *self,
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
      g_task_return_pointer (task, self->joined_members, NULL);
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

GListModel *
cm_room_get_joined_members_finish (CmRoom        *self,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
keys_query_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  self = g_task_get_source_object (task);
  object = g_task_propagate_pointer (G_TASK (result), &error);

  self->querying_keys = FALSE;
  if (error)
    {
      g_debug ("Error key query: %s", error->message);
      g_task_return_error (task, error);
    }
  else
    {
      g_autoptr(GList) members = NULL;
      JsonObject *keys;

      keys = cm_utils_json_object_get_object (object, "device_keys");
      if (object)
        members = json_object_get_members (keys);

      for (GList *member = members; member; member = member->next)
        {
          CmRoomMember *cm_member;
          JsonObject *key;

          cm_member = room_find_member (self, G_LIST_MODEL (self->joined_members), member->data);

          if (!cm_member)
            {
              g_warning ("‘%s’ not found in member list", (char *)member->data);
              continue;
            }

          key = cm_utils_json_object_get_object (keys, member->data);
          cm_room_member_set_devices (cm_member, key);
          self->keys_queried = TRUE;
        }

      g_task_return_boolean (task, TRUE);

      /* TODO: Handle errors */
      room_send_message_from_queue (self);
    }
}

/**
 * cm_room_query_keys_async:
 * @self: A #CmRoom
 * @callback: A #GAsyncReadyCallback
 * @user_data: user data passed to @callback
 *
 * Get identity keys of all devices in @self.
 *
 * Finish the call with cm_room_query_keys_finish()
 * to get the result.
 */
void
cm_room_query_keys_async (CmRoom              *self,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  GListModel *members;
  JsonObject *object, *child;
  GTask *task;
  guint n_items;

  g_return_if_fail (CM_IS_ROOM (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  if (self->querying_keys)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Querying keys in progres");
      return;
    }

  self->querying_keys = TRUE;
  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-keys-query */
  object = json_object_new ();
  json_object_set_int_member (object, "timeout", KEY_TIMEOUT);

  members = G_LIST_MODEL (self->joined_members);
  n_items = g_list_model_get_n_items (members);
  child = json_object_new ();

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmRoomMember) member = NULL;

      member = g_list_model_get_item (members, i);
      /* TODO: Implement and handle device blocking */
      json_object_set_array_member (child,
                                    cm_room_member_get_user_id (member),
                                    json_array_new ());
    }

  json_object_set_object_member (object, "device_keys", child);

  cm_net_send_json_async (cm_client_get_net (self->client), 0, object,
                          "/_matrix/client/r0/keys/query", SOUP_METHOD_POST,
                          NULL, cancellable, keys_query_cb, task);
}

gboolean
cm_room_query_keys_finish (CmRoom        *self,
                           GAsyncResult  *result,
                           GError       **error)
{
  g_return_val_if_fail (CM_IS_ROOM (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
room_load_from_db_cb (GObject      *object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  CmRoom *self;
  g_autoptr(GCancellable) cancellable = NULL;
  g_autoptr(GTask) task = user_data;
  g_autofree char *json_str;
  char *prev_batch;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM (self));

  cancellable = g_task_get_cancellable (task);
  if (cancellable)
    g_object_ref (cancellable);

  json_str = cm_db_load_room_finish (CM_DB (object), result, NULL);
  prev_batch = g_object_get_data (G_OBJECT (task), "prev-batch");

  if (json_str)
    {
      g_autoptr(JsonObject) root = NULL;
      JsonObject *obj;

      root = cm_utils_string_to_json_object (json_str);
      obj = cm_utils_json_object_get_object (root, "local");
      self->name_loaded = TRUE;
      if (cm_utils_json_object_get_int (obj, "encryption"))
        self->encryption = g_strdup ("encrypted");
      cm_room_set_is_direct (self, cm_utils_json_object_get_bool (obj, "direct"));
      cm_room_set_name (self, cm_utils_json_object_get_string (obj, "alias"));
      cm_room_set_prev_batch (self, prev_batch);

      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ENCRYPTED]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
      g_task_return_boolean (task, TRUE);
      return;
    }
}

void
cm_room_load_async (CmRoom              *self,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GTask *task;

  task = g_task_new (self, cancellable, callback, user_data);
  cm_db_load_room_async (cm_client_get_db (self->client), self->client, self,
                         room_load_from_db_cb,
                         task);
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
  g_autoptr(CmClient) self = user_data;
  g_autoptr(GError) error = NULL;

  if (!cm_db_save_room_finish (CM_DB (object), result, &error))
    g_warning ("Error saving room details: %s", error->message);
}

void
cm_room_save (CmRoom *self)
{
  g_return_if_fail (CM_IS_ROOM (self));

  cm_db_save_room_async (cm_client_get_db (self->client), self->client, self,
                         save_room_cb,
                         g_object_ref (self));
}
