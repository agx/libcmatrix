/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-client.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <libsoup/soup.h>

#include "cm-room-member-private.h"
#include "cm-net-private.h"
#include "cm-utils-private.h"
#include "cm-common.h"
#include "cm-db-private.h"
#include "cm-enc-private.h"
#include "cm-enums.h"
#include "users/cm-user-private.h"
#include "users/cm-account.h"
#include "cm-room-private.h"
#include "cm-room.h"
#include "cm-client-private.h"
#include "cm-client.h"

/**
 * SECTION: cm-client
 * @title: CmClient
 * @short_description:
 * @include: "cm-client.h"
 */

#define KEY_TIMEOUT         10000 /* milliseconds */
#define URI_REQUEST_TIMEOUT 30    /* seconds */
#define SYNC_TIMEOUT        30000 /* milliseconds */

struct _CmClient
{
  GObject         parent_instance;

  char           *user_id;
  /* @login_username can be email/[incomplete] matrix-id/phone-number etc */
  char           *login_user_id;
  char           *homeserver;
  char           *password;
  char           *device_id;
  char           *device_name;

  CmAccount      *cm_account;
  CmDb           *cm_db;
  CmNet          *cm_net;
  CmEnc          *cm_enc;
  GSocketAddress *gaddress;

  CmCallback      callback;
  gpointer        cb_data;
  GDestroyNotify  cb_destroy;

  GCancellable   *cancellable;
  char           *filter_id;
  char           *next_batch;
  char           *key;
  char           *pickle_key;

  /* direct_rooms are set on initial sync from 'account_data',
   * which will then be moved to joined_rooms later */
  GHashTable     *direct_rooms;
  GListStore     *joined_rooms;
  /* for sending events, incremented for each event */
  int             event_id;

  guint           resync_id;

  gboolean        db_migrated;
  gboolean        room_list_loading;
  gboolean        room_list_loaded;
  gboolean        direct_room_list_loading;
  gboolean        direct_room_list_loaded;

  gboolean        db_loading;
  gboolean        db_loaded;
  gboolean        client_enabled;
  gboolean        has_tried_connecting;
  gboolean        is_logging_in;
  /* Set if passsword is right/success using @access_token */
  gboolean        login_success;
  gboolean        is_sync;
  gboolean        sync_failed;
  gboolean        save_client_pending;
  gboolean        is_saving_client;
  gboolean        homeserver_verified;
};

G_DEFINE_TYPE (CmClient, cm_client, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_HOME_SERVER,
  PROP_USER_ID,
  N_PROPS
};

enum {
  STATUS_CHANGED,
  ACCESS_TOKEN_CHANGED,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

const char *filter_json_str = "{ \"room\": { "
  "  \"timeline\": { \"limit\": 20 }, "
  "  \"state\": { \"lazy_load_members\": true } "
  " }"
  "}";

static void     matrix_start_sync      (CmClient *self,
                                        gpointer  tsk);
static void     matrix_upload_key      (CmClient *self);
static gboolean handle_matrix_glitches (CmClient *self,
                                        GError    *error);

static void
cm_set_string_value (char       **strp,
                     const char  *value)
{
  g_assert (strp);

  g_free (*strp);
  *strp = g_strdup (value);
}

static void
client_set_login_state (CmClient *self,
                        gboolean  logging_in,
                        gboolean  logged_in)
{
  g_assert (CM_IS_CLIENT (self));

  /* We can't be in process of logging in and logged in the same time */
  if (logging_in)
    g_assert (!logged_in);

  if (self->is_logging_in == logging_in &&
      self->login_success == logged_in)
    return;

  self->is_logging_in = logging_in;
  self->login_success = logged_in;
  g_signal_emit (self, signals[STATUS_CHANGED], 0);
}

static void
client_reset_state (CmClient *self)
{
  g_assert (CM_IS_CLIENT (self));

  self->is_sync = FALSE;
  g_clear_pointer (&self->next_batch, g_free);
  g_clear_pointer (&self->key, g_free);
  g_clear_pointer (&self->pickle_key, gcry_free);
  g_clear_pointer (&self->filter_id, g_free);

  g_hash_table_remove_all (self->direct_rooms);
  g_list_store_remove_all (self->joined_rooms);
  cm_net_set_access_token (self->cm_net, NULL);
  cm_enc_set_details (self->cm_enc, NULL, NULL);
  client_set_login_state (self, FALSE, FALSE);
}

static void
send_json_cb (GObject      *obj,
              GAsyncResult *result,
              gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_TASK (task));

  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, g_steal_pointer (&root), (GDestroyNotify)json_object_unref);
}

