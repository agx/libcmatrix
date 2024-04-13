/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-matrix"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <libsecret/secret.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cm-db-private.h"
#include "cm-utils-private.h"
#include "cm-secret-store-private.h"
#include "cm-client.h"
#include "cm-client-private.h"
#include "cm-matrix.h"
#include "cm-matrix-private.h"

struct _CmMatrix
{
  GObject parent_instance;

  char *db_path;
  char *db_name;

  char *data_dir;
  char *cache_dir;

  CmDb *cm_db;

  GListStore *clients_list;
  GHashTable *clients_to_save;
  CmSecretStore *secret_store;

  guint    network_change_id;

  gboolean secrets_loaded;
  gboolean db_loaded;
  gboolean is_opening;
  gboolean loading_accounts;
  gboolean disable_auto_login;
};


#define RECONNECT_TIMEOUT    500 /* milliseconds */

char *cmatrix_data_dir, *cmatrix_app_id;

G_DEFINE_TYPE (CmMatrix, cm_matrix, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_READY,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void matrix_save_client (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data);

static gboolean
matrix_has_client (CmMatrix *self,
                   CmClient *client,
                   gboolean  check_pending)
{
  const char *login_name, *user_name;
  CmAccount *account;
  GListModel *model;
  guint n_items;

  g_assert (CM_IS_MATRIX (self));
  g_assert (CM_IS_CLIENT (client));

  model = G_LIST_MODEL (self->clients_list);
  n_items = g_list_model_get_n_items (model);
  account = cm_client_get_account (client);
  user_name = cm_client_get_user_id (client);
  login_name = cm_account_get_login_id (account);

  /* For the time being, let's ignore the fact that the same username
   * can exist in different homeservers
   */
  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmClient) item = NULL;
      CmAccount *item_account;

      item = g_list_model_get_item (model, i);
      item_account = cm_client_get_account (item);

      if (login_name &&
          g_strcmp0 (login_name, cm_account_get_login_id (item_account)) == 0)
        return TRUE;

      if (user_name &&
          g_strcmp0 (user_name, cm_client_get_user_id (item)) == 0)
        return TRUE;

      if (user_name &&
          g_strcmp0 (user_name, cm_account_get_login_id (item_account)) == 0)
        return TRUE;
    }

  if (check_pending)
    return g_hash_table_contains (self->clients_to_save, login_name);

  return FALSE;
}

static gboolean
matrix_reconnect (gpointer user_data)
{
  CmMatrix *self = user_data;
  GListModel *model;
  guint n_items;

  self->network_change_id = 0;

  model = G_LIST_MODEL (self->clients_list);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++) {
    g_autoptr(CmClient) client = NULL;

    client = g_list_model_get_item (model, i);

    if (cm_client_can_connect (client) &&
        cm_client_get_enabled (client))
      cm_client_start_sync (client);
    else
      cm_client_stop_sync (client);
  }

  return G_SOURCE_REMOVE;
}

static void
matrix_network_changed_cb (CmMatrix        *self,
                           gboolean         network_available,
                           GNetworkMonitor *network_monitor)
{
  g_assert (CM_IS_MATRIX (self));
  g_assert (G_IS_NETWORK_MONITOR (network_monitor));

  if (!cm_matrix_is_ready (self))
    return;

  g_clear_handle_id (&self->network_change_id, g_source_remove);
  self->network_change_id = g_timeout_add (RECONNECT_TIMEOUT,
                                           matrix_reconnect, self);
}

static void
matrix_stop (CmMatrix *self)
{
  GListModel *model;
  guint n_items;

  g_assert (CM_IS_MATRIX (self));

  model = G_LIST_MODEL (self->clients_list);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmClient) client = NULL;

      client = g_list_model_get_item (model, i);

      cm_client_stop_sync (client);
    }
}

