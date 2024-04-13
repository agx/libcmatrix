/* cm-user-list.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-user-list"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "events/cm-event-private.h"
#include "cm-user-private.h"
#include "cm-net-private.h"
#include "cm-room-member-private.h"
#include "cm-utils-private.h"
#include "cm-common.h"
#include "cm-device-private.h"
#include "cm-client-private.h"
#include "cm-user-list-private.h"

/**
 * CmUserList:
 *
 * Track all users that belongs to the account
 *
 * #CmUserList tracks all users associated with the account, instead
 * of tracking them per room individually. Please note that only
 * relevant users (eg: Users that shares an encrypted room, or users
 * that have sent an event recently) are stored to avoid eating too
 * much memory
 */

/*
 * Device key request (ie, loading all devices of user(s) from server):
 *   https://matrix.org/docs/spec/client_server/r0.6.1#tracking-the-device-list-for-a-user
 *   - We keep track of all changed users in `changed_users` hash table
 *     - changed_users may not contain users that don't share any
 *       encrypted room.
 *   - Only one request shall run at a time so as to avoid races
 *   - On a request to load user devices, if no requested user is in the
 *     changed_users table, return early.
 *   - On a request, Remove the users from `changed_users`, and keep the
 *     items in `current_request`.
 *     - We remove the items early so that if the user devices change
 *       again midst the request, `changed_users` shall have them again.
 *     - Add back to changed_users if the request fails.
 *     - On success, check if any of the requested user is in `changed_users`
 *       - If so return CM_ERROR_USER_DEVICE_CHANGED,
 *       - else return %TRUE
 */

#define KEY_TIMEOUT         10000 /* milliseconds */

struct _CmUserList
{
  GObject       parent_instance;

  CmClient     *client;
  GHashTable   *users_table;
  GHashTable   *changed_users;

  /* We make only one request a time */
  GQueue       *device_request_queue;
  GTask        *current_request;

  gboolean      is_requesting_device;
};

G_DEFINE_TYPE (CmUserList, cm_user_list, G_TYPE_OBJECT)

enum {
  USER_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void request_device_keys_from_queue (CmUserList *self);

static void
device_keys_query_cb (GObject      *obj,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  CmUserList *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GPtrArray *users = NULL;
  GError *error = NULL;
  CmUser *user = NULL;

  self = g_task_get_source_object (task);
  users = g_task_get_task_data (task);
  object = g_task_propagate_pointer (G_TASK (result), &error);

  g_assert (CM_IS_USER_LIST (self));
  g_assert (users);

  g_debug ("(%p) Load user devices %s", users, CM_LOG_SUCCESS (!error));

  if (error)
    {
      /* Re-add the users to changed_users */
      for (guint i = 0; i < users->len; i++)
        {
          GRefString *user_id;

          user = users->pdata[i];
          user_id = cm_user_get_id (user);
          g_hash_table_insert (self->changed_users, user_id, g_object_ref (user));
        }

      g_debug ("(%p) Load user devices error: %s", users, error->message);
    }
  else
    {
      g_autoptr(GList) members = NULL;
      JsonObject *keys;

      keys = cm_utils_json_object_get_object (object, "device_keys");
      if (object)
        members = json_object_get_members (keys);

      g_debug ("(%p) Load user devices, to load: %u, loaded: %u", users, users->len,
               keys ? json_object_get_size (keys) : 0);
      for (GList *member = members; member; member = member->next)
        {
          g_autoptr(GRefString) user_id = NULL;
          g_autoptr(GPtrArray) removed = NULL;
          g_autoptr(GPtrArray) added = NULL;
          JsonObject *key;
          gboolean check_again;

          user_id = g_ref_string_new_intern (member->data);
          user = g_hash_table_lookup (self->users_table, user_id);

          if (!user)
            {
              g_debug ("(%p) Load user devices, '%s' not in users list",
                       users, user_id);
              if (user)
                g_ptr_array_remove (users, user);
              continue;
            }

          added = g_ptr_array_new_full (32, g_object_unref);
          removed = g_ptr_array_new_full (32, g_object_unref);
          check_again = g_hash_table_contains (self->changed_users, user_id);
          key = cm_utils_json_object_get_object (keys, member->data);
          cm_user_set_devices (user, key, !check_again, added, removed);

          /*
           * Both 'added' and 'removed' can be empty in the cases
           * where the changes are already in the cache (for which,
           * the changes will already be in the db)
           */
          if (added->len || removed->len)
            cm_db_update_user_devices (cm_client_get_db (self->client), self->client,
                                       user, added, removed, FALSE);
          g_signal_emit (self, signals[USER_CHANGED], 0, user, added, removed);
          g_ptr_array_remove (users, user);

          g_debug ("(%p) Load user devices, user: %s, devices, added: %u, removed: %u",
                   users, user_id, added->len, removed->len);
        }
    }

  if (!error && users->len)
    g_debug ("(%p) Load user devices, %u users changed again",
             users, users->len);

  g_clear_object (&self->current_request);
  self->is_requesting_device = FALSE;
  request_device_keys_from_queue (self);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task,
                           g_ptr_array_ref (users),
                           (GDestroyNotify)g_ptr_array_unref);
}