static gboolean
schedule_resync (gpointer user_data)
{
  CmClient *self = user_data;
  gboolean sync_now;

  g_assert (CM_IS_CLIENT (self));
  self->resync_id = 0;

  sync_now = cm_client_can_connect (self);

  if (sync_now)
    matrix_start_sync (self, NULL);
  else
    self->resync_id = g_timeout_add_seconds (URI_REQUEST_TIMEOUT,
                                             schedule_resync, self);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_matrix_glitches (CmClient *self,
                        GError   *error)
{
  if (!error)
    return FALSE;

  if (g_error_matches (error, CM_ERROR, M_UNKNOWN_TOKEN) &&
      self->password)
    {
      client_reset_state (self);
      cm_db_delete_client_async (self->cm_db, self, NULL, NULL);
      self->callback (self->cb_data, self, CM_ACCESS_TOKEN_LOGIN, NULL, NULL, NULL);
      matrix_start_sync (self, NULL);

      return TRUE;
    }

  /*
   * The G_RESOLVER_ERROR may be suggesting that the hostname is wrong, but we don't
   * know if it's network/DNS/Proxy error. So keep retrying.
   */
  if ((error->domain == SOUP_HTTP_ERROR &&
       error->code <= SOUP_STATUS_TLS_FAILED &&
       error->code > SOUP_STATUS_CANCELLED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
      /* Should we handle connection_refused, or just keep it for localhost? */
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
      error->domain == G_RESOLVER_ERROR ||
      error->domain == JSON_PARSER_ERROR)
    {
      if (cm_client_can_connect (self))
        {
          g_clear_handle_id (&self->resync_id, g_source_remove);

          self->sync_failed = TRUE;
          g_signal_emit (self, signals[STATUS_CHANGED], 0);
          self->resync_id = g_timeout_add_seconds (URI_REQUEST_TIMEOUT,
                                                   schedule_resync, self);
          return TRUE;
        }
    }

  return FALSE;
}

static void
client_login_with_password_async (CmClient            *self,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  JsonObject *object, *child;
  GTask *task;

  g_assert (CM_IS_CLIENT (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_assert (self->login_user_id || self->user_id);
  g_assert (self->homeserver_verified);
  g_assert (self->password && *self->password);

  g_debug ("Logging in with '%s'", self->login_user_id);

  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-login */
  object = json_object_new ();
  json_object_set_string_member (object, "type", "m.login.password");
  json_object_set_string_member (object, "password", self->password);
  json_object_set_string_member (object, "initial_device_display_name", self->device_name ?: "CMatrix");

  child = json_object_new ();

  if (cm_utils_user_name_is_email (self->login_user_id))
    {
      json_object_set_string_member (child, "type", "m.id.thirdparty");
      json_object_set_string_member (child, "medium", "email");
      json_object_set_string_member (child, "address", self->login_user_id);
    }
  else
    {
      json_object_set_string_member (child, "type", "m.id.user");
      json_object_set_string_member (child, "user", self->login_user_id);
    }

  json_object_set_object_member (object, "identifier", child);

  task = g_task_new (self, cancellable, callback, user_data);
  cm_net_send_json_async (self->cm_net, 2, object,
                          "/_matrix/client/r0/login", SOUP_METHOD_POST,
                          NULL, cancellable, send_json_cb,
                          task);
}

static void
cm_upload_filter_cb (GObject      *obj,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      if (!handle_matrix_glitches (self, error))
        g_warning ("Error uploading filter: %s", error->message);

      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, NULL, NULL);
  self->filter_id = g_strdup (cm_utils_json_object_get_string (root, "filter_id"));
  g_debug ("Uploaded filter, id: %s", self->filter_id);

  client_set_login_state (self, FALSE, TRUE);

  if (!self->filter_id)
    self->filter_id = g_strdup ("");

  self->save_client_pending = TRUE;
  cm_client_save (self, FALSE);

  matrix_start_sync (self, NULL);
}

static void
matrix_upload_filter (CmClient *self,
                      gpointer  tsk)
{
  g_autoptr(GTask) task = tsk;
  g_autoptr(GError) error = NULL;
  GCancellable *cancellable;
  g_autofree char *uri = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonObject *filter = NULL;
  JsonNode *root = NULL;

  g_debug ("Uploading filter, user: %s", self->user_id);

  parser = json_parser_new ();
  json_parser_load_from_data (parser, filter_json_str, -1, &error);

  if (error)
    g_warning ("Error parsing filter file: %s", error->message);

  if (!error)
    root = json_parser_get_root (parser);

  if (root)
    filter = json_node_get_object (root);

  if (error || !root || !filter)
    {
      if (error)
        g_warning ("Error getting filter file: %s", error->message);

      self->filter_id = g_strdup ("");
      /* Even if we have error uploading filter, continue syncing */
      matrix_start_sync (self, NULL);

      return;
    }

  cancellable = g_task_get_cancellable (task);
  uri = g_strconcat ("/_matrix/client/r0/user/", self->user_id, "/filter", NULL);
  cm_net_send_json_async (self->cm_net, 2, json_object_ref (filter),
                          uri, SOUP_METHOD_POST,
                          NULL, cancellable, cm_upload_filter_cb,
                          g_steal_pointer (&task));
}

static void
cm_client_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  CmClient *self = (CmClient *)object;

  switch (prop_id)
    {
    case PROP_HOME_SERVER:
      cm_client_set_homeserver (self, g_value_get_string (value));
      break;

    case PROP_USER_ID:
      cm_client_set_user_id (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cm_client_finalize (GObject *object)
{
  CmClient *self = (CmClient *)object;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  g_clear_object (&self->cm_account);
  g_clear_object (&self->cm_net);

  g_list_store_remove_all (self->joined_rooms);
  g_clear_object (&self->joined_rooms);

  g_hash_table_unref (self->direct_rooms);

  g_free (self->user_id);
  g_free (self->login_user_id);
  g_free (self->homeserver);
  g_free (self->device_id);
  g_free (self->device_name);
  gcry_free (self->password);
  gcry_free (self->pickle_key);

  G_OBJECT_CLASS (cm_client_parent_class)->finalize (object);
}

static void
cm_client_class_init (CmClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = cm_client_set_property;
  object_class->finalize = cm_client_finalize;

  properties[PROP_HOME_SERVER] =
    g_param_spec_string ("home-server",
                         "Home Server",
                         "Matrix Home Server",
                         NULL,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

  properties[PROP_USER_ID] =
    g_param_spec_string ("user-id",
                         "User ID",
                         "Matrix User ID",
                         NULL,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * ChattyItem::status-changed:
   * @self: a #ChattyItem
   *
   * status-changed signal is emitted when the client's
   * login and sync status are changed.
   */
  signals [STATUS_CHANGED] =
    g_signal_new ("status-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
cm_client_init (CmClient *self)
{
  self->cm_net = cm_net_new ();
  self->cancellable = g_cancellable_new ();
  self->joined_rooms = g_list_store_new (CM_TYPE_ROOM);
  self->direct_rooms = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, g_object_unref);
}

/**
 * cm_client_new:
 *
 * Returns: (transfer full): A #CmClient
 */
CmClient *
cm_client_new (void)
{
  return g_object_new (CM_TYPE_CLIENT, NULL);
}

CmAccount *
cm_client_get_account (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  if (!self->cm_account)
    {
      self->cm_account = g_object_new (CM_TYPE_ACCOUNT, NULL);
      cm_user_set_client (CM_USER (self->cm_account), self);
    }

  return self->cm_account;
}

static void
db_load_client_cb (GObject      *obj,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  CmClient *self;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  g_assert (!self->cm_enc);

  self->db_loaded = TRUE;
  self->db_loading = FALSE;

  if (!cm_db_load_client_finish (self->cm_db, result, &error))
    {
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_warning ("Error loading client '%s': %s",
                   cm_client_get_user_id (self), error->message);

      /* We can load further even if fail to load from db */
      /* XXX: handle difference between if the user is missing from db and failed fetching data */
      matrix_start_sync (self, g_steal_pointer (&task));
      return;
    }

  if (g_object_get_data (G_OBJECT (result), "pickle") && self->pickle_key)
    {
      const char *pickle;

      pickle = g_object_get_data (G_OBJECT (result), "pickle");
      self->cm_enc = cm_enc_new (self->cm_db, pickle, self->pickle_key);
    }

  if (self->cm_enc)
    cm_enc_set_details (self->cm_enc,
                        cm_client_get_user_id (self),
                        cm_client_get_device_id (self));
  else
    g_clear_pointer (&self->pickle_key, gcry_free);

  if (g_object_get_data (G_OBJECT (result), "rooms"))
    {
      g_autoptr(GPtrArray) rooms = NULL;

      rooms = g_object_steal_data (G_OBJECT (result), "rooms");

      for (guint i = 0; i < rooms->len; i++)
        {
          CmRoom *room = rooms->pdata[i];
          cm_room_set_client (room, self);
        }

      g_list_store_splice (self->joined_rooms, 0, 0, rooms->pdata, rooms->len);
    }

  self->db_migrated = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (result), "db-migrated"));
  self->filter_id = g_strdup (g_object_get_data (G_OBJECT (result), "filter-id"));
  self->next_batch = g_strdup (g_object_get_data (G_OBJECT (result), "batch"));
  matrix_start_sync (self, g_steal_pointer (&task));
}

/*
 * cm_client_pop_event_id:
 * @self: A #CmClient
 *
 * Get a number to be used as event suffix.
 * The number is incremented per request,
 * so you always get a different number.
 *
 * Returns: An integer
 */
int
cm_client_pop_event_id (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), 0);

  self->event_id++;

  return self->event_id - 1;
}

/*
 * cm_client_get_db:
 * @self: A #CmClient
 *
 * Get the #CmDb associated with the client.
 * Can be %NULL if client hasn't loaded yet.
 *
 * Returns: (transfer none): A #CmDb
 */
CmDb *
cm_client_get_db (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->cm_db;
}

/*
 * cm_client_get_net:
 * @self: A #CmClient
 *
 * Get the #CmNet associated with the client
 *
 * Returns: (transfer none): A #CmNet
 */
CmNet *
cm_client_get_net (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->cm_net;
}

/*
 * cm_client_get_enc:
 * @self: A #CmClient
 *
 * Get the #CmEnc associated with the client,
 * Can be %NULL if not loaded/logged in yet.
 *
 * Returns: (transfer none): A #CmEnc
 */
CmEnc *
cm_client_get_enc (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->cm_enc;
}

/*
 * cm_client_set_db:
 * @self: A #CmClient
 * @db: A #CmDb
 *
 * Set the db object for the client.
 * There is at most one db object which
 * is shared for every client.
 *
 * This should be set only after user ID
 * and device ID is set.
 *
 * @self shall load the client info immediately
 * after db is set and enable if the account
 * if that's how it's set in db.
 */
void
cm_client_set_db (CmClient *self,
                  CmDb     *db)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (CM_IS_DB (db));

  if (self->cm_db)
    return;

  self->cm_db = g_object_ref (db);
}

const char *
cm_client_get_filter_id (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  if (self->filter_id && *self->filter_id)
    return self->filter_id;

  return NULL;
}

/**
 * cm_client_set_enabled:
 * @self: A #CmClient
 * @enable: Whether to enable client
 *
 * Whether to enable the client and begin
 * sync with the server.
 */
void
cm_client_set_enabled (CmClient *self,
                       gboolean  enable)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  if (enable)
    g_return_if_fail (self->cm_db);

  enable = !!enable;

  if (self->client_enabled == enable)
    return;

  self->client_enabled = enable;
  g_signal_emit (self, signals[STATUS_CHANGED], 0);

  if (self->client_enabled)
    {
      matrix_start_sync (self, NULL);
    }
  else
    {
      cm_client_stop_sync (self);
    }
}

static void
db_save_cb (GObject      *object,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(GError) error = NULL;
  gboolean status, save_pending;

  status = cm_db_save_client_finish (self->cm_db, result, &error);
  self->is_saving_client = FALSE;

  save_pending = self->save_client_pending;

  if (error || !status)
    self->save_client_pending = TRUE;

  if (error)
    g_warning ("Error saving to db: %s", error->message);

  /* If settings changed when we were saving the current settings, repeat. */
  if (save_pending)
    cm_client_save (self, FALSE);
}

/*
 * cm_client_set_enabled:
 * @self: A #CmClient
 * @force: Whether to force saving to db
 *
 * Save the changes to associated CmDb.
 * Set @force to %TRUE to force saving to
 * db even when no changes are made
 */
void
cm_client_save (CmClient *self,
                gboolean  force)
{
  char *pickle = NULL;

  g_return_if_fail (CM_IS_CLIENT (self));

  if ((!self->save_client_pending && !force) ||
      self->is_saving_client)
    return;

  self->is_saving_client = TRUE;
  self->save_client_pending = FALSE;

  if (self->cm_enc)
    pickle = cm_enc_get_pickle (self->cm_enc);
  cm_db_save_client_async (self->cm_db, self, pickle,
                           db_save_cb,
                           g_object_ref (self));
}

/**
 * cm_client_get_enabled:
 * @self: A #CmClient
 *
 * Wheter @self has been enabled.
 *
 * Enabled client doesn't necessarily mean the client
 * has logged in or is in sync with server.
 * Also See cm_client_is_sync().
 *
 * Returns: %TRUE if @self has been enabled, %FALSE
 * otherwise
 */
gboolean
cm_client_get_enabled (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);

  return self->client_enabled;
}

/**
 * cm_client_set_sync_callback:
 * @self: A #CmClient
 * @callback: A #CmCallback
 * @object: A #GObject derived object for @callback user_data
 *
 * Set the sync callback which shall be executed for the
 * events happening in @self.  You shall be able to the
 * callback only once.
 */
void
cm_client_set_sync_callback (CmClient       *self,
                             CmCallback      callback,
                             gpointer        callback_data,
                             GDestroyNotify  callback_data_destroy)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (callback);
  g_return_if_fail (!self->callback);

  self->callback = callback;
  self->cb_data = callback_data;
  self->cb_destroy = callback_data_destroy;
}