static void
cm_matrix_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  CmMatrix *self = (CmMatrix *)object;

  switch (prop_id)
    {
    case PROP_READY:
      g_value_set_boolean (value, cm_matrix_is_ready (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cm_matrix_finalize (GObject *object)
{
  CmMatrix *self = (CmMatrix *)object;

  g_clear_handle_id (&self->network_change_id, g_source_remove);
  g_clear_object (&self->secret_store);
  matrix_stop (self);
  g_list_store_remove_all (self->clients_list);
  g_clear_object (&self->clients_list);
  g_hash_table_unref (self->clients_to_save);

  g_free (self->db_path);
  g_free (self->db_name);

  g_free (self->data_dir);
  g_free (self->cache_dir);

  G_OBJECT_CLASS (cm_matrix_parent_class)->finalize (object);
}

static void
cm_matrix_class_init (CmMatrixClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_matrix_finalize;
  object_class->get_property = cm_matrix_get_property;

  /**
   * CmMatrix:ready:
   *
   * Whether matrix is enabled and usable
   */
  properties[PROP_READY] =
    g_param_spec_boolean ("ready",
                          "matrix is ready",
                          "matrix is ready",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
cm_matrix_init (CmMatrix *self)
{
  if (!gcry_control (GCRYCTL_INITIALIZATION_FINISHED_P))
    g_error ("libgcrypt has not been initialized, did you run cm_init()?");

  self->secret_store = cm_secret_store_new ();
  self->clients_list = g_list_store_new (CM_TYPE_CLIENT);
  self->clients_to_save = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_object_unref);

  g_signal_connect_object (g_network_monitor_get_default (),
                           "network-changed",
                           G_CALLBACK (matrix_network_changed_cb), self,
                           G_CONNECT_AFTER | G_CONNECT_SWAPPED);
}

/**
 * cm_matrix_new:
 * @data_dir: The data directory
 * @cache_dir: The cache directory
 * @app_id: The app id string (unused)
 * @disable_auto_login: Disable auto login
 *
 * Create a new #CmMatrix with the provided details
 *
 * @data_dir is used to store downloaded files,
 * avatars, and thumbnails.  The content shall not
 * be encrypted even if that was the case when
 * received over the wire.
 *
 * @app_id should be a valid string when validated
 * with g_application_id_is_valid()
 *
 * The same values should be provided every time
 * #CmMatrix is created as these info are used
 * to store data.
 *
 * Returns: (transfer full): A #CmMatrix
 */
/*
 * @cache_dir may be used to store files temporarily
 * when needed (eg: when resizing images)
 */
CmMatrix *
cm_matrix_new (const char *data_dir,
               const char *cache_dir,
               const char *app_id,
               gboolean    disable_auto_login)
{
  CmMatrix *self;
  char *dir;

  g_return_val_if_fail (data_dir && *data_dir, NULL);
  g_return_val_if_fail (cache_dir && *cache_dir, NULL);
  g_return_val_if_fail (g_application_id_is_valid (app_id), NULL);

  self = g_object_new (CM_TYPE_MATRIX, NULL);
  self->disable_auto_login = !!disable_auto_login;
  self->data_dir = g_build_filename (data_dir, "cmatrix", NULL);
  cmatrix_data_dir = g_strdup (self->data_dir);
  cmatrix_app_id = g_strdup (app_id);
  self->cache_dir = g_build_filename (cache_dir, "cmatrix", NULL);

  dir = cm_utils_get_path_for_m_type (self->data_dir, CM_M_ROOM_MESSAGE, TRUE, NULL);
  g_mkdir_with_parents (dir, S_IRWXU);
  g_free (dir);

  dir = cm_utils_get_path_for_m_type (self->data_dir, CM_M_ROOM_MEMBER, TRUE, NULL);
  g_mkdir_with_parents (dir, S_IRWXU);
  g_free (dir);

  dir = cm_utils_get_path_for_m_type (self->data_dir, CM_M_ROOM_AVATAR, TRUE, NULL);
  g_mkdir_with_parents (dir, S_IRWXU);
  g_free (dir);

  g_debug ("(%p) New matrix, data: %s, cache: %s", self, self->data_dir, self->cache_dir);

  return self;
}

/**
 * cm_init:
 * @init_gcrypt: Whether to initialize gcrypt
 *
 * This function should be called to initialize the library.
 * You may call this in main()
 *
 * If you don't initialize gcrypt, you should do it yourself
 */
void
cm_init (gboolean init_gcrypt)
{
  /* Force HTTP1 as we have issues with HTTP/2 implementation in libsoup3
   * Like https://gitlab.gnome.org/GNOME/libsoup/-/issues/302,
   * https://gitlab.gnome.org/GNOME/libsoup/-/issues/296, etc */
  /* todo: Remove once we have better HTTP/2 support */
  g_setenv ("SOUP_FORCE_HTTP1", "1", FALSE);

  if (init_gcrypt)
    {
      /* Version check should be the very first call because it
         makes sure that important subsystems are initialized. */
      if (!gcry_check_version (GCRYPT_VERSION))
        {
          g_critical ("libgcrypt version mismatch");
          exit (2);
        }
      gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
      gcry_control (GCRYCTL_INIT_SECMEM, 512 * 1024, 0);
      gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
      gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
    }
}

static void
load_accounts_from_secrets (CmMatrix  *self,
                            GPtrArray *accounts)
{
  g_autoptr(GPtrArray) clients = NULL;

  g_assert (CM_IS_MATRIX (self));

  if (!accounts || !accounts->len)
    return;

  g_debug ("(%p) Load %u account secrets", self, accounts->len);

  g_assert (SECRET_IS_RETRIEVABLE (accounts->pdata[0]));

  clients = g_ptr_array_new_full (accounts->len, g_object_unref);

  for (guint i = 0; i < accounts->len; i++)
    {
      g_autoptr(CmClient) client = NULL;

      client = cm_client_new_from_secret (accounts->pdata[i], self->cm_db);

      if (!client)
        continue;

      g_ptr_array_add (clients, client);

      if (!g_object_get_data (G_OBJECT (self->secret_store), "force-save")) {
        g_list_store_append (self->clients_list, client);

        if (!self->disable_auto_login)
          cm_client_enable_as_in_store (client);
      }
    }

  if (g_object_get_data (G_OBJECT (self->secret_store), "force-save")) {
    GTask *task;

    task = g_task_new (self, NULL, NULL, NULL);
    g_task_set_task_data (task, g_ptr_array_ref (clients), (GDestroyNotify)g_ptr_array_unref);
    matrix_save_client (NULL, NULL, task);
  }
}

static void
db_open_cb (GObject      *obj,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  CmMatrix *self;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_MATRIX (self));

  self->db_loaded = cm_db_open_finish (self->cm_db, result, &error);
  self->is_opening = FALSE;
  g_debug ("(%p) Open DB %s", self, CM_LOG_SUCCESS (self->db_loaded));

  if (!self->db_loaded)
    {
      g_clear_object (&self->cm_db);
      g_warning ("(%p) Open DB error: %s", self, error ? error->message : "");
      g_task_return_error (task, error);
      return;
    }
  else
    {
      GPtrArray *accounts;

      accounts = g_task_get_task_data (task);
      load_accounts_from_secrets (self, accounts);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_READY]);
      g_task_return_boolean (task,  self->db_loaded && self->secrets_loaded);
    }
}

static void
matrix_store_load_cb (GObject      *object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  CmMatrix *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GPtrArray) accounts = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_MATRIX (self));

  accounts = cm_secret_store_load_finish (self->secret_store, result, &error);
  self->is_opening = FALSE;
  if (!error)
    self->secrets_loaded = TRUE;

  g_debug ("(%p) Load secrets %s", self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_warning ("(%p) Load secrets error: %s", self, error->message);
      g_task_return_error (task, error);
      return;
    }

  if (self->db_loaded)
    {
      load_accounts_from_secrets (self, accounts);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_READY]);
      g_task_return_boolean (task, TRUE);
    }
  else
    {
      self->is_opening = TRUE;
      if (accounts)
        g_task_set_task_data (task, g_steal_pointer (&accounts),
                              (GDestroyNotify)g_ptr_array_unref);

      self->cm_db = cm_db_new ();

      g_debug ("(%p) Open DB", self);
      cm_db_open_async (self->cm_db,
                        g_strdup (self->db_path), self->db_name,
                        db_open_cb, g_steal_pointer (&task));
    }
}