static void
remove_unlisted_users (CmUserList *self,
                       GPtrArray  *users)
{
  GPtrArray *old_users = NULL;
  guint len;

  g_assert (CM_IS_USER_LIST (self));
  g_assert (users);

  len = users->len;

  if (self->current_request)
    old_users = g_task_get_task_data (self->current_request);

  /* If the users list contain users not in the changed users table, or in
   * current request, remove them as we shall have already loaded them */
  for (guint i = 0; i < users->len;)
    {
      GListModel *devices;
      CmUser *user = users->pdata[i];
      GRefString *user_id;

      user_id = cm_user_get_id (user);
      devices = cm_user_get_devices (user);

      /* Don't remove if the user is in changed users or in the current request */
      if (g_hash_table_contains (self->changed_users, user_id) ||
          g_list_model_get_n_items (devices) == 0 ||
          users == old_users ||
          (old_users && g_ptr_array_find (old_users, user_id, NULL)))
        i++;
      else
        g_ptr_array_remove_index (users, i);
    }

  if (len != users->len)
    g_debug ("(%p) Request to load device keys removed %u users from %u",
             users, len - users->len, len);
}

static void
request_device_keys_from_queue (CmUserList *self)
{
  GCancellable *cancellable;
  GPtrArray *users;
  JsonObject *object, *child;
  GTask *task;

  g_assert (CM_IS_USER_LIST (self));

  if (!g_queue_peek_head (self->device_request_queue) ||
      self->is_requesting_device)
    return;

  self->is_requesting_device = TRUE;
  task = g_queue_pop_head (self->device_request_queue);
  users = g_task_get_task_data (task);
  cancellable = g_task_get_cancellable (task);

  g_assert (users);
  g_assert (!self->current_request);
  g_set_object (&self->current_request, task);

  for (guint i = 0; i < users->len; i++)
    g_hash_table_remove (self->changed_users, cm_user_get_id (users->pdata[i]));

  remove_unlisted_users (self, users);

  /* If no users left to request, return */
  if (users->len == 0)
    {
      g_debug ("(%p) Load user devices %s", users, CM_LOG_SUCCESS (TRUE));
      g_task_return_pointer (task, NULL, NULL);
      self->is_requesting_device = FALSE;
      /* Repeat */
      request_device_keys_from_queue (self);
      return;
    }

  object = json_object_new ();
  child = json_object_new ();
  json_object_set_int_member (object, "timeout", KEY_TIMEOUT);
  json_object_set_object_member (object, "device_keys", child);

  for (guint i = 0; i < users->len; i++)
    json_object_set_array_member (child,
                                  cm_user_get_id (users->pdata[i]),
                                  json_array_new ());

  g_debug ("(%p) Load user devices, users count: %u", users, users->len);
  cm_net_send_json_async (cm_client_get_net (self->client), 0, object,
                          "/_matrix/client/r0/keys/query", SOUP_METHOD_POST,
                          NULL, cancellable, device_keys_query_cb, task);
}

static void
cm_user_list_finalize (GObject *object)
{
  CmUserList *self = (CmUserList *)object;

  g_clear_object (&self->client);
  g_hash_table_unref (self->users_table);
  g_clear_object (&self->current_request);
  g_hash_table_unref (self->changed_users);

  G_OBJECT_CLASS (cm_user_list_parent_class)->finalize (object);
}

static void
cm_user_list_class_init (CmUserListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_user_list_finalize;

  /**
   * CmUserList::user-changed:
   * @self: a #CmUserList
   * @added_devices: The number of devices newly added
   * @removed_devices: The number of existing devices removed
   *
   * user-changed signal is emitted when the user's
   * device list changes.
   */
  signals [USER_CHANGED] =
    g_signal_new ("user-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  CM_TYPE_USER,
                  G_TYPE_PTR_ARRAY,
                  G_TYPE_PTR_ARRAY);
}