/**
 * cm_client_set_user_id:
 * @self: A #CmClient
 * @matrix_user_id: A fully qualified matrix user ID
 *
 * Set the client matrix user ID.  This can be set only
 * before the account has been enabled and never after.
 *
 * Returns: %TRUE if @matrix_user_id was successfully set,
 * %FALSE otherwise.
 */
gboolean
cm_client_set_user_id (CmClient   *self,
                       const char *matrix_user_id)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (!self->is_logging_in, FALSE);
  g_return_val_if_fail (!self->login_success, FALSE);

  if (!cm_utils_user_name_valid (matrix_user_id))
    return FALSE;

  g_free (self->user_id);
  self->user_id = g_ascii_strdown (matrix_user_id, -1);

  return TRUE;
}

/**
 * cm_client_get_user_id:
 * @self: A #CmClient
 *
 * Get the matrix user ID of the client @self.
 * user ID may be available only after the
 * login has succeeded and may return %NULL
 * otherwise.
 *
 * Returns: (nullable): The matrix user ID of the client
 */
const char *
cm_client_get_user_id (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->user_id;
}

/**
 * cm_client_set_login_id:
 * @self: A #CmClient
 * @login_id: A login ID for the client.
 *
 * Set login ID for the client.  The login
 * can be a fully qualified matrix ID, or
 * an email address which shall be used to log
 * in to the server.
 *
 * Returns: %TRUE if @login_id was successfully set,
 * %FALSE otherwise.
 */
gboolean
cm_client_set_login_id (CmClient   *self,
                        const char *login_id)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (!self->is_logging_in, FALSE);
  g_return_val_if_fail (!self->login_success, FALSE);

  if (cm_utils_user_name_valid (login_id) ||
      cm_utils_user_name_is_email (login_id))
    {
      g_free (self->login_user_id);
      self->login_user_id = g_ascii_strdown (login_id, -1);

      return TRUE;
    }

  return FALSE;
}

/**
 * cm_client_get_login_id:
 * @self: A #CmClient
 *
 * Get the login ID set for the client
 *
 * Returns: The login ID for the client
 */
