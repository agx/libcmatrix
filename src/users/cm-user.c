/* cm-user.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-user"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-utils-private.h"
#include "cm-client-private.h"
#include "cm-device.h"
#include "cm-device-private.h"
#include "cm-matrix-private.h"
#include "cm-user-list-private.h"
#include "cm-user-private.h"
#include "cm-user.h"

typedef struct
{
  CmClient *cm_client;

  GRefString   *user_id;
  char *display_name;
  char *avatar_url;
  char *avatar_file_path;

  GTask        *avatar_task;
  GFile        *avatar_file;
  JsonObject   *generated_json;

  GListStore   *devices;
  GHashTable   *devices_table;

  /* Set when we know about some change, but not sure what it is */
  gboolean      device_changed;

  gboolean      info_loading;
  gboolean      info_loaded;
  gboolean      avatar_loading;
  gboolean      avatar_loaded;
} CmUserPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (CmUser, cm_user, G_TYPE_OBJECT)

enum {
  CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
cm_user_finalize (GObject *object)
{
  CmUser *self = (CmUser *)object;
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_clear_pointer (&priv->user_id, g_ref_string_release);
  g_free (priv->display_name);
  g_free (priv->avatar_url);
  g_free (priv->avatar_file_path);

  g_list_store_remove_all (priv->devices);
  g_clear_object (&priv->devices);
  g_clear_object (&priv->avatar_task);
  g_clear_pointer (&priv->devices_table, g_hash_table_unref);

  g_clear_object (&priv->avatar_file);
  g_clear_pointer (&priv->generated_json, json_object_unref);

  G_OBJECT_CLASS (cm_user_parent_class)->finalize (object);
}

static void
cm_user_class_init (CmUserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_user_finalize;

  /**
   * CmUser::changed:
   * @self: a #CmUser
   *
   * changed signal is emitted when name or avatar
   * of the user changes.
   */
  signals [CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
cm_user_init (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  priv->devices = g_list_store_new (CM_TYPE_DEVICE);
  priv->devices_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_object_unref);
}

JsonObject *
cm_user_generate_json (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_val_if_fail (CM_IS_USER (self), NULL);

  if (!priv->generated_json &&
      (priv->display_name || priv->avatar_url || priv->avatar_file))
    {
      g_autofree char *avatar_path = NULL;
      JsonObject *local, *child;
      GFile *parent;

      parent = g_file_new_for_path (cm_matrix_get_data_dir ());
      local = json_object_new ();
      json_object_set_object_member (local, "local", json_object_new ());
      priv->generated_json = local;

      if (priv->avatar_file)
        avatar_path = g_file_get_relative_path (parent, priv->avatar_file);

      child = json_object_get_object_member (local, "local");
      if (priv->display_name)
        json_object_set_string_member (child, "display_name", priv->display_name);
      if (priv->avatar_url)
        json_object_set_string_member (child, "avatar_url", priv->avatar_url);
      if (avatar_path)
        json_object_set_string_member (child, "avatar_path", avatar_path);
    }

  return priv->generated_json;
}

void
cm_user_set_json_data (CmUser     *self,
                       JsonObject *root)
{
  const char *name, *avatar_url;
  JsonObject *child;

  g_return_if_fail (CM_IS_USER (self));

  if (!root)
    return;

  child = cm_utils_json_object_get_object (root, "content");

  if (!child)
    child = root;

  name = cm_utils_json_object_get_string (child, "display_name");
  if (!name)
    name = cm_utils_json_object_get_string (child, "displayname");

  avatar_url = cm_utils_json_object_get_string (child, "avatar_url");
  cm_user_set_details (self, name, avatar_url);
}

void
cm_user_set_client (CmUser   *self,
                    CmClient *client)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  if (!priv->cm_client)
    priv->cm_client = g_object_ref (client);
}

CmClient *
cm_user_get_client (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_val_if_fail (CM_IS_USER (self), NULL);

  return priv->cm_client;
}

void
cm_user_set_user_id (CmUser     *self,
                     GRefString *user_id)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_if_fail (CM_IS_USER (self));

  if (priv->user_id == user_id)
    return;

  /* Allow setting user id only once, and never allow changing */
  g_return_if_fail (!priv->user_id);
  g_return_if_fail (user_id && *user_id == '@');