static void
cm_user_list_init (CmUserList *self)
{
  self->device_request_queue = g_queue_new ();
  self->users_table = g_hash_table_new_full (g_direct_hash,
                                             g_direct_equal,
                                             (GDestroyNotify)g_ref_string_release,
                                             g_object_unref);
  self->changed_users = g_hash_table_new_full (g_direct_hash,
                                               g_direct_equal,
                                               (GDestroyNotify)g_ref_string_release,
                                               g_object_unref);
}

void
cm_user_key_free (gpointer data)
{
  CmUserKey *key = data;

  if (!data)
    return;

  g_clear_object (&key->user);
  g_clear_pointer (&key->devices, g_ptr_array_unref);
  g_clear_pointer (&key->keys, g_ptr_array_unref);
  g_free (key);
}

CmUserList *
cm_user_list_new (CmClient *client)
{
  CmUserList *self;

  g_return_val_if_fail (CM_IS_CLIENT (client), NULL);

  self = g_object_new (CM_TYPE_USER_LIST, NULL);
  self->client = g_object_ref (client);

  g_debug ("(%p) New user list with client %p created", self, client);

  return self;
}

/**
 * cm_user_list_device_changed:
 * @self: A #CmUserList
 * @root: A #JsonObject
 * @changed: (out): A #GPtrArray
 *
 * @changed should be created with g_object_unref()
 * as free function.  @changed shall be filled with
 * the #CmUser that got changed.
 */
void
cm_user_list_device_changed (CmUserList *self,
                             JsonObject *root,
                             GPtrArray  *changed)
{
  JsonArray *users;
  CmUser *user;
  guint length = 0;

  g_return_if_fail (CM_IS_USER_LIST (self));
  g_return_if_fail (root);
  /* The user list should be empty, we shall fill it in */
  g_return_if_fail (changed && changed->len == 0);

  users = cm_utils_json_object_get_array (root, "changed");
  if (users)
    length = json_array_get_length (users);

  for (guint i = 0; i < length; i++)
    {
      GRefString *matrix_id;
      const char *user_id;

      user_id = json_array_get_string_element (users, i);
      CM_TRACE ("(%p) User '%s' device changed", self->client, user_id);
      matrix_id = g_ref_string_new_intern (user_id);
      user = cm_user_list_find_user (self, matrix_id, TRUE);
      g_ptr_array_add (changed, g_object_ref (user));
      g_hash_table_insert (self->changed_users, matrix_id,
                           g_object_ref (user));
    }
}

/**
 * cm_user_list_find_user:
 * @self: A #CmUserList
 * @user_id: A fully qualified matrix id
 * @create_if_missing: Create if missing
 *
 * Find the #CmUser for the given @user_id.  If
 * @create_if_missing is %FALSE and the user is not
 * already in the cache, %NULL shall be returned.
 *
 * Returns: (nullable) (transfer none): A #CmUser
 */
CmUser *
cm_user_list_find_user (CmUserList *self,
                        GRefString *user_id,
                        gboolean    create_if_missing)
{
  CmUser *user;

  g_return_val_if_fail (CM_IS_USER_LIST (self), NULL);
  g_return_val_if_fail (user_id && *user_id == '@', NULL);

  user = g_hash_table_lookup (self->users_table, user_id);

  if (user || !create_if_missing)
    return user;

  user = (CmUser *)cm_room_member_new (user_id);
  cm_user_set_client (user, self->client);
  g_hash_table_insert (self->users_table,
                       g_ref_string_acquire (user_id), user);

  return user;
}

/**
 * cm_user_list_set_account:
 * @self: A #CmUserList
 * @account: A #CmAccount
 *
 * Set the @account of @self after the matrix user id
 * of @account is set.  This should be set before adding
 * any user to the list.
 */
void
cm_user_list_set_account (CmUserList *self,
                          CmAccount  *account)
{
  GRefString *user_id;

  g_return_if_fail (CM_IS_USER_LIST (self));
  g_return_if_fail (CM_IS_ACCOUNT (account));

  user_id = cm_user_get_id (CM_USER (account));
  g_return_if_fail (user_id);

  if (g_hash_table_contains (self->users_table, user_id))
    return;

  /* @account should be the first user added to the table */
  g_return_if_fail (g_hash_table_size (self->users_table) == 0);

  g_hash_table_insert (self->users_table,
                       g_ref_string_acquire (user_id), account);
}