const char *
cm_client_get_login_id (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->login_user_id;
}

/**
 * cm_client_set_homeserver:
 * @self: A #CmClient
 * @homeserver: The homserver URL
 *
 * Set the matrix Home server URL for the client.
 *
 * Returns: %TRUE if @homeserver is valid,
 * %FALSE otherwise.
 */
gboolean
cm_client_set_homeserver (CmClient   *self,
                          const char *homeserver)
{
  g_autoptr(SoupURI) uri = NULL;
  GString *server;

  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (!self->is_logging_in, FALSE);
  g_return_val_if_fail (!self->login_success, FALSE);

  if (!homeserver || !*homeserver)
    return FALSE;

  if (!g_str_has_prefix (homeserver, "http://") &&
      !g_str_has_prefix (homeserver, "https://"))
    return FALSE;

  if (!cm_utils_home_server_valid (homeserver))
    return FALSE;

  server = g_string_new (homeserver);
  if (g_str_has_suffix (server->str, "/"))
    g_string_truncate (server, server->len - 1);

  g_free (self->homeserver);
  self->homeserver = g_string_free (server, FALSE);
  cm_net_set_homeserver (self->cm_net, homeserver);

  return TRUE;
}

/**
 * cm_client_get_homeserver:
 * @self: A #CmClient
 *
 * Get the Home server set for the client.
 *
 * Returns: (nullable): The Home server for the client
 */
const char *
cm_client_get_homeserver (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  if (self->homeserver && *self->homeserver)
    return self->homeserver;

  return NULL;
}

/**
 * cm_client_set_password:
 * @self: A #CmClient
 * @password: The password for the @self
 *
 * Set the password for the client.  You can't
 * set the password once the client is enabled,
 * except in the callback when the client failed
 * to login due to wrong password.
 */
void
cm_client_set_password (CmClient   *self,
                        const char *password)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (!self->is_logging_in);
  g_return_if_fail (!self->login_success);

  gcry_free (self->password);
  self->password = gcry_malloc_secure (strlen (password) + 1);
  strcpy (self->password, password);
}

/**
 * cm_client_get_password:
 * @self: A #CmClient
 *
 * Get the password set for the @self.
 *
 * Returns: The password string
 */
const char *
cm_client_get_password (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->password;
}

/**
 * cm_client_set_access_token:
 * @self: A #CmClient
 * @access_token: (nullable): The access token
 *
 * Set the access token to be used for sync.
 * If set to %NULL, @self shall login using password
 * And use the newly got access token for sync.
 */
void
cm_client_set_access_token (CmClient   *self,
                            const char *access_token)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (!self->is_logging_in);
  g_return_if_fail (!self->login_success);

  cm_net_set_access_token (self->cm_net, access_token);
}

/**
 * cm_client_get_access_token:
 * @self: A #CmClient
 *
 * Get the access token of the client @self
 *
 * Returns: (nullable): The access token set
 */
const char *
cm_client_get_access_token (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return cm_net_get_access_token (self->cm_net);
}

/*
 * cm_client_get_next_batch:
 * @self: A #CmClient
 *
 * Get the next batch of the client @self
 * for the last /sync request
 *
 * Returns: (nullable): The next batch if set
 */
const char *
cm_client_get_next_batch (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->next_batch;
}

/**
 * cm_client_set_device_id:
 * @self: A #CmClient
 * @device_id: The device ID
 *
 * Set the device ID for @client.
 */
void
cm_client_set_device_id (CmClient   *self,
                         const char *device_id)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (!self->is_logging_in);
  g_return_if_fail (!self->login_success);

  g_free (self->device_id);
  self->device_id = g_strdup (device_id);
}

/**
 * cm_client_get_device_id:
 * @self: A #CmClient
 *
 * Get the device ID of the client @self.
 * Device ID may be available only after
 * a successful login.
 *
 * Returns: (nullable): The Device ID string
 */
const char *
cm_client_get_device_id (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->device_id;
}

/**
 * cm_client_set_device_name:
 * @self: A #CmClient
 * @device_name: The device name string
 *
 * Set the device name which shall be used as
 * the human readable identifier for the device
 */
void
cm_client_set_device_name (CmClient   *self,
                           const char *device_name)
{
  g_return_if_fail (CM_IS_CLIENT (self));

  g_free (self->device_name);
  self->device_name = g_strdup (device_name);
}

/**
 * cm_client_get_device_name:
 * @self: A #CmClient
 *
 * Get the device name of the client @self
 * as set with cm_client_set_device_name().
 *
 * Returns: (nullable): The Device name string
 */
const char *
cm_client_get_device_name (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->device_name;
}

/**
 * cm_client_set_pickle_key:
 * @self: A #CmClient
 * @pickle_key: The pickle key
 *
 * Set the account pickle key.  This key is used as
 * the password to decrypt the encrypted pickle loaded
 * from db, and so, this should be set before the db
 * is set.
 */
void
cm_client_set_pickle_key (CmClient   *self,
                          const char *pickle_key)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (!self->pickle_key);

  if (pickle_key && *pickle_key)
    {
      self->pickle_key = gcry_malloc_secure (strlen (pickle_key) + 1);
      strcpy (self->pickle_key, pickle_key);
    }
}

/**
 * cm_client_get_pickle_key:
 * @self: A #CmClient
 *
 * Get the pickle password
 */
const char *
cm_client_get_pickle_key (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  if (self->cm_enc)
    return cm_enc_get_pickle_key (self->cm_enc);

  return NULL;
}

/**
 * cm_client_get_ed25519_key:
 * @self: A #CmClient
 *
 * Get the public ed25519 key of own device.
 */
const char *
cm_client_get_ed25519_key (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  if (self->cm_enc)
    return cm_enc_get_ed25519_key (self->cm_enc);

  return NULL;
}

void
cm_client_get_homeserver_async (CmClient            *self,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (CM_IS_CLIENT (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, cm_client_get_homeserver_async);

  if (self->homeserver_verified && self->homeserver && *(self->homeserver))
    {
      g_task_return_pointer (task, self->homeserver, NULL);
      return;
    }

  if (!self->user_id && !self->homeserver)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No user id present in client");
      return;
    }

  matrix_start_sync (self, g_steal_pointer (&task));
}