  priv->user_id = g_ref_string_acquire (user_id);
}

void
cm_user_set_details (CmUser     *self,
                     const char *display_name,
                     const char *avatar_url)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  gboolean changed = FALSE;

  g_return_if_fail (CM_IS_USER (self));

  if (g_strcmp0 (display_name, priv->display_name) != 0)
    {
      g_free (priv->display_name);
      priv->display_name = g_strdup (display_name);
      changed = TRUE;
    }

  if (g_strcmp0 (avatar_url, priv->avatar_url) != 0)
    {
      g_free (priv->avatar_url);
      priv->avatar_url = g_strdup (avatar_url);
      g_clear_object (&priv->avatar_file);
      priv->avatar_loaded = FALSE;
      changed = TRUE;

      if (priv->avatar_task)
        {
          GCancellable *cancellable;

          cancellable = g_task_get_cancellable (priv->avatar_task);
          if (cancellable)
            g_cancellable_cancel (cancellable);
          g_clear_object (&priv->avatar_task);
        }
    }

  /* If we are not already loading info, mark as info has loaded */
  if (changed)
    g_signal_emit (self, signals[CHANGED], 0);
}

GRefString *
cm_user_get_id (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_val_if_fail (CM_IS_USER (self), NULL);

  return priv->user_id;
}

const char *
cm_user_get_display_name (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_val_if_fail (CM_IS_USER (self), NULL);

  return priv->display_name;
}

const char *
cm_user_get_avatar_url (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_val_if_fail (CM_IS_USER (self), NULL);

  return priv->avatar_url;
}

static void
user_get_user_info_cb (GObject      *obj,
                       GAsyncResult *result,
                       gpointer      user_data);

static void
user_get_avatar_cb (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  CmUser *self;
  CmUserPrivate *priv;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  priv = cm_user_get_instance_private (self);

  g_clear_object (&priv->avatar_file);
  priv->avatar_file = cm_utils_save_url_to_path_finish (result, &error);
  g_debug ("(%p) Get avatar %s", self, CM_LOG_SUCCESS (!error));

  priv->avatar_loading = FALSE;
  priv->avatar_loaded = !error;

  if (error)
    g_task_return_error (task, error);
  else if (priv->avatar_file)
    {
      GInputStream *istream;

      istream = (GInputStream *)g_file_read (priv->avatar_file, NULL, NULL);
      g_object_set_data_full (G_OBJECT (priv->avatar_file), "stream",
                              istream, g_object_unref);
      g_task_return_pointer (task, g_object_ref (istream), g_object_unref);
      return;
    }
  else
    g_task_return_pointer (task, NULL, NULL);
}

static void
avatar_get_user_info_cb (GObject      *object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  CmUser *self;
  CmUserPrivate *priv;
  g_autoptr(GTask) task = user_data;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  priv = cm_user_get_instance_private (self);
  g_assert (CM_IS_USER (self));

  if (priv->avatar_url)
    {
      g_autofree char *file_name = NULL;
      GCancellable *cancellable;
      const char *path;
      char *file_path;

      path = cm_matrix_get_data_dir ();
      file_name = g_path_get_basename (priv->avatar_url);
      file_path = cm_utils_get_path_for_m_type (path, CM_M_ROOM_MEMBER, FALSE, file_name);
      cancellable = g_task_get_cancellable (task);

      priv->avatar_loading = TRUE;
      cm_utils_save_url_to_path_async (priv->cm_client, priv->avatar_url,
                                       file_path, cancellable,
                                       NULL, NULL,
                                       user_get_avatar_cb, g_steal_pointer (&task));
    }
  else
    g_task_return_pointer (task, NULL, NULL);
}