/**
 * cm_matrix_open_async:
 * @db_path: The path where db is (to be) stored
 * @db_name: The name of database
 * @cancellable: (nullable): A #GCancellable
 * @callback: The callback to run when ready
 * @user_data: user data for @callback
 *
 * Open the matrix E2EE db which shall be used by clients
 * when required.
 *
 * Run cm_matrix_open_finish() to get the result.
 */
void
cm_matrix_open_async (CmMatrix            *self,
                      const char          *db_path,
                      const char          *db_name,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (CM_IS_MATRIX (self));
  g_return_if_fail (db_path && *db_path);
  g_return_if_fail (db_name && *db_name);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (!cm_matrix_is_ready (self));

  task = g_task_new (self, cancellable, callback, user_data);

  if (self->is_opening)
    {
      g_debug ("(%p) Open matrix already in progress", self);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Opening db in progress");
      return;
    }

  if (cm_matrix_is_ready (self))
    {
      g_debug ("(%p) Open matrix already succeeded", self);
      g_task_return_boolean (task, TRUE);
      return;
    }

  self->is_opening = TRUE;

  if (!self->db_path)
    self->db_path = g_strdup (db_path);

  if (!self->db_name)
    self->db_name = g_strdup (db_name);

  /* Don't load libsecret in tests as password request requires X11 */
  if (g_test_initialized ())
    self->secrets_loaded = TRUE;

  if (!self->secrets_loaded)
    {
      g_debug ("(%p) Load secrets", self);
      cm_secret_store_load_async (self->secret_store,
                                  cancellable,
                                  matrix_store_load_cb,
                                  g_steal_pointer (&task));
    }
  else if (!self->db_loaded)
    {
      self->cm_db = cm_db_new ();

      g_debug ("(%p) Open DB", self);
      cm_db_open_async (self->cm_db, g_strdup (db_path), db_name,
                        db_open_cb,
                        g_steal_pointer (&task));
    }
  else
    g_assert_not_reached ();
}