const char *
cm_client_get_homeserver_finish (CmClient      *self,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
client_verify_homeserver_cb (GObject      *obj,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  client_set_login_state (self, FALSE, FALSE);
  self->homeserver_verified = cm_utils_verify_homeserver_finish (result, &error);
  g_object_set_data (G_OBJECT (task), "action", "verify-homeserver");

  g_debug ("Verifying home server, has error: %d, home server: %s",
           error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED),
           self->homeserver);

  /* Since GTask can't have timeout, We cancel the cancellable to fake timeout */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    {
      g_clear_object (&self->cancellable);
      self->cancellable = g_cancellable_new ();
    }

  /* self->has_tried_connecting = TRUE; */

  /* if (handle_common_errors (self, error)) */
  /*   return; */

  if (self->homeserver_verified)
    {
      g_clear_object (&self->gaddress);
      self->gaddress = g_object_steal_data (G_OBJECT (result), "address");

      if (g_task_get_source_tag (task) == cm_client_get_homeserver_async)
        g_task_return_pointer (task, self->homeserver, NULL);
      else
        matrix_start_sync (self, g_steal_pointer (&task));
    }
  else
    {
      self->sync_failed = TRUE;

      g_signal_emit (self, signals[STATUS_CHANGED], 0);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to verify homeserver");
      if (self->callback)
        self->callback (self->cb_data, self, CM_VERIFY_HOMESERVER, NULL, NULL, error);
    }
}

static void
client_password_login_cb (GObject      *obj,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonObject) root = NULL;
  JsonObject *object = NULL;
  const char *value;
  CmClient *self;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      self->sync_failed = TRUE;

      g_debug ("Login failed, username: %s", self->login_user_id);
      if (error->code == M_FORBIDDEN)
        error->code = M_BAD_PASSWORD;

      client_set_login_state (self, FALSE, FALSE);

      if (!handle_matrix_glitches (self, error) && self->callback)
        self->callback (self->cb_data, self, CM_PASSWORD_LOGIN, NULL, NULL, error);

      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-login */
  value = cm_utils_json_object_get_string (root, "user_id");
  cm_set_string_value (&self->user_id, value);

  value = cm_utils_json_object_get_string (root, "access_token");
  cm_net_set_access_token (self->cm_net, value);

  value = cm_utils_json_object_get_string (root, "device_id");
  cm_set_string_value (&self->device_id, value);

  object = cm_utils_json_object_get_object (root, "well_known");
  object = cm_utils_json_object_get_object (object, "m.homeserver");
  value = cm_utils_json_object_get_string (object, "base_url");
  g_clear_object (&self->cm_enc);
  self->cm_enc = cm_enc_new (self->cm_db, NULL, NULL);
  cm_enc_set_details (self->cm_enc,
                      cm_client_get_user_id (self),
                      cm_client_get_device_id (self));
  cm_set_string_value (&self->key, cm_enc_get_device_keys_json (self->cm_enc));
  self->is_logging_in = FALSE;
  cm_client_set_homeserver (self, value);
  client_set_login_state (self, FALSE, !!cm_net_get_access_token (self->cm_net));
  cm_client_save (self, TRUE);

  g_assert (self->callback);
  self->callback (self->cb_data, self, CM_PASSWORD_LOGIN, NULL, NULL, NULL);

  g_debug ("Login success: %d, username: %s", self->login_success, self->login_user_id);

  matrix_start_sync (self, g_steal_pointer (&task));
}

static gboolean
room_matches_id (CmRoom     *room,
                 const char *room_id)
{
  const char *item_room_id;

  g_assert (CM_IS_ROOM (room));

  item_room_id = cm_room_get_id (room);

  if (g_strcmp0 (room_id, item_room_id) == 0)
    return TRUE;

  return FALSE;
}

static CmRoom *
client_find_room (CmClient   *self,
                  const char *room_id)
{
  guint n_items;

  g_assert (CM_IS_CLIENT (self));
  g_assert (room_id && *room_id);

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->joined_rooms));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmRoom) room = NULL;

      room = g_list_model_get_item (G_LIST_MODEL (self->joined_rooms), i);
      if (room_matches_id (room, room_id))
        return room;
    }

  return NULL;
}

static void
room_loaded_cb (GObject      *obj,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(GError) error = NULL;
  CmRoom *room = CM_ROOM (obj);

  cm_room_load_finish (room, result, &error);
}

static gboolean
handle_one_time_keys (CmClient   *self,
                      JsonObject *object)
{
  size_t count, limit;

  g_assert (CM_IS_CLIENT (self));

  if (!object)
    return FALSE;

  count = cm_utils_json_object_get_int (object, "signed_curve25519");
  limit = cm_enc_max_one_time_keys (self->cm_enc) / 2;

  /* If we don't have enough onetime keys add some */
  if (count < limit)
    {
      /* First, get all already created keys */
      if (!self->key)
        self->key = cm_enc_get_one_time_keys_json (self->cm_enc);

      /* If we got no keys, create new ones and get them */
      if (!self->key)
        {
          g_debug ("generating %" G_GSIZE_FORMAT " onetime keys", limit - count);
          cm_enc_create_one_time_keys (self->cm_enc, limit - count);
          self->key = cm_enc_get_one_time_keys_json (self->cm_enc);
        }
      matrix_upload_key (self);

      return TRUE;
    }

  return FALSE;
}

static void
upload_key_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GError) error = NULL;
  JsonObject *object = NULL;
  g_autofree char *json_str = NULL;

  g_assert (CM_IS_CLIENT (self));
  g_assert (G_IS_TASK (result));

  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      self->sync_failed = TRUE;
      if (!handle_matrix_glitches (self, error))
        self->callback (self->cb_data, self, CM_UPLOAD_KEY, NULL, NULL, error);
      g_debug ("Error uploading key: %s", error->message);
      return;
    }

  json_str = cm_utils_json_object_to_string (root, FALSE);
  cm_enc_publish_one_time_keys (self->cm_enc);
  self->callback (self->cb_data, self, CM_UPLOAD_KEY, NULL, json_str, NULL);

  object = cm_utils_json_object_get_object (root, "one_time_key_counts");

  if (!handle_one_time_keys (self, object))
    matrix_start_sync (self, NULL);
}

static void
matrix_upload_key (CmClient *self)
{
  char *key;

  g_assert (CM_IS_CLIENT (self));
  g_assert (self->key);

  key = g_steal_pointer (&self->key);

  cm_net_send_data_async (self->cm_net, 2, key, strlen (key),
                          "/_matrix/client/r0/keys/upload", SOUP_METHOD_POST,
                          NULL, self->cancellable, upload_key_cb,
                          g_object_ref (self));
}

/* Get the list of chat rooms marked as direct */
static void
parse_direct_rooms (CmClient   *self,
                    JsonObject *root)
{
  g_autoptr(GList) user_ids = NULL;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  user_ids = json_object_get_members (root);

  for (GList *user_id = user_ids; user_id; user_id = user_id->next)
    {
      JsonArray *array;
      guint length = 0;

      array = cm_utils_json_object_get_array (root, user_id->data);
      if (array)
        length = json_array_get_length (array);

      for (guint i = 0; i < length; i++)
        {
          CmRoom *room;
          const char *room_id;

          room_id = json_array_get_string_element (array, i);

          room = g_hash_table_lookup (self->direct_rooms, room_id);

          if (!room)
            room = client_find_room (self, room_id);

          if (room)
            {
              cm_room_set_generated_name (room, user_id->data);
              cm_room_set_is_direct (room, TRUE);
              /* cm_room_save (room); */

              continue;
            }

          room = cm_room_new (room_id);
          cm_room_set_client (room, self);
          cm_room_set_is_direct (room, TRUE);
          cm_room_set_generated_name (room, user_id->data);
          /* cm_room_save (room); */

          /* This eats the ref on the new room */
          g_hash_table_insert (self->direct_rooms, g_strdup (room_id), room);
        }
    }

}