void
cm_user_get_avatar_async (CmUser              *self,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  g_autoptr(GTask) task = NULL;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, cm_user_get_avatar_async);

  g_debug ("(%p) Get avatar", self);

  if (priv->avatar_file)
    {
      GInputStream *istream;

      istream = (GInputStream *)g_file_read (priv->avatar_file, NULL, NULL);
      g_object_set_data_full (G_OBJECT (priv->avatar_file), "stream",
                              istream, g_object_unref);
      g_task_return_pointer (task, g_object_ref (istream), g_object_unref);
      return;
    }

  if (priv->avatar_loaded)
    {
      g_task_return_pointer (task, NULL, NULL);
      return;
    }

  if (priv->info_loading || priv->avatar_loading)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "Avatar already being downloaded");
      return;
    }

  g_set_object (&priv->avatar_task, task);

  if ((!priv->display_name && !priv->avatar_url) || !priv->info_loaded)
    cm_user_load_info_async (self, cancellable,
                             avatar_get_user_info_cb,
                             g_steal_pointer (&task));
  else if (priv->avatar_url)
    {
      g_autofree char *file_name = NULL;
      const char *path;
      char *file_path;

      path = cm_matrix_get_data_dir ();
      file_name = g_path_get_basename (priv->avatar_url);
      file_path = cm_utils_get_path_for_m_type (path, CM_M_ROOM_MEMBER, FALSE, file_name);

      priv->avatar_loading = TRUE;
      cm_utils_save_url_to_path_async (priv->cm_client, priv->avatar_url,
                                       file_path, cancellable,
                                       NULL, NULL,
                                       user_get_avatar_cb, g_steal_pointer (&task));
    }
  else
    g_task_return_pointer (task, NULL, NULL);
}

