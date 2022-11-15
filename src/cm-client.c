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

#define G_LOG_DOMAIN "cm-client"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <libsecret/secret.h>
#include <libsoup/soup.h>

#include "cm-net-private.h"
#include "cm-utils-private.h"
#include "cm-common.h"
#include "cm-db-private.h"
#include "cm-olm-sas-private.h"
#include "cm-enc-private.h"
#include "cm-enums.h"
#include "events/cm-event-private.h"
#include "events/cm-verification-event.h"
#include "events/cm-verification-event-private.h"
#include "users/cm-room-member-private.h"
#include "users/cm-user-private.h"
#include "users/cm-user-list-private.h"
#include "users/cm-account.h"
#include "cm-room-private.h"
#include "cm-room.h"
#include "cm-secret-store-private.h"
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

  CmUserList     *user_list;
  /* direct_rooms are set on initial sync from 'account_data',
   * which will then be moved to joined_rooms later */
  GHashTable     *direct_rooms;
  GListStore     *joined_rooms;
  GListStore     *invited_rooms;

  GListStore     *key_verifications;

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
  gboolean        client_enabled_in_store;
  gboolean        has_tried_connecting;
  gboolean        is_logging_in;
  /* Set if passsword is right/success using @access_token */
  gboolean        login_success;
  gboolean        is_sync;
  gboolean        sync_failed;
  gboolean        is_self_change;
  gboolean        save_client_pending;
  gboolean        save_secret_pending;
  gboolean        is_saving_client;
  gboolean        is_saving_secret;
  gboolean        homeserver_verified;
};

G_DEFINE_TYPE (CmClient, cm_client, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_HOME_SERVER,
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

static CmEvent *
client_find_key_verification (CmClient            *self,
                              CmVerificationEvent *event,
                              gboolean             add_if_missing)
{
  CmOlmSas *olm_sas;
  GListModel *model;
  CmEventType type;

  g_assert (CM_IS_CLIENT (self));
  g_assert (CM_IS_VERIFICATION_EVENT (event));

  type = cm_event_get_m_type (CM_EVENT (event));
  model = G_LIST_MODEL (self->key_verifications);

  g_assert (type >= CM_M_KEY_VERIFICATION_ACCEPT && type <= CM_M_KEY_VERIFICATION_START);

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr(CmVerificationEvent) item = NULL;

      item = g_list_model_get_item (model, i);
      if (item == event)
        return CM_EVENT (event);

      olm_sas = g_object_get_data (G_OBJECT (item), "olm-sas");

      if (cm_olm_sas_matches_event (olm_sas, event))
        return CM_EVENT (item);
    }

  if (type != CM_M_KEY_VERIFICATION_START && type != CM_M_KEY_VERIFICATION_REQUEST)
    return NULL;

  olm_sas = cm_enc_get_sas_for_event (self->cm_enc, event);
  cm_olm_sas_set_client (olm_sas, self);

  if (add_if_missing)
    g_list_store_append (self->key_verifications, event);

  return CM_EVENT (event);
}

/*
 * client_mark_for_save:
 * @save_client: 1 for TRUE, 0 for FALSE, ignore otherwise
 * @save_secret: 1 for TRUE, 0 for FALSE, ignore otherwise
 */