static void
handle_account_data (CmClient   *self,
                     JsonObject *root)
{
  JsonObject *object, *content;
  JsonArray *array;
  guint length = 0;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  array = cm_utils_json_object_get_array (root, "events");
  if (array)
    length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      const char *type;

      object = json_array_get_object_element (array, i);
      type = cm_utils_json_object_get_string (object, "type");

      if (g_strcmp0 (type, "m.direct") != 0)
        continue;

      content = cm_utils_json_object_get_object (object, "content");

      if (!content)
        break;

      parse_direct_rooms (self, content);
    }
}

static void
handle_to_device (CmClient   *self,
                  JsonObject *root)
{
  JsonObject *object;
  JsonArray *array;
  guint length = 0;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  array = cm_utils_json_object_get_array (root, "events");
  if (array)
    length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      const char *type;

      object = json_array_get_object_element (array, i);
      type = cm_utils_json_object_get_string (object, "type");

      if (g_strcmp0 (type, "m.room.encrypted") == 0)
        cm_enc_handle_room_encrypted (self->cm_enc, object);
    }
}

static void
handle_room_join (CmClient   *self,
                  JsonObject *root)
{
  g_autoptr(GList) joined_room_ids = NULL;
  JsonObject *object;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  joined_room_ids = json_object_get_members (root);

  for (GList *room_id = joined_room_ids; room_id; room_id = room_id->next)
    {
      CmRoom *room;
      JsonObject *room_data;

      room = client_find_room (self, room_id->data);
      room_data = cm_utils_json_object_get_object (root, room_id->data);

      if (!room)
        {
          room = g_hash_table_lookup (self->direct_rooms, room_id->data);

          if (room)
            {
              g_list_store_append (self->joined_rooms, room);
              g_hash_table_remove (self->direct_rooms, room_id->data);
            }
          else
            {
              room = cm_room_new (room_id->data);
              cm_room_set_client (room, self);
              g_list_store_append (self->joined_rooms, room);
              g_object_unref (room);
            }

          cm_room_load_async (room, self->cancellable,
                              room_loaded_cb,
                              g_object_ref (self));
        }

      cm_room_set_data (room, room_data);
      object = cm_utils_json_object_get_object (room_data, "timeline");

      if (cm_utils_json_object_get_bool (object, "limited"))
        {
          const char *prev;

          prev = cm_utils_json_object_get_string (object, "prev_batch");
          cm_room_set_prev_batch (room, prev);
          cm_room_save (room);
        }
    }
}

static void
handle_device_list (CmClient   *self,
                    JsonObject *root)
{
  JsonArray *users;
  guint length = 0;
  guint n_items;

  if (!root)
    return;

  users = cm_utils_json_object_get_array (root, "changed");

  if (users)
    length = json_array_get_length (users);

  for (guint i = 0; i < length; i++)
    {
      const char *user_id;

      user_id = json_array_get_string_element (users, i);
      n_items = g_list_model_get_n_items (G_LIST_MODEL (self->joined_rooms));

      for (guint j = 0; j < n_items; j++)
        {
          g_autoptr(CmRoom) room = NULL;

          room = g_list_model_get_item (G_LIST_MODEL (self->joined_rooms), j);
          cm_room_user_changed (room, user_id);
        }
    }
}

static void
handle_red_pill (CmClient   *self,
                 JsonObject *root)
{
  JsonObject *object;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  handle_account_data (self, cm_utils_json_object_get_object (root, "account_data"));
  /* to_device should be handled first as it might contain keys to be used
   * to decrypt following events */
  handle_to_device (self, cm_utils_json_object_get_object (root, "to_device"));

  object = cm_utils_json_object_get_object (root, "rooms");
  handle_room_join (self, cm_utils_json_object_get_object (object, "join"));
  handle_device_list (self, cm_utils_json_object_get_object (root, "device_lists"));
}

static void
matrix_take_red_pill_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GError) error = NULL;
  JsonObject *object = NULL;

  g_assert (CM_IS_CLIENT (self));
  g_assert (G_IS_TASK (result));

  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      self->sync_failed = TRUE;
      client_set_login_state (self, FALSE, FALSE);
      if (!handle_matrix_glitches (self, error))
        self->callback (self->cb_data, self, CM_RED_PILL, NULL, NULL, error);
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Error syncing with time %s: %s", self->next_batch, error->message);
      return;
    }

  client_set_login_state (self, FALSE, TRUE);

  g_free (self->next_batch);
  self->next_batch = g_strdup (cm_utils_json_object_get_string (root, "next_batch"));
  cm_client_save (self, TRUE);

  {
    g_autofree char *json_str = NULL;

    json_str = cm_utils_json_object_to_string (root, FALSE);
    handle_red_pill (self, root);

    /* update variables only after the result is locally parsed  */
    if (self->sync_failed || !self->is_sync)
      {
        self->sync_failed = FALSE;
        self->is_sync = TRUE;
        g_signal_emit (self, signals[STATUS_CHANGED], 0);
      }

    self->callback (self->cb_data, self, CM_RED_PILL, NULL, json_str, NULL);
  }

  object = cm_utils_json_object_get_object (root, "device_one_time_keys_count");
  if (handle_one_time_keys (self, object))
    return;

  /* Repeat */
  matrix_start_sync (self, NULL);
}

static void
matrix_take_red_pill (CmClient *self,
                      gpointer  tsk)
{
  g_autoptr(GTask) task = tsk;
  GCancellable *cancellable;
  GHashTable *query;

  g_assert (CM_IS_CLIENT (self));

  query = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (self->login_success)
    g_hash_table_insert (query, g_strdup ("timeout"), g_strdup_printf ("%u", SYNC_TIMEOUT));
  else
    g_hash_table_insert (query, g_strdup ("timeout"), g_strdup_printf ("%u", SYNC_TIMEOUT / 1000));

  if (self->filter_id)
    g_hash_table_insert (query, g_strdup ("filter"), g_strdup (self->filter_id));

  if (self->next_batch)
    g_hash_table_insert (query, g_strdup ("since"), g_strdup (self->next_batch));

  cancellable = g_task_get_cancellable (task);
  cm_net_send_json_async (self->cm_net, 2, NULL,
                          "/_matrix/client/r0/sync", SOUP_METHOD_GET,
                          query, cancellable, matrix_take_red_pill_cb,
                          g_object_ref (self));
}