gboolean
cm_matrix_open_finish (CmMatrix      *self,
                       GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
cm_matrix_is_ready (CmMatrix *self)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);

  return self->db_loaded || self->secrets_loaded;
}

GListModel *
cm_matrix_get_clients_list (CmMatrix *self)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);

  return G_LIST_MODEL (self->clients_list);
}

gboolean
cm_matrix_has_client_with_id (CmMatrix   *self,
                              const char *user_id)
{
  GListModel *model;
  guint n_items;

  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);
  g_return_val_if_fail (user_id && *user_id, FALSE);

  model = G_LIST_MODEL (self->clients_list);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmClient) item = NULL;
      CmAccount *item_account;

      item = g_list_model_get_item (model, i);
      item_account = cm_client_get_account (item);

      if (g_strcmp0 (user_id, cm_account_get_login_id (item_account)) == 0)
        return TRUE;

      if (g_strcmp0 (user_id, cm_client_get_user_id (item)) == 0)
        return TRUE;
    }

  return g_hash_table_contains (self->clients_to_save, user_id);
}

/**
 * cm_matrix_client_new:
 * @self: A #CmMatrix
 *
 * Create a new #CmClient.  It's an error
 * to create a new client before opening
 the db with cm_matrix_open_async()
 *
 * Returns: (transfer full): A #CmClient
 */
CmClient *
cm_matrix_client_new (CmMatrix *self)
{
  CmClient *client;

  g_return_val_if_fail (CM_IS_MATRIX (self), NULL);

  if (!cm_matrix_is_ready (self))
    g_error ("(%p) DB not open, See cm_matrix_open_async()", self);

  client = g_object_new (CM_TYPE_CLIENT, NULL);
  /* Mark the client as not to save automatically unless asked explicitly
   * with cm_matrix_save_client_async() at least once.
   */
  g_object_set_data (G_OBJECT (client), "no-save", GINT_TO_POINTER (TRUE));
  cm_client_set_db (client, self->cm_db);

  g_debug ("(%p) New client %p created", self, client);

  return client;
}

static void
matrix_save_client_cb (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  CmMatrix *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  gboolean ret;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_MATRIX (self));

  ret = cm_client_save_secrets_finish (CM_CLIENT (object), result, &error);
  g_debug ("(%p) Save client %p %s", self, object, CM_LOG_SUCCESS (self->db_loaded));

  if (error)
    {
      g_warning ("(%p) Save client %p error: %s", self, object, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      CmAccount *account;

      account = cm_client_get_account (CM_CLIENT (object));
      g_list_store_append (self->clients_list, object);
      g_hash_table_remove (self->clients_to_save,
                           cm_account_get_login_id (account));
      g_task_return_boolean (task, ret);
    }

  g_object_set_data (object, "enable", GINT_TO_POINTER (FALSE));
}