/**
 * cm_user_list_load_devices_async:
 * @self: A #CmUserList
 * @users: A #GPtrArray of #CmUsers
 * @callback: A #GAsyncReadyCallback
 * @user_data: user data for @callback
 *
 * Load all devices for the given users.  Each #CmUser
 * in @users should have a ref added.
 */
void
cm_user_list_load_devices_async (CmUserList          *self,
                                 GPtrArray           *users,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (CM_IS_USER_LIST (self));
  g_return_if_fail (users && users->len > 0);

  g_debug ("(%p) Queue Load %p user devices, users count: %u",
           self->client, users, users->len);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_task_data (task, g_ptr_array_ref (users),
                        (GDestroyNotify)g_ptr_array_unref);
  remove_unlisted_users (self, users);

  /* If no users left to request, return */
  if (users->len == 0)
    {
      g_debug ("(%p) Load %p user devices %s", self->client, users, CM_LOG_SUCCESS (TRUE));
      g_task_return_pointer (task, NULL, NULL);
    }
  else
    {
      g_queue_push_tail (self->device_request_queue, g_steal_pointer (&task));
    }

  request_device_keys_from_queue (self);
}

/**
 * cm_user_list_load_devices_finish:
 * @self: A #CmUserList
 * @result: A #GAsyncResult
 * @error: A #GError
 *
 * Returns: (transfer full): An array of #CmUser whose
 * device keys was not updated.
 */