static void
client_get_homeserver_cb (GObject      *obj,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  char *homeserver;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  homeserver = cm_utils_get_homeserver_finish (result, &error);
  g_object_set_data (G_OBJECT (task), "action", "get-homeserver");

  g_debug ("Get home server, has error: %d, home server: %s",
           error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED), homeserver);

  client_set_login_state (self, FALSE, FALSE);

  if (error)
    {
      self->sync_failed = TRUE;

      if (g_task_get_source_tag (task) != cm_client_get_homeserver_async)
        handle_matrix_glitches (self, error);
      g_task_return_error (task, error);

      return;
    }

  if (!homeserver)
    {
      self->sync_failed = TRUE;
      g_task_return_new_error (task, CM_ERROR, M_NO_HOME_SERVER,
                               "Couldn't fetch homeserver");
      if (self->callback)
        self->callback (self->cb_data, self, CM_GET_HOMESERVER, NULL, NULL, error);

      return;
    }

  cm_client_set_homeserver (self, homeserver);

  if (!self->homeserver)
    {
      self->sync_failed = TRUE;
      g_task_return_new_error (task, CM_ERROR, M_BAD_HOME_SERVER,
                               "'%s' is not a valid URI", homeserver);
      return;
    }

  matrix_start_sync (self, g_steal_pointer (&task));
}

gboolean
cm_client_get_logging_in (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);

  return self->is_logging_in;
}

gboolean
cm_client_get_logged_in (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);

  return self->login_success;
}

static void
get_joined_rooms_cb (GObject      *obj,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (CM_IS_CLIENT (self));
  g_assert (G_IS_TASK (result));

  self->room_list_loading = FALSE;
  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (handle_matrix_glitches (self, error))
    return;

  if (!error) {
    JsonArray *array;
    guint length = 0;

    array = cm_utils_json_object_get_array (root, "joined_rooms");

    if (array)
      length = json_array_get_length (array);

    for (guint i = 0; i < length; i++)
      {
        const char *room_id;
        CmRoom *room;

        room_id = json_array_get_string_element (array, i);
        room = client_find_room (self, room_id);

        if (!room)
          {
            room = g_hash_table_lookup (self->direct_rooms, room_id);
            if (room)
              {
                g_list_store_append (self->joined_rooms, room);
                g_hash_table_remove (self->direct_rooms, room_id);
              }
          }

        if (!room)
          {
            room = cm_room_new (room_id);
            cm_room_set_client (room, self);
            g_list_store_append (self->joined_rooms, room);
            g_object_unref (room);
          }

        cm_room_load_async (room, self->cancellable,
                            room_loaded_cb,
                            g_object_ref (self));
      }

    self->room_list_loaded = TRUE;
    matrix_start_sync (self, NULL);
  }
}

static void
get_direct_rooms_cb (GObject      *obj,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (CM_IS_CLIENT (self));
  g_assert (G_IS_TASK (result));

  self->direct_room_list_loading = FALSE;
  root = g_task_propagate_pointer (G_TASK (result), &error);

  if (root)
    parse_direct_rooms (self, root);

  if (root || !error)
    self->direct_room_list_loaded = TRUE;

  matrix_start_sync (self, NULL);
}

static void
matrix_start_sync (CmClient *self,
                   gpointer  tsk)
{
  g_autoptr(GTask) task = tsk;
  GCancellable *cancellable = NULL;

  g_assert (CM_IS_CLIENT (self));

  self->sync_failed = FALSE;

  if (!task)
    {
      task = g_task_new (self, self->cancellable, NULL, NULL);
      cancellable = self->cancellable;
    }

  cancellable = g_task_get_cancellable (task);

  if (self->db_loading || self->room_list_loading || self->direct_room_list_loading)
    return;

  if (!self->db_loaded)
    {
      self->db_loading = TRUE;
      cm_db_load_client_async (self->cm_db, self,
                               cm_client_get_device_id (self),
                               db_load_client_cb,
                               g_steal_pointer (&task));
    }
  else if (!self->homeserver)
    {
      const char *user_id = self->user_id ?: self->login_user_id;

      if (!cm_utils_home_server_valid (self->homeserver) &&
          !cm_utils_user_name_valid (user_id))
        {
          g_debug ("Error: No Homeserver provided");

          g_task_return_new_error (task, CM_ERROR, M_NO_HOME_SERVER,
                                   "No Homeserver provided");
          return;
        }

      client_set_login_state (self, TRUE, FALSE);
      g_debug ("Getting homeserver, username: %s", self->user_id);
      cm_utils_get_homeserver_async (user_id, 30, cancellable,
                                     client_get_homeserver_cb,
                                     g_steal_pointer (&task));
    }
  else if (!self->homeserver_verified)
    {
      client_set_login_state (self, TRUE, FALSE);
      g_debug ("Verifying homeserver, homeserver: %s, username: %s",
               self->homeserver, self->user_id);
      cm_utils_verify_homeserver_async (self->homeserver, 30, cancellable,
                                        client_verify_homeserver_cb,
                                        g_steal_pointer (&task));
    }
  else if (!self->password && !cm_net_get_access_token (self->cm_net))
    {
      g_debug ("No password provided, nor access token");
      g_task_return_new_error (task, CM_ERROR, M_BAD_PASSWORD,
                               "No Password provided");
    }
  else if (!cm_net_get_access_token (self->cm_net) || !self->cm_enc)
    {
      g_assert (self->cm_db);
      cm_net_set_access_token (self->cm_net, NULL);
      client_set_login_state (self, TRUE, FALSE);
      client_login_with_password_async (self, cancellable,
                                        client_password_login_cb,
                                        g_steal_pointer (&task));
    }
  else if (self->db_migrated && !self->direct_room_list_loaded)
    {
      g_autofree char *uri = NULL;

      self->direct_room_list_loading = TRUE;

      uri = g_strconcat ("/_matrix/client/r0/user/", self->user_id,
                         "/account_data/m.direct", NULL);
      cm_net_send_json_async (self->cm_net, 0, NULL, uri, SOUP_METHOD_GET,
                              NULL, NULL, get_direct_rooms_cb,
                              g_object_ref (self));
    }
  else if (self->db_migrated && !self->room_list_loaded)
    {
      self->room_list_loading = TRUE;
      cm_net_send_json_async (self->cm_net, 0, NULL,
                              "/_matrix/client/r0/joined_rooms", SOUP_METHOD_GET,
                              NULL, NULL, get_joined_rooms_cb,
                              g_object_ref (self));

    }
  else if (!self->filter_id)
    {
      g_assert (self->cm_enc);
      g_assert (self->callback);
      client_set_login_state (self, TRUE, FALSE);
      matrix_upload_filter (self, g_steal_pointer (&task));
    }
  else
    {
      g_assert (self->cm_db);
      g_assert (self->callback);
      matrix_take_red_pill (self, g_steal_pointer (&task));
    }
}

/**
 * cm_client_can_connect:
 * @self: A #CmClient
 *
 * Check if @self can be connected to homeserver with current
 * network state.  This function is a bit dumb: returning
 * %TRUE shall not ensure that the @self is connectable.
 * But if %FALSE is returned, @self shall not be
 * able to connect.
 *
 * Returns: %TRUE if heuristics says @self can be connected.
 * %FALSE otherwise.
 */