void
cm_matrix_save_client_async (CmMatrix            *self,
                             CmClient            *client,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  const char *login_id;
  CmAccount *account;
  GTask *task;

  g_return_if_fail (CM_IS_MATRIX (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  account = cm_client_get_account (client);
  g_return_if_fail (cm_account_get_login_id (account));
  /* user id is set after login, which should be set only by cmatrix */
  g_return_if_fail (!cm_client_get_user_id (client));
  g_return_if_fail (cm_client_get_homeserver (client));

  task = g_task_new (self, NULL, callback, user_data);
  g_object_set_data (G_OBJECT (client), "no-save", GINT_TO_POINTER (FALSE));
  g_debug ("(%p) Save client %p", self, client);

  if (matrix_has_client (self, client, TRUE))
    {
      g_debug ("(%p) Save client %p error, user exists", self, client);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_EXISTS,
                               "User already exists");
      return;
    }

  login_id = cm_account_get_login_id (account);
  g_hash_table_insert (self->clients_to_save, g_strdup (login_id), g_object_ref (client));
  g_object_set_data (G_OBJECT (client), "enable", GINT_TO_POINTER (TRUE));
  cm_client_save_secrets_async (client,
                                matrix_save_client_cb,
                                task);
}

gboolean
cm_matrix_save_client_finish (CmMatrix      *self,
                              GAsyncResult  *result,
                              GError       **error)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
matrix_delete_client_cb (GObject      *object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  CmMatrix *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  gboolean ret;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_MATRIX (self));

  ret = cm_client_delete_secrets_finish (CM_CLIENT (object), result, &error);
  g_debug ("(%p) Delete client %p %s", self, object, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_warning ("(%p) Delete client %p error: %s", self, object, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      CmClient *client;

      client = g_task_get_task_data (task);
      cm_utils_remove_list_item (self->clients_list, client);
      g_task_return_boolean (task, ret);
    }
}

void
cm_matrix_delete_client_async (CmMatrix            *self,
                               CmClient            *client,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_MATRIX (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  g_debug ("(%p) Delete client %p", self, client);
  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_task_data (task, g_object_ref (client), g_object_unref);

  cm_client_delete_secrets_async (client,
                                  matrix_delete_client_cb,
                                  task);
}

gboolean
cm_matrix_delete_client_finish (CmMatrix      *self,
                                GAsyncResult  *result,
                                GError       **error)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

CmDb *
cm_matrix_get_db (CmMatrix *self)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), NULL);

  return self->cm_db;
}

const char *
cm_matrix_get_data_dir (void)
{
  return cmatrix_data_dir;
}

const char *
cm_matrix_get_app_id (void)
{
  return cmatrix_app_id;
}

static void
matrix_save_client (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  CmMatrix *self;
  g_autoptr(GTask) task = user_data;
  GPtrArray *clients;
  CmClient *client;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_MATRIX (self));

  clients = g_task_get_task_data (task);
  client = (CmClient *)object;

  if (client &&
      cm_client_save_secrets_finish (client, result, NULL))
    {
      g_debug ("(%p) Save client %p done", self, CM_LOG_SUCCESS (TRUE));
      g_list_store_append (self->clients_list, CM_CLIENT (object));
      cm_client_enable_as_in_store (client);
    }

  if (!clients || !clients->len)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  client = g_ptr_array_steal_index (clients, 0);
  g_object_set_data_full (user_data, "client", client, g_object_unref);

  g_debug ("(%p) Save client %p, %u left to save", self, client, clients->len);
  cm_client_save_secrets_async (client,
                                matrix_save_client,
                                g_steal_pointer (&task));
}

void
cm_matrix_add_clients_async (CmMatrix            *self,
                             GPtrArray           *secrets,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  GPtrArray *clients;
  GTask *task;

  g_return_if_fail (CM_IS_MATRIX (self));
  g_return_if_fail (secrets && secrets->len);
  g_return_if_fail (SECRET_IS_RETRIEVABLE (secrets->pdata[0]));
  g_return_if_fail (cm_matrix_is_ready (self));

  clients = g_ptr_array_new_full (secrets->len, g_object_unref);
  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_task_data (task, clients, (GDestroyNotify)g_ptr_array_unref);

  for (guint i = 0; i < secrets->len; i++)
    {
      SecretRetrievable *secret = secrets->pdata[i];
      CmClient *client;

      client = cm_client_new_from_secret (secret, self->cm_db);
      if (client)
        g_ptr_array_add (clients, client);
      else
        g_warning ("(%p) Failed to create client from secret", self);
    }

  g_debug ("(%p) Save clients, count: %u", self, secrets->len);
  matrix_save_client (NULL, NULL, task);
}

gboolean
cm_matrix_add_clients_finish (CmMatrix      *self,
                              GAsyncResult  *result,
                              GError       **error)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