GInputStream *
cm_user_get_avatar_finish (CmUser        *self,
                           GAsyncResult  *result,
                           GError       **error)
{
  g_return_val_if_fail (CM_IS_USER (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
user_get_user_info_cb (GObject      *obj,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  CmUser *self;
  CmUserPrivate *priv;
  g_autoptr(GTask) task = user_data;
  const char *name, *avatar_url;
  GError *error = NULL;
  g_autoptr(JsonObject) object = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  priv = cm_user_get_instance_private (self);
  g_assert (CM_IS_USER (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("(%p) Load info %s", self,  CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_task_return_error (task, error);
      return;
    }

  name = cm_utils_json_object_get_string (object, "displayname");
  avatar_url = cm_utils_json_object_get_string (object, "avatar_url");

  g_free (priv->display_name);
  g_free (priv->avatar_url);

  priv->display_name = g_strdup (name);
  priv->avatar_url = g_strdup (avatar_url);
  priv->info_loaded = TRUE;
  priv->info_loading = FALSE;

  if (!priv->avatar_url)
    priv->avatar_loaded = TRUE;

  g_task_return_boolean (task, TRUE);
}

void
cm_user_load_info_async (CmUser              *self,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  g_autofree char *uri = NULL;
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  g_debug ("(%p) Load info", self);

  if (priv->info_loaded)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  if (priv->info_loading)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "info already being loaded");
      return;
    }

  priv->info_loading = TRUE;
  uri = g_strdup_printf ("/_matrix/client/r0/profile/%s", priv->user_id);
  cm_net_send_json_async (cm_client_get_net (priv->cm_client),
                          1, NULL, uri, SOUP_METHOD_GET,
                          NULL, cancellable, user_get_user_info_cb,
                          g_steal_pointer (&task));
}

gboolean
cm_user_load_info_finish (CmUser        *self,
                          GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (CM_IS_USER (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

GListModel *
cm_user_get_devices (CmUser *self)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_val_if_fail (CM_IS_USER (self), NULL);

  return G_LIST_MODEL (priv->devices);
}

CmDevice *
cm_user_find_device (CmUser     *self,
                     const char *device_id)
{
  GListModel *devices;
  guint n_items;

  g_return_val_if_fail (CM_IS_USER (self), NULL);
  g_return_val_if_fail (device_id && *device_id, NULL);

  devices = cm_user_get_devices (self);
  n_items = g_list_model_get_n_items (devices);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmDevice) device = NULL;
      const char *id;

      device = g_list_model_get_item (devices, i);
      id = cm_device_get_id (device);

      if (g_strcmp0 (id, device_id) == 0)
        return device;
    }

  return NULL;
}

/**
 * cm_user_set_devices:
 * @self: A #CmUser
 * @root: A #JsonObject
 * @update_state: Whether to update state
 * @added: (out): The number of new devices added
 * @removed: (out): The number of existing devices removed
 *
 * Set devices for @self removing all
 * non existing devices in @root
 *
 * If @update_state is %FALSE, the device changed info
 * shall not be updated, and so cm_user_get_device_changed()
 * shall return the old values.
 */
void
cm_user_set_devices (CmUser     *self,
                     JsonObject *root,
                     gboolean    update_state,
                     GPtrArray  *added,
                     GPtrArray  *removed)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  g_autoptr(GHashTable) devices_table = NULL;
  g_autoptr(GList) members = NULL;
  GHashTable *old_devices;
  JsonObject *child;

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (root);

  /* Create a table of devices and add the items here */
  devices_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, g_object_unref);
  members = json_object_get_members (root);

  for (GList *member = members; member; member = member->next)
    {
      g_autoptr(CmDevice) device = NULL;
      const char *device_id, *user;

      child = cm_utils_json_object_get_object (root, member->data);
      device_id = cm_utils_json_object_get_string (child, "device_id");
      user = cm_utils_json_object_get_string (child, "user_id");

      if (!device_id || !*device_id)
        continue;

      if (priv->devices_table &&
          (device = g_hash_table_lookup (priv->devices_table, device_id)))
        {
          /* If the device is already in the old table, remove it there
           * so that it's present only in the new devices_table.
           */
          /* device variable is autofree, so the ref is unref later automatically */
          g_object_ref (device);
          g_hash_table_remove (priv->devices_table, device_id);
          g_hash_table_insert (devices_table, g_strdup (device_id), g_object_ref (device));
          continue;
        }

      if (g_strcmp0 (user, cm_user_get_id (self)) != 0)
        {
          g_warning ("‘%s’ and ‘%s’ are not the same users",
                     user, cm_user_get_id (self));
          continue;
        }

      if (g_strcmp0 (member->data, device_id) != 0)
        {
          g_warning ("‘%s’ and ‘%s’ are not the same device", (char *)member->data, device_id);
          continue;
        }

      device = cm_device_new (self, priv->cm_client, child);
      g_hash_table_insert (devices_table, g_strdup (device_id), g_object_ref (device));
      g_list_store_append (priv->devices, device);
      if (added)
        g_ptr_array_add (added, g_object_ref (device));
    }

  old_devices = priv->devices_table;
  priv->devices_table = g_steal_pointer (&devices_table);
  /* Assign so as to autofree */
  devices_table = old_devices;

  if (old_devices)
    {
      g_autoptr(GList) devices = NULL;

      /* The old table now contains the devices that are not used by the user anymore */
      devices = g_hash_table_get_values (old_devices);

      for (GList *device = devices; device && device->data; device = device->next)
        {
          if (removed)
            g_ptr_array_add (removed, g_object_ref (device->data));
          cm_utils_remove_list_item (priv->devices, device->data);
        }
    }
}

void
cm_user_add_one_time_keys (CmUser     *self,
                           const char *room_id,
                           JsonObject *root,
                           GPtrArray  *out_keys)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  g_autoptr(CmUserKey) key = NULL;
  JsonObject *object, *child;
  guint n_items;

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (root);
  g_return_if_fail (out_keys);

  n_items = g_list_model_get_n_items (G_LIST_MODEL (priv->devices));

  key = g_new (CmUserKey, 1);
  key->user = g_object_ref (self);
  key->devices = g_ptr_array_new_full (n_items, g_object_unref);
  key->keys = g_ptr_array_new_full (n_items, g_free);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmDevice) device = NULL;
      g_autoptr(GList) members = NULL;
      const char *device_id;

      device = g_list_model_get_item (G_LIST_MODEL (priv->devices), i);
      device_id = cm_device_get_id (device);
      child = cm_utils_json_object_get_object (root, device_id);

      if (!child)
        {
          g_debug ("device '%s' doesn't have any keys", device_id);
          continue;
        }

      members = json_object_get_members (child);

      for (GList *node = members; node; node = node->next)
        {
          object = cm_utils_json_object_get_object (child, node->data);

          if (cm_enc_verify (cm_client_get_enc (priv->cm_client), object,
                             cm_user_get_id (self), device_id,
                             cm_device_get_ed_key (device)))
            {
              g_ptr_array_add (key->devices, g_object_ref (device));
              g_ptr_array_add (key->keys, cm_utils_json_object_dup_string (object, "key"));
            }
        }
    }

  if (key->devices->len)
    g_ptr_array_add (out_keys, g_steal_pointer (&key));
}