gboolean
cm_client_can_connect (CmClient *self)
{
  GNetworkMonitor *nm;
  GInetAddress *inet;

  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);

  /* If never tried, assume we can connect */
  if (!self->has_tried_connecting)
    return TRUE;

  nm = g_network_monitor_get_default ();

  if (!self->gaddress || !G_IS_INET_SOCKET_ADDRESS (self->gaddress))
    goto end;

  inet = g_inet_socket_address_get_address ((GInetSocketAddress *)self->gaddress);

  if (g_inet_address_get_is_loopback (inet) ||
      g_inet_address_get_is_site_local (inet))
    return g_network_monitor_can_reach (nm, G_SOCKET_CONNECTABLE (self->gaddress), NULL, NULL);

 end:
  /* Distributions may advertise to have full network support event
   * when connected only to local network, so this isn't always right */
  return g_network_monitor_get_connectivity (nm) == G_NETWORK_CONNECTIVITY_FULL;
}

/**
 * cm_client_start_sync:
 * @self: A #CmClient
 *
 * Start sync with server.  If @self is already
 * in sync or in the process to do so, this method
 * simply returns.
 */
void
cm_client_start_sync (CmClient *self)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (self->callback);

  if (self->is_sync || self->is_logging_in)
    return;

  matrix_start_sync (self, NULL);
}

/**
 * cm_client_is_sync:
 * @self: A #CmClient
 *
 * Check whether the client @self is in
 * sync with the server or not.
 *
 * Returns: %TRUE if in sync with server.
 * %FALSE otherwise.
 */
gboolean
cm_client_is_sync (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);

  return cm_net_get_access_token (self->cm_net) &&
    self->login_success &&
    self->is_sync && !self->sync_failed;
}

/**
 * cm_client_stop_sync:
 * @self: A #CmClient
 *
 * Stop sync with server.
 */
void
cm_client_stop_sync (CmClient *self)
{
  g_return_if_fail (CM_IS_CLIENT (self));

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  self->is_sync = FALSE;
  self->sync_failed = FALSE;
  self->is_logging_in = FALSE;
  self->login_success = FALSE;

  g_clear_handle_id (&self->resync_id, g_source_remove);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
}

/**
 * cm_client_get_joined_rooms:
 * @self: A #CmClient
 *
 * Get the list of joined rooms with
 * #CmRoom as the members.
 *
 * Returns: (transfer none): A #GListModel
 */
GListModel *
cm_client_get_joined_rooms (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return G_LIST_MODEL (self->joined_rooms);
}

static void
keys_claim_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  JsonObject *object = NULL;
  GError *error = NULL;

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_debug ("Error key query: %s", error->message);
      g_task_return_error (task, error);
    }
  else
    {
      g_task_return_pointer (task, object, (GDestroyNotify)json_object_unref);
    }
}

/**
 * cm_client_claim_keys_async:
 * @self: A #CmClient
 * @member_list: A #GListModel of #ChattyMaBuddy
 * @callback: A #GAsyncReadyCallback
 * @user_data: user data passed to @callback
 *
 * Claim a key for all devices of @members_list
 */
void
cm_client_claim_keys_async (CmClient            *self,
                            GListModel          *member_list,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  JsonObject *object, *child;
  GTask *task;

  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (G_IS_LIST_MODEL (member_list));
  g_return_if_fail (g_list_model_get_item_type (member_list) == CM_TYPE_ROOM_MEMBER);
  g_return_if_fail (g_list_model_get_n_items (member_list) > 0);

  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-keys-claim */
  object = json_object_new ();
  json_object_set_int_member (object, "timeout", KEY_TIMEOUT);

  child = json_object_new ();

  for (guint i = 0; i < g_list_model_get_n_items (member_list); i++)
    {
      g_autoptr(CmRoomMember) member = NULL;
      JsonObject *key_json;

      member = g_list_model_get_item (member_list, i);
      key_json = cm_room_member_get_device_key_json (member);

      if (key_json)
        json_object_set_object_member (child,
                                       cm_room_member_get_user_id (member),
                                       key_json);
    }

  json_object_set_object_member (object, "one_time_keys", child);

  task = g_task_new (self, self->cancellable, callback, user_data);

  cm_net_send_json_async (self->cm_net, 0, object,
                          "/_matrix/client/r0/keys/claim", SOUP_METHOD_POST,
                          NULL, self->cancellable, keys_claim_cb, task);
}

JsonObject *
cm_client_claim_keys_finish (CmClient      *self,
                             GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
get_file_cb (GObject      *object,
             GAsyncResult *result,
             gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  GInputStream *istream = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  istream = cm_net_get_file_finish (self->cm_net, result, &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, istream, g_object_unref);
}

static void
find_file_enc_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  CmEncFileInfo *file_info;
  GCancellable *cancellable;
  char *uri;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  file_info = cm_enc_find_file_enc_finish (self->cm_enc, result, NULL);

  cancellable = g_task_get_cancellable (task);
  uri = g_task_get_task_data (task);

  cm_net_get_file_async (self->cm_net,
                         uri, file_info, cancellable,
                         get_file_cb,
                         g_steal_pointer (&task));
}

void
cm_client_get_file_async (CmClient              *self,
                          const char            *uri,
                          GCancellable          *cancellable,
                          GAsyncReadyCallback    callback,
                          gpointer               user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (uri && *uri);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (uri), g_free);

  cm_enc_find_file_enc_async (self->cm_enc, uri,
                              find_file_enc_cb, task);
}

GInputStream *
cm_client_get_file_finish (CmClient      *self,
                           GAsyncResult  *result,
                           GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
client_upload_group_keys_cb (GObject      *obj,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_debug ("Error uploading group keys: %s", error->message);
      g_task_return_error (task, error);
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }
}

void
cm_client_upload_group_keys_async (CmClient            *self,
                                   const char          *room_id,
                                   GListModel          *member_list,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_autofree char *uri = NULL;
  JsonObject *root, *object;
  GTask *task;

  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (G_IS_LIST_MODEL (member_list));
  g_return_if_fail (g_list_model_get_item_type (member_list) == CM_TYPE_ROOM_MEMBER);
  g_return_if_fail (g_list_model_get_n_items (member_list) > 0);

  root = json_object_new ();
  object = cm_enc_create_out_group_keys (self->cm_enc, room_id, member_list);
  json_object_set_object_member (root, "messages", object);

  task = g_task_new (self, self->cancellable, callback, user_data);

  self->event_id++;
  uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.room.encrypted/m%"G_GINT64_FORMAT".%d",
                         g_get_real_time () / G_TIME_SPAN_MILLISECOND,
                         self->event_id);
  cm_net_send_json_async (self->cm_net, 0, root, uri, SOUP_METHOD_PUT,
                          NULL, self->cancellable, client_upload_group_keys_cb, task);
}

gboolean
cm_client_upload_group_keys_finish (CmClient     *self,
                                    GAsyncResult  *result,
                                    GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