GPtrArray *
cm_user_list_load_devices_finish (CmUserList    *self,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  g_return_val_if_fail (CM_IS_USER_LIST (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
claim_keys_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  CmUserList *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) root = NULL;
  JsonObject *object = NULL;
  GPtrArray *users = NULL;
  GError *error = NULL;
  CmRoom *room;

  self = g_task_get_source_object (task);
  room = g_object_get_data (user_data, "cm-room");
  users = g_task_get_task_data (task);
  g_assert (CM_IS_USER_LIST (self));
  g_assert (users);

  root = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("(%p) Claim %p user keys %s", room, users, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_debug ("(%p) Claim %p user keys, error: %s", room, users, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      g_autoptr(GPtrArray) one_time_keys = NULL;
      g_autoptr(GList) members = NULL;
      const char *room_id;

      object = cm_utils_json_object_get_object (root, "one_time_keys");
      one_time_keys = g_ptr_array_new_full (32, cm_user_key_free);
      room_id = cm_room_get_id (room);

      if (object)
        members = json_object_get_members (object);

      for (GList *member = members; member; member = member->next)
        {
          g_autoptr(GRefString) user_id = NULL;
          CmUser *user;
          JsonObject *keys;

          user_id = g_ref_string_new_intern (member->data);
          user = g_hash_table_lookup (self->users_table, user_id);

          keys = cm_utils_json_object_get_object (object, member->data);
          cm_user_add_one_time_keys (user, room_id, keys, one_time_keys);
        }

      g_debug ("(%p) Claim %p user keys success, keys: %u",
               room, users, one_time_keys->len);

      g_task_return_pointer (task,
                             g_steal_pointer (&one_time_keys),
                             (GDestroyNotify)g_ptr_array_unref);
    }
}

/**
 * cm_user_list_claim_keys_async:
 * @self: A #CmUserList
 * @room: A #CmRoom
 * @users: A #GHashTable of users
 * @callback: A callback to run when finished
 * @user_data: The user data for @callback
 *
 * Claim one time keys for the devices of given
 * users in @users.
 *
 * The key in @users should be a user_id #GRefString.
 * The value of @users should a #GPtrArray of #CmDevices
 * for which the one time keys should be claimed.
 *
 * The method shall return %CM_ERROR_USER_DEVICE_CHANGED
 * error if any of the user in @users is in the list of
 * changed users in @self.
 */
void
cm_user_list_claim_keys_async (CmUserList          *self,
                               CmRoom              *room,
                               GHashTable          *users,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GList) keys = NULL;
  JsonObject *root, *child;
  guint changed_count = 0;

  g_return_if_fail (CM_IS_USER_LIST (self));
  g_return_if_fail (CM_IS_ROOM (room));
  g_return_if_fail (users);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_task_data (task, g_hash_table_ref (users),
                        (GDestroyNotify)g_hash_table_unref);
  g_object_set_data_full (G_OBJECT (task),
                          "cm-room", g_object_ref (room),
                          g_object_unref);

  g_debug ("(%p) Claim %p user keys, users: %u",
           room, users, g_hash_table_size (users));

  keys = g_hash_table_get_keys (users);

  /* Check if any user's device got changed ... */
  for (GList *node = keys; node; node = node->next)
    {
      GRefString *user_id = node->data;

      if (g_hash_table_contains (self->changed_users, user_id))
        changed_count++;
    }

  /* ... if so, return an error as the caller should update user devices. */
  if (changed_count)
    {
      g_debug ("(%p) Claim %p user keys error, %u users pending update",
               room, users, changed_count);
      g_task_return_new_error (task, CM_ERROR, CM_ERROR_USER_DEVICE_CHANGED,
                               "%u users have their devices changed", changed_count);
      return;
    }

  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-keys-claim */
  root = json_object_new ();
  json_object_set_int_member (root, "timeout", KEY_TIMEOUT);
  child = json_object_new ();

  for (GList *node = keys; node; node = node->next)
    {
      GRefString *user_id = node->data;
      GPtrArray *devices;
      JsonObject *object;

      devices = g_hash_table_lookup (users, user_id);
      object = json_object_new ();

      for (guint i = 0; i < devices->len; i++)
        {
          const char *device_id;

          device_id = cm_device_get_id (devices->pdata[i]);
          json_object_set_string_member (object, device_id, "signed_curve25519");
        }

      if (object)
        json_object_set_object_member (child, user_id, object);
    }

  json_object_set_object_member (root, "one_time_keys", child);

  cm_net_send_json_async (cm_client_get_net (self->client), 0, root,
                          "/_matrix/client/r0/keys/claim", SOUP_METHOD_POST,
                          NULL, NULL, claim_keys_cb,
                          g_steal_pointer (&task));
}

/**
 * cm_user_list_claim_keys_finish:
 * @self: A #CmUserList
 * @result: A #GAsyncResult
 * @error: A #GError
 *
 * Get the claimed one time keys
 *
 * Returns: (transfer full): A #GPtrArray of #CmUserKey
 */
GPtrArray *
cm_user_list_claim_keys_finish (CmUserList    *self,
                                GAsyncResult  *result,
                                GError       **error)
{
  g_return_val_if_fail (CM_IS_USER_LIST (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
upload_group_keys_cb (GObject      *obj,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  CmUserList *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;
  CmRoom *room = NULL;

  self = g_task_get_source_object (task);
  room = g_task_get_task_data (task);
  g_assert (CM_IS_USER_LIST (self));
  g_assert (CM_IS_ROOM (room));

  object = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("(%p) Upload group keys %s", room, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_debug ("(%p) Upload group keys error: %s", room, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      gpointer session;

      session = g_object_get_data (G_OBJECT (task), "session");
      cm_enc_set_room_group_key (cm_client_get_enc (self->client),
                                 room, session);
      g_task_return_boolean (task, TRUE);
    }
}

/**
 * cm_user_list_upload_keys_async:
 * @self: A #CmUserList
 * @room: A #CmRoom
 * @one_time_keys: A #GptrArray for #CmUserKey
 * @callback: A callback to run when finished
 * @user_data: The user data for @callback
 *
 * Upload the given @one_time_keys to server.
 */
void
cm_user_list_upload_keys_async (CmUserList          *self,
                                CmRoom              *room,
                                GPtrArray           *one_time_keys,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr(CmEvent) event = NULL;
  g_autoptr(GTask) task = NULL;
  g_autofree char *uri = NULL;
  JsonObject *root, *object;
  gpointer olm_session = NULL;

  g_return_if_fail (CM_IS_USER_LIST (self));
  g_return_if_fail (CM_IS_ROOM (room));
  g_return_if_fail (one_time_keys && one_time_keys->len);

  g_debug ("(%p) Upload group keys, keys count: %u",
           room, one_time_keys->len);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_task_data (task, g_object_ref (room), g_object_unref);

  root = json_object_new ();
  object = cm_enc_create_out_group_keys (cm_client_get_enc (self->client),
                                         room, one_time_keys, &olm_session);
  g_object_set_data_full (G_OBJECT (task), "session", olm_session, g_object_unref);
  json_object_set_object_member (root, "messages", object);

  /* Create an event only to create event id */
  event = cm_event_new (CM_M_UNKNOWN);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->client));

  uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.room.encrypted/%s",
                         cm_event_get_txn_id (event));
  cm_net_send_json_async (cm_client_get_net (self->client),
                          0, root, uri, SOUP_METHOD_PUT,
                          NULL, NULL,
                          upload_group_keys_cb,
                          g_steal_pointer (&task));
}

gboolean
cm_user_list_upload_keys_finish (CmUserList    *self,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (CM_IS_USER_LIST (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