static void
client_mark_for_save (CmClient *self,
                      int       save_client,
                      int       save_secret)
{
  g_assert (CM_IS_CLIENT (self));

  /* always reset to 0 when asked, as this is done after saving */
  if (save_client == 0)
    self->save_client_pending = save_client;
  if (save_secret == 0)
    self->save_secret_pending = save_secret;

  if (g_object_get_data (G_OBJECT (self), "no-save"))
    return;

  if (self->is_self_change)
    goto end;

  if (save_client == 1)
    self->save_client_pending = save_client;
  if (save_secret == 1)
    self->save_secret_pending = save_secret;

 end:
  cm_client_save (self);
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
  g_list_store_remove_all (self->invited_rooms);
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

  if (g_error_matches (error, CM_ERROR, CM_ERROR_UNKNOWN_TOKEN) &&
      self->password)
    {
      g_debug ("(%p) Handle glitch, unknown token", self);

      client_reset_state (self);
      cm_db_delete_client_async (self->cm_db, self, NULL, NULL);
      matrix_start_sync (self, NULL);

      return TRUE;
    }

  /*
   * The G_RESOLVER_ERROR may be suggesting that the hostname is wrong, but we don't
   * know if it's network/DNS/Proxy error. So keep retrying.
   */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
#if SOUP_MAJOR_VERSION == 2
      (error->domain == SOUP_HTTP_ERROR &&
       error->code <= SOUP_STATUS_TLS_FAILED &&
       error->code > SOUP_STATUS_CANCELLED) ||
#else
      error->domain == SOUP_TLD_ERROR ||
      error->domain == G_TLS_ERROR ||
#endif
      /* Should we handle connection_refused, or just keep it for localhost? */
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
      error->domain == G_RESOLVER_ERROR ||
      error->domain == JSON_PARSER_ERROR)
    {
      self->sync_failed = TRUE;
      g_signal_emit (self, signals[STATUS_CHANGED], 0);

      if (cm_client_can_connect (self))
        {
          CM_TRACE ("(%p) Handle glitch, network error", self);
          g_clear_handle_id (&self->resync_id, g_source_remove);

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
  g_autoptr(GString) str = NULL;
  JsonObject *object, *child;
  GTask *task;

  g_assert (CM_IS_CLIENT (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_assert (cm_account_get_login_id (self->cm_account) ||
            cm_user_get_id (CM_USER (self->cm_account)));
  g_assert (self->homeserver_verified);
  g_assert (self->password && *self->password);

  str = g_string_new (NULL);
  g_debug ("(%p) Logging in with '%s'", self,
           cm_utils_anonymize (str, cm_account_get_login_id (self->cm_account)));

  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-login */
  object = json_object_new ();
  json_object_set_string_member (object, "type", "m.login.password");
  json_object_set_string_member (object, "password", self->password);
  json_object_set_string_member (object, "initial_device_display_name", self->device_name ?: "CMatrix");

  child = json_object_new ();

  if (cm_utils_user_name_is_email (cm_account_get_login_id (self->cm_account)))
    {
      json_object_set_string_member (child, "type", "m.id.thirdparty");
      json_object_set_string_member (child, "medium", "email");
      json_object_set_string_member (child, "address", cm_account_get_login_id (self->cm_account));
    }
  else
    {
      json_object_set_string_member (child, "type", "m.id.user");
      json_object_set_string_member (child, "user", cm_account_get_login_id (self->cm_account));
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
  g_debug ("(%p) Upload filter %s", self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      if (!handle_matrix_glitches (self, error))
        g_warning ("Error uploading filter: %s", error->message);

      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, NULL, NULL);
  self->filter_id = g_strdup (cm_utils_json_object_get_string (root, "filter_id"));
  g_debug ("(%p) Upload filter, id: %s", self, self->filter_id);

  client_set_login_state (self, FALSE, TRUE);

  if (!self->filter_id)
    self->filter_id = g_strdup ("");

  client_mark_for_save (self, TRUE, -1);
  cm_client_save (self);

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

  g_debug ("(%p) Upload filter", self);

  parser = json_parser_new ();
  json_parser_load_from_data (parser, filter_json_str, -1, &error);

  if (error)
    g_warning ("(%p) Error parsing filter file: %s", self, error->message);

  if (!error)
    root = json_parser_get_root (parser);

  if (root)
    filter = json_node_get_object (root);

  if (error || !root || !filter)
    {
      g_warning ("(%p) Error getting filter file: %s", self, error ? error->message : "");

      self->filter_id = g_strdup ("");
      /* Even if we have error uploading filter, continue syncing */
      matrix_start_sync (self, NULL);

      return;
    }

  cancellable = g_task_get_cancellable (task);
  uri = g_strconcat ("/_matrix/client/r0/user/", cm_user_get_id (CM_USER (self->cm_account)), "/filter", NULL);
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

  g_list_store_remove_all (self->key_verifications);
  g_clear_object (&self->key_verifications);

  g_hash_table_unref (self->direct_rooms);

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
  self->cm_account = g_object_new (CM_TYPE_ACCOUNT, NULL);
  cm_user_set_client (CM_USER (self->cm_account), self);

  self->cm_net = cm_net_new ();
  self->user_list = cm_user_list_new (self);
  self->cancellable = g_cancellable_new ();
  self->joined_rooms = g_list_store_new (CM_TYPE_ROOM);
  self->invited_rooms = g_list_store_new (CM_TYPE_ROOM);
  self->key_verifications = g_list_store_new (CM_TYPE_VERIFICATION_EVENT);
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

static char *
client_get_value (const char *str,
                  const char *key)
{
  const char *start, *end;

  if (!str || !*str)
    return NULL;

  g_assert (key && *key);

  start = strstr (str, key);
  if (start) {
    start = start + strlen (key);
    while (*start && *start++ != '"')
      ;

    end = start - 1;
    do {
      end++;
      end = strchr (end, '"');
    } while (end && *(end - 1) == '\\' && *(end - 2) != '\\');

    if (end && end > start)
      return g_strndup (start, end - start);
  }

  return NULL;
}

void
cm_client_enable_as_in_store (CmClient *self)
{
  g_return_if_fail (CM_IS_CLIENT (self));

  self->is_self_change = TRUE;

  if (self->client_enabled_in_store)
    cm_client_set_enabled (self, TRUE);

  self->client_enabled_in_store = FALSE;
  self->is_self_change = FALSE;
}

CmClient *
cm_client_new_from_secret (gpointer  secret_retrievable,
                           CmDb     *db)
{
  CmClient *self = NULL;
  g_autoptr(GHashTable) attributes = NULL;
  SecretRetrievable *item = secret_retrievable;
  g_autoptr(SecretValue) value = NULL;
  const char *homeserver, *credentials = NULL;
  const char *username, *login_username;
  char *password, *token, *device_id;
  char *password_str = NULL, *token_str = NULL;
  g_autofree char *enabled = NULL;

  g_return_val_if_fail (SECRET_IS_RETRIEVABLE (item), NULL);
  g_return_val_if_fail (CM_IS_DB (db), NULL);

  value = secret_retrievable_retrieve_secret_sync (item, NULL, NULL);

  if (value)
    credentials = secret_value_get_text (value);

  if (!credentials)
    return NULL;

  attributes = secret_retrievable_get_attributes (item);
  login_username = g_hash_table_lookup (attributes, CM_USERNAME_ATTRIBUTE);
  homeserver = g_hash_table_lookup (attributes, CM_SERVER_ATTRIBUTE);

  device_id = client_get_value (credentials, "\"device-id\"");
  username = client_get_value (credentials, "\"username\"");
  password = client_get_value (credentials, "\"password\"");
  enabled = client_get_value (credentials, "\"enabled\"");
  token = client_get_value (credentials, "\"access-token\"");

  if (token)
    token_str = g_strcompress (token);

  if (password)
    password_str = g_strcompress (password);

  self = g_object_new (CM_TYPE_CLIENT, NULL);
  self->is_self_change = TRUE;
  cm_client_set_db (self, db);
  cm_client_set_homeserver (self, homeserver);
  cm_account_set_login_id (self->cm_account, login_username);
  cm_client_set_user_id (self, username);
  cm_client_set_password (self, password_str);
  cm_client_set_device_id (self, device_id);

  if (g_strcmp0 (enabled, "true") == 0)
    self->client_enabled_in_store = TRUE;

  cm_client_set_access_token (self, token_str);

  if (token && device_id) {
    g_autofree char *pickle = NULL;

    pickle = client_get_value (credentials, "\"pickle-key\"");
    cm_client_set_pickle_key (self, pickle);
  }

  self->is_self_change = FALSE;

  cm_utils_free_buffer (device_id);
  cm_utils_free_buffer (password);
  cm_utils_free_buffer (password_str);
  cm_utils_free_buffer (token);
  cm_utils_free_buffer (token_str);

  return self;
}
static void
save_secrets_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  gboolean ret;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  ret = cm_secret_store_save_finish (result, &error);

  if (error)
    {
      client_mark_for_save (self, -1, TRUE);
      g_task_return_error (task, error);
    }
  else
    {
      g_task_return_boolean (task, ret);
    }

  /* Unset at the end so that the client won't initiate
   * saving the secret immediately on error.
   */
  self->is_saving_secret = FALSE;
}

void
cm_client_save_secrets_async (CmClient            *self,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  char *pickle_key = NULL;

  g_return_if_fail (CM_IS_CLIENT (self));

  task = g_task_new (self, NULL, callback, user_data);

  if (g_object_get_data (G_OBJECT (self), "no-save"))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Secrets marked not to save");
      return;
    }

  if (self->is_saving_secret)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "Secrets are already being saved");
      return;
    }

  self->is_saving_secret = TRUE;
  client_mark_for_save (self, -1, FALSE);

  if (self->cm_enc)
    pickle_key = cm_enc_get_pickle_key (self->cm_enc);

  cm_secret_store_save_async (self,
                              g_strdup (cm_client_get_access_token (self)),
                              pickle_key, NULL,
                              save_secrets_cb,
                              g_steal_pointer (&task));
}

gboolean
cm_client_save_secrets_finish (CmClient      *self,
                               GAsyncResult  *result,
                               GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
delete_secrets_cb (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  CmClient *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  gboolean ret;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  ret = cm_secret_store_delete_finish (result, &error);

  if (error)
    {
      g_task_return_error (task, error);
    }
  else
    {
      g_list_store_remove_all (self->joined_rooms);
      g_task_return_boolean (task, ret);
    }
}

void
cm_client_delete_secrets_async (CmClient            *self,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_CLIENT (self));

  task = g_task_new (self, NULL, callback, user_data);
  cm_client_set_enabled (self, FALSE);
  cm_secret_store_delete_async (self, NULL,
                                delete_secrets_cb, task);
}

gboolean
cm_client_delete_secrets_finish (CmClient      *self,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

CmAccount *
cm_client_get_account (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

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
  guint room_count = 0;
  gboolean success;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  g_assert (!self->cm_enc);

  self->db_loaded = TRUE;
  self->db_loading = FALSE;

  success = cm_db_load_client_finish (self->cm_db, result, &error);
  g_debug ("(%p) Load db %s", self,
           CM_LOG_SUCCESS (!error || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)));

  if (!success)
    {
      if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_autoptr(GString) str = NULL;

          str = g_string_new (NULL);
          g_warning ("(%p) Error loading client '%s': %s", self,
                     cm_utils_anonymize (str, cm_user_get_id (CM_USER (self->cm_account))),
                     error->message);
        }

      /* We can load further even if fail to load from db */
      /* XXX: handle difference between if the user is missing from db and failed fetching data */
      matrix_start_sync (self, g_steal_pointer (&task));
      return;
    }

  if ((g_object_get_data (G_OBJECT (result), "pickle") && !self->pickle_key) ||
      (!g_object_get_data (G_OBJECT (result), "pickle") && self->pickle_key))
    {
      g_autoptr(GString) str = NULL;
      gboolean has_pickle, has_pickle_key;

      has_pickle_key = !!self->pickle_key;
      has_pickle = !!g_object_get_data (G_OBJECT (result), "pickle");

      str = g_string_new (NULL);
      cm_utils_anonymize (str, cm_client_get_user_id (self));

      g_critical ("'%s' Missing secrets, has-pickle: %d, has-pickle-key: %d",
                  str->str, has_pickle, has_pickle_key);
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
      room_count = rooms->len;

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
  g_debug ("(%p) Load db, added %u room(s), db migrated: %s, filter-id: %s",
           self, room_count, CM_LOG_BOOL (self->db_migrated), self->filter_id);

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

  g_debug ("(%p) Set enable to %s", self, CM_LOG_BOOL (enable));

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

  client_mark_for_save (self, TRUE, TRUE);
  cm_client_save (self);
}

static void
db_save_cb (GObject      *object,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
  g_autoptr(GError) error = NULL;
  gboolean status;

  status = cm_db_save_client_finish (self->cm_db, result, &error);
  self->is_saving_client = FALSE;

  if (error || !status)
    self->save_client_pending = TRUE;

  if (error)
    g_warning ("Error saving to db: %s", error->message);

  /* If settings changed when we were saving the current settings, repeat. */
  cm_client_save (self);
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
cm_client_save (CmClient *self)
{
  g_return_if_fail (CM_IS_CLIENT (self));

  if (g_object_get_data (G_OBJECT (self), "no-save"))
    return;

  if (!cm_account_get_login_id (self->cm_account) &&
      !cm_user_get_id (CM_USER (self->cm_account)))
    return;

  if (self->save_client_pending && !self->is_saving_client &&
      cm_client_get_device_id (self))
    {
      char *pickle = NULL;

      self->is_saving_client = TRUE;
      self->save_client_pending = FALSE;

      if (self->cm_enc)
        pickle = cm_enc_get_pickle (self->cm_enc);

      cm_db_save_client_async (self->cm_db, self, pickle,
                               db_save_cb,
                               g_object_ref (self));
    }

  if (self->save_secret_pending && !self->is_saving_secret)
    cm_client_save_secrets_async (self, NULL, NULL);
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

  return self->client_enabled || self->client_enabled_in_store ||
    GPOINTER_TO_INT (g_object_get_data (G_OBJECT (self), "enable"));
}

/**
 * cm_client_set_sync_callback:
 * @self: A #CmClient
 * @callback: A #CmCallback
 * @callback_data: A #GObject derived object for @callback user_data
 * @callback_data_destroy: (nullable): The method to destroy @callback_data
 *
 * Set the sync callback which shall be executed for the
 * events happening in @self.
 *
 * @callback_data_destroy() shall be executead only if @callback_data
 * is not NULL.
 */
void
cm_client_set_sync_callback (CmClient       *self,
                             CmCallback      callback,
                             gpointer        callback_data,
                             GDestroyNotify  callback_data_destroy)
{
  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (callback);

  if (self->cb_data &&
      self->cb_destroy)
    self->cb_destroy (self->cb_data);

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
  g_autoptr(GString) str = NULL;
  GRefString *new_user_id = NULL;
  g_autofree char *user_id = NULL;

  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (!self->is_logging_in, FALSE);
  g_return_val_if_fail (!self->login_success, FALSE);

  if (!cm_utils_user_name_valid (matrix_user_id))
    {
      g_debug ("(%p) New user ID: '%s' %s. ID not valid", self,
               matrix_user_id, CM_LOG_SUCCESS (FALSE));
      return FALSE;
    }

  if (cm_user_get_id (CM_USER (self->cm_account)))
    {
      g_debug ("(%p) New user ID not set, a user id is already set", self);
      return FALSE;
    }

  user_id = g_ascii_strdown (matrix_user_id, -1);
  new_user_id = g_ref_string_new_intern (user_id);

  cm_user_set_user_id (CM_USER (self->cm_account), new_user_id);
  cm_user_list_set_account (self->user_list, self->cm_account);

  str = g_string_new (NULL);
  g_debug ("(%p) New user ID set: '%s'", self,
           cm_utils_anonymize (str, matrix_user_id));

  client_mark_for_save (self, TRUE, TRUE);

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
 * Returns: (nullable) (transfer none): The matrix user ID of the client
 */
GRefString *
cm_client_get_user_id (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return cm_user_get_id (CM_USER (self->cm_account));
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
#if SOUP_MAJOR_VERSION == 2
  g_autoptr(SoupURI) uri = NULL;
#else
  g_autoptr(GUri) uri = NULL;
#endif
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

  if (g_strcmp0 (self->homeserver, server->str) == 0)
    {
      g_string_free (server, TRUE);
      return TRUE;
    }

  g_free (self->homeserver);
  self->homeserver = g_string_free (server, FALSE);
  cm_net_set_homeserver (self->cm_net, homeserver);
  client_mark_for_save (self, TRUE, TRUE);

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
  g_return_if_fail (!self->is_sync);

  g_clear_pointer (&self->password, gcry_free);

  if (password && *password)
    {
      self->password = gcry_malloc_secure (strlen (password) + 1);
      strcpy (self->password, password);
    }

  client_mark_for_save (self, -1, TRUE);

  if (self->has_tried_connecting &&
      cm_client_get_enabled (self))
    {
      cm_client_stop_sync (self);
      cm_client_start_sync (self);
    }
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

CmUserList *
cm_client_get_user_list (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return self->user_list;
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

static void
client_join_room_by_id_cb (GObject      *obj,
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
cm_client_join_room_by_id_async (CmClient            *self,
                                 const char          *room_id,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_autofree char *uri = NULL;
  GTask *task;

  g_return_if_fail (CM_IS_CLIENT (self));
  g_return_if_fail (room_id && *room_id == '!');
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  if (!cancellable)
    cancellable = self->cancellable;

  task = g_task_new (self, cancellable, callback, user_data);
  uri = g_strconcat ("/_matrix/client/r0/join/", room_id, NULL);
  cm_net_send_data_async (self->cm_net, 2, NULL, 0,
                          uri, SOUP_METHOD_POST, NULL,
                          cancellable,
                          client_join_room_by_id_cb,
                          task);
}

gboolean
cm_client_join_room_by_id_finish (CmClient      *self,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_client_get_homeserver_async (CmClient            *self,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  const char *user_id;

  g_return_if_fail (CM_IS_CLIENT (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, cm_client_get_homeserver_async);
  g_debug ("(%p) Get homeserver", self);

  if (self->homeserver_verified && self->homeserver && *(self->homeserver))
    {
      g_debug ("(%p) Get homeserver already loaded", self);
      g_task_return_pointer (task, self->homeserver, NULL);
      return;
    }

  user_id = cm_user_get_id (CM_USER (self->cm_account));

  if (!user_id)
    user_id = cm_account_get_login_id (self->cm_account);

  if (!cm_utils_user_name_valid (user_id))
    user_id = NULL;

  if (!user_id && !self->homeserver)
    {
      g_debug ("(%p) Get homeserver failed, no user id to guess", self);
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

  g_debug ("(%p) Verify home server %s", self, CM_LOG_SUCCESS (!error));

  /* Since GTask can't have timeout, We cancel the cancellable to fake timeout */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    {
      g_clear_object (&self->cancellable);
      self->cancellable = g_cancellable_new ();
    }

  g_clear_object (&self->gaddress);
  self->gaddress = g_object_steal_data (G_OBJECT (result), "address");

  self->has_tried_connecting = TRUE;

  if (g_task_get_source_tag (task) != cm_client_get_homeserver_async &&
      handle_matrix_glitches (self, error))
    return;

  if (self->homeserver_verified)
    {
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
        self->callback (self->cb_data, self, NULL, NULL, error);
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
  g_debug ("(%p) Login with password %s", self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_autoptr(GString) str = NULL;
      self->sync_failed = TRUE;

      str = g_string_new (NULL);
      g_debug ("(%p) Login %s, username: %s, error: %s", self, CM_LOG_SUCCESS (FALSE),
               cm_utils_anonymize (str, cm_account_get_login_id (self->cm_account)),
               error->message);
      if (error->code == CM_ERROR_FORBIDDEN)
        error->code = CM_ERROR_BAD_PASSWORD;

      client_set_login_state (self, FALSE, FALSE);

      if (!handle_matrix_glitches (self, error) && self->callback)
        self->callback (self->cb_data, self, NULL, NULL, error);

      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* https://matrix.org/docs/spec/client_server/r0.6.1#post-matrix-client-r0-login */
  value = cm_utils_json_object_get_string (root, "user_id");
  self->is_logging_in = FALSE;
  cm_client_set_user_id (self, value);
  self->is_logging_in = TRUE;

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
  client_mark_for_save (self, TRUE, TRUE);
  cm_client_save (self);

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
                  const char *room_id,
                  GListStore *rooms)
{
  guint n_items;

  g_assert (CM_IS_CLIENT (self));
  g_assert (room_id && *room_id);

  n_items = g_list_model_get_n_items (G_LIST_MODEL (rooms));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmRoom) room = NULL;

      room = g_list_model_get_item (G_LIST_MODEL (rooms), i);
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
          g_debug ("(%p) Generating %" G_GSIZE_FORMAT " onetime keys", self, limit - count);
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
  g_debug ("(%p) Upload key %s", self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      self->sync_failed = TRUE;
      handle_matrix_glitches (self, error);
      g_debug ("Error uploading key: %s", error->message);
      return;
    }

  json_str = cm_utils_json_object_to_string (root, FALSE);
  cm_enc_publish_one_time_keys (self->cm_enc);

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

  g_debug ("(%p) Upload key", self);
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
            room = client_find_room (self, room_id, self->joined_rooms);

          if (room)
            {
              cm_room_set_generated_name (room, user_id->data);
              cm_room_set_is_direct (room, TRUE);

              continue;
            }

          room = cm_room_new (room_id);
          cm_room_set_status (room, CM_STATUS_JOIN);
          cm_room_set_client (room, self);
          cm_room_set_is_direct (room, TRUE);
          cm_room_set_generated_name (room, user_id->data);

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
verification_send_key_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(CmClient) self = user_data;
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
      g_autoptr(GPtrArray) events = NULL;
      g_autoptr(CmEvent) event = NULL;
      GRefString *user_id;
      CmUser *user = NULL;
      CmEventType type;

      object = json_array_get_object_element (array, i);

      event = cm_event_new_from_json (object, NULL);
      user_id = cm_event_get_sender_id (event);
      if (user_id)
        {
          user = cm_user_list_find_user (self->user_list, user_id, TRUE);
          cm_event_set_sender (event, user);
        }

      type = cm_event_get_m_type (event);
      events = g_ptr_array_new ();

      if (type == CM_M_ROOM_ENCRYPTED)
        {
          cm_enc_handle_room_encrypted (self->cm_enc, object);
        }
      else if (type >= CM_M_KEY_VERIFICATION_ACCEPT &&
               type <= CM_M_KEY_VERIFICATION_START)
        {
          g_autoptr(CmVerificationEvent) verification_event = NULL;
          CmEvent *key_event;
          CmOlmSas *olm_sas;

          verification_event = cm_verification_event_new (self);
          cm_verification_event_set_json (verification_event, object);

          /* Don't force add the event now. If the event has to be cancelled
           * for any reason cancel it now and don't even report to the user */
          key_event = client_find_key_verification (self, verification_event, FALSE);
          if (!key_event)
            return;

          if (user && !cm_event_get_sender (key_event))
            cm_event_set_sender (key_event, user);

          olm_sas = g_object_get_data (G_OBJECT (key_event), "olm-sas");

          if (cm_olm_sas_get_cancel_code (olm_sas))
            {
              cm_verification_event_cancel_async (CM_VERIFICATION_EVENT (key_event), NULL, NULL, NULL);
              return;
            }

          /* Now add the event if not already done so */
          key_event = client_find_key_verification (self, verification_event, TRUE);

          if (user && !cm_event_get_sender (key_event))
            cm_event_set_sender (key_event, user);

          if (key_event && type == CM_M_KEY_VERIFICATION_KEY)
            {
              /* The start/request event (which is `key_event`) shall not be the same as key event */
              if (key_event != event)
                {
                  g_autofree char *uri = NULL;
                  CmEvent *reply_event;
                  JsonObject *obj;

                  olm_sas = g_object_get_data (G_OBJECT (key_event), "olm-sas");
                  reply_event = cm_olm_sas_get_key_event (olm_sas);

                  obj = cm_event_get_json (reply_event);
                  uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.key/%s",
                                         cm_event_get_txn_id (reply_event));
                  cm_net_send_json_async (self->cm_net, 0, obj, uri, SOUP_METHOD_PUT,
                                          NULL, NULL,
                                          verification_send_key_cb,
                                          g_object_ref (self));
                }
            }

          if (g_object_get_data (G_OBJECT (key_event), "mac") &&
              g_object_get_data (G_OBJECT (key_event), "mac-sent")) {
            CmDevice *device;

            device = cm_olm_sas_get_device (olm_sas);
            cm_db_update_device (self->cm_db, self, cm_event_get_sender (key_event), device);
            cm_verification_event_done_async (CM_VERIFICATION_EVENT (key_event), NULL, NULL, NULL);
          }
        }
    }
}

static void
handle_room_join (CmClient   *self,
                  JsonObject *root)
{
  g_autoptr(GList) joined_room_ids = NULL;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  joined_room_ids = json_object_get_members (root);

  for (GList *room_id = joined_room_ids; room_id; room_id = room_id->next)
    {
      g_autoptr(GPtrArray) events = NULL;
      CmRoom *room;
      JsonObject *room_data;

      room = client_find_room (self, room_id->data, self->joined_rooms);
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
              cm_room_set_status (room, CM_STATUS_JOIN);
              cm_room_set_client (room, self);
              g_list_store_append (self->joined_rooms, room);
              g_object_unref (room);
            }
        }

      cm_room_set_status (room, CM_STATUS_JOIN);
      events = cm_room_set_data (room, room_data);
      cm_db_add_room_events (self->cm_db, room, events, FALSE);

      if (self->callback)
        self->callback (self->cb_data, self, room, events, NULL);

      cm_utils_remove_list_item (self->invited_rooms, room);

      if (cm_room_get_replacement_room (room))
        cm_utils_remove_list_item (self->joined_rooms, room);
    }
}

static void
handle_room_leave (CmClient   *self,
                   JsonObject *root)
{
  g_autoptr(GList) left_room_ids = NULL;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  left_room_ids = json_object_get_members (root);

  for (GList *room_id = left_room_ids; room_id; room_id = room_id->next)
    {
      g_autoptr(GPtrArray) events = NULL;
      CmRoom *room;
      JsonObject *room_data;

      room = client_find_room (self, room_id->data, self->joined_rooms);
      room_data = cm_utils_json_object_get_object (root, room_id->data);

      if (!room)
        continue;

      events = cm_room_set_data (room, room_data);
      cm_room_set_status (room, CM_STATUS_LEAVE);
      cm_db_add_room_events (self->cm_db, room, events, FALSE);

      if (self->callback)
        self->callback (self->cb_data, self, room, events, NULL);

      cm_utils_remove_list_item (self->joined_rooms, room);
    }
}

static void
handle_room_invite (CmClient   *self,
                    JsonObject *root)
{
  g_autoptr(GList) invited_room_ids = NULL;

  g_assert (CM_IS_CLIENT (self));

  if (!root)
    return;

  invited_room_ids = json_object_get_members (root);

  for (GList *room_id = invited_room_ids; room_id; room_id = room_id->next)
    {
      g_autoptr(GPtrArray) events = NULL;
      CmRoom *room;
      JsonObject *room_data;

      room = client_find_room (self, room_id->data, self->invited_rooms);
      room_data = cm_utils_json_object_get_object (root, room_id->data);

      if (!room)
        {
          room = cm_room_new (room_id->data);
          cm_room_set_status (room, CM_STATUS_INVITE);
          cm_room_set_client (room, self);
          g_list_store_append (self->joined_rooms, room);
          g_object_unref (room);
        }

      events = cm_room_set_data (room, room_data);

      if (events && events->len)
        cm_db_add_room_events (self->cm_db, room, events, FALSE);

      if (self->callback)
        self->callback (self->cb_data, self, room, events, NULL);
    }
}

static void
handle_device_list (CmClient   *self,
                    JsonObject *root)
{
  g_autoptr(GPtrArray) users = NULL;
  guint n_items;

  if (!root)
    return;

  users = g_ptr_array_new_with_free_func (g_object_unref);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->joined_rooms));
  cm_user_list_device_changed (self->user_list, root, users);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmRoom) room = NULL;

      room = g_list_model_get_item (G_LIST_MODEL (self->joined_rooms), i);
      cm_room_user_changed (room, users);
    }

  cm_db_mark_user_device_change (self->cm_db, self, users, TRUE, TRUE);
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
  handle_device_list (self, cm_utils_json_object_get_object (root, "device_lists"));
  /* to_device should be handled first as it might contain keys to be used
   * to decrypt following events */
  handle_to_device (self, cm_utils_json_object_get_object (root, "to_device"));

  object = cm_utils_json_object_get_object (root, "rooms");
  handle_room_join (self, cm_utils_json_object_get_object (object, "join"));
  handle_room_leave (self, cm_utils_json_object_get_object (object, "leave"));
  handle_room_invite (self, cm_utils_json_object_get_object (object, "invite"));
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
        self->callback (self->cb_data, self, NULL, NULL, error);
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Error syncing with time %s: %s", self->next_batch, error->message);
      return;
    }

  client_set_login_state (self, FALSE, TRUE);

  g_free (self->next_batch);
  self->next_batch = g_strdup (cm_utils_json_object_get_string (root, "next_batch"));
  client_mark_for_save (self, TRUE, -1);
  cm_client_save (self);

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
  g_autofree char *homeserver = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (self));

  homeserver = cm_utils_get_homeserver_finish (result, &error);
  g_object_set_data (G_OBJECT (task), "action", "get-homeserver");
  g_debug ("(%p) Get home server %s", self, CM_LOG_SUCCESS (!error));

  client_set_login_state (self, FALSE, FALSE);

  if (error)
    {
      self->sync_failed = TRUE;

      g_debug ("(%p) Get home server error: %s", self, error->message);
      if (g_task_get_source_tag (task) != cm_client_get_homeserver_async)
        handle_matrix_glitches (self, error);
      g_task_return_error (task, error);

      return;
    }

  g_debug ("(%p) Got home server: %s", self, homeserver);
  if (!homeserver)
    {
      self->sync_failed = TRUE;
      g_task_return_new_error (task, CM_ERROR, CM_ERROR_NO_HOME_SERVER,
                               "Couldn't fetch homeserver");
      if (self->callback)
        self->callback (self->cb_data, self, NULL, NULL, error);

      return;
    }

  cm_client_set_homeserver (self, homeserver);

  if (!self->homeserver)
    {
      self->sync_failed = TRUE;
      g_debug ("(%p) Get home server: '%s' is invalid uri", self, homeserver);
      g_task_return_new_error (task, CM_ERROR, CM_ERROR_BAD_HOME_SERVER,
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

  g_debug ("(%p) Get joined rooms %s", self, CM_LOG_SUCCESS (!error));

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

    g_debug ("(%p) Get joined rooms, count: %u", self, length);

    for (guint i = 0; i < length; i++)
      {
        const char *room_id;
        CmRoom *room;

        room_id = json_array_get_string_element (array, i);
        room = client_find_room (self, room_id, self->joined_rooms);

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
            cm_room_set_status (room, CM_STATUS_JOIN);
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
  g_debug ("(%p) Get direct rooms %s", self, CM_LOG_SUCCESS (!error));

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
      g_debug ("(%p) Load db", self);
      cm_db_load_client_async (self->cm_db, self,
                               cm_client_get_device_id (self),
                               db_load_client_cb,
                               g_steal_pointer (&task));
    }
  else if (!self->homeserver)
    {
      const char *user_id;

      user_id = cm_user_get_id (CM_USER (self->cm_account));

      if (!user_id)
        user_id = cm_account_get_login_id (self->cm_account);

      if (!cm_utils_home_server_valid (self->homeserver) &&
          !cm_utils_user_name_valid (user_id))
        {
          g_warning ("(%p) Error: No Homeserver provided", self);

          g_task_return_new_error (task, CM_ERROR, CM_ERROR_NO_HOME_SERVER,
                                   "No Homeserver provided");
          return;
        }

      client_set_login_state (self, TRUE, FALSE);
      g_debug ("(%p) Getting homeserver", self);
      cm_utils_get_homeserver_async (user_id, 30, cancellable,
                                     client_get_homeserver_cb,
                                     g_steal_pointer (&task));
    }
  else if (!self->homeserver_verified)
    {
      client_set_login_state (self, TRUE, FALSE);
      g_debug ("(%p) Verify homeserver '%s'", self, self->homeserver);
      cm_utils_verify_homeserver_async (self->homeserver, 30, cancellable,
                                        client_verify_homeserver_cb,
                                        g_steal_pointer (&task));
    }
  else if (!self->password && !cm_net_get_access_token (self->cm_net))
    {
      GError *error;

      g_warning ("(%p) No password provided, nor access token", self);

      error = g_error_new (CM_ERROR, CM_ERROR_BAD_PASSWORD, "No Password provided");

      if (self->callback)
        self->callback (self->cb_data, self, NULL, NULL, error);

      g_task_return_error (task, error);
    }
  else if (!cm_net_get_access_token (self->cm_net) || !self->cm_enc)
    {
      g_assert (self->cm_db);
      cm_net_set_access_token (self->cm_net, NULL);
      client_set_login_state (self, TRUE, FALSE);
      g_debug ("(%p) Login with password", self);
      client_login_with_password_async (self, cancellable,
                                        client_password_login_cb,
                                        g_steal_pointer (&task));
    }
  else if (self->db_migrated && !self->direct_room_list_loaded)
    {
      g_autofree char *uri = NULL;

      self->direct_room_list_loading = TRUE;

      uri = g_strconcat ("/_matrix/client/r0/user/",
                         cm_user_get_id (CM_USER (self->cm_account)),
                         "/account_data/m.direct", NULL);
      g_debug ("(%p) Get direct rooms", self);
      cm_net_send_json_async (self->cm_net, 0, NULL, uri, SOUP_METHOD_GET,
                              NULL, NULL, get_direct_rooms_cb,
                              g_object_ref (self));
    }
  else if (self->db_migrated && !self->room_list_loaded)
    {
      self->room_list_loading = TRUE;
      g_debug ("(%p) Get joined rooms", self);
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

  g_debug ("(%p) Start sync", self);
  g_clear_handle_id (&self->resync_id, g_source_remove);
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
  g_debug ("(%p) Stop sync", self);

  g_signal_emit (self, signals[STATUS_CHANGED], 0);
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

/**
 * cm_client_get_invited_rooms:
 * @self: A #CmClient
 *
 * Get the list of invited rooms with
 * #CmRoom as the members.
 *
 * Returns: (transfer none): A #GListModel
 */
GListModel *
cm_client_get_invited_rooms (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return G_LIST_MODEL (self->invited_rooms);
}

GListModel *
cm_client_get_key_verifications (CmClient *self)
{
  g_return_val_if_fail (CM_IS_CLIENT (self), NULL);

  return G_LIST_MODEL (self->key_verifications);
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
                          GFileProgressCallback  progress_callback,
                          gpointer               progress_user_data,
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
