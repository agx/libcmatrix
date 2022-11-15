/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-secret-store.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#define G_LOG_DOMAIN "cm-secret-store"

#include <libsecret/secret.h>
#include <glib/gi18n.h>

#include "cm-matrix-private.h"
#include "cm-utils-private.h"
#include "cm-secret-store-private.h"

#define PROTOCOL_MATRIX_STR  "matrix"

static const SecretSchema *
secret_store_get_schema (void)
{
  static SecretSchema *schema;
  static char *secret_id;

  if (schema)
    return schema;

  if (!secret_id)
    secret_id = g_strconcat (cm_matrix_get_app_id (), ".CMatrix", NULL);

  /** SECRET_SCHEMA_DONT_MATCH_NAME is used as a workaround for a bug in gnome-keyring
   *  which prevents cold keyrings from being searched (and hence does not prompt for unlocking)
   *  see https://gitlab.gnome.org/GNOME/gnome-keyring/-/issues/89 and
   *  https://gitlab.gnome.org/GNOME/libsecret/-/issues/7 for more information
   */
  schema = secret_schema_new (secret_id, SECRET_SCHEMA_DONT_MATCH_NAME,
                              CM_USERNAME_ATTRIBUTE, SECRET_SCHEMA_ATTRIBUTE_STRING,
                              CM_SERVER_ATTRIBUTE,   SECRET_SCHEMA_ATTRIBUTE_STRING,
                              CM_PROTOCOL_ATTRIBUTE, SECRET_SCHEMA_ATTRIBUTE_STRING,
                              NULL);
  return schema;
}

void
cm_secret_store_save_async (CmClient            *client,
                            char                *access_token,
                            char                *pickle_key,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  const SecretSchema *schema;
  g_autofree char *label = NULL;
  const char *server, *old_pass, *username, *device_id;
  char *password = NULL, *token = NULL, *key = NULL;
  CmAccount *account;
  char *credentials;

  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  old_pass = cm_client_get_password (client);

  if (old_pass && *old_pass)
    password = g_strescape (old_pass, NULL);
  if (access_token && *access_token)
    token = g_strescape (access_token, NULL);
  if (pickle_key && *pickle_key)
    key = g_strescape (pickle_key, NULL);

  account = cm_client_get_account (client);
  device_id = cm_client_get_device_id (client);
  username = cm_account_get_login_id (account);

  {
    g_autoptr(GString) str = NULL;

    str = g_string_new (NULL);
    cm_utils_anonymize (str, username);

    if (!access_token && pickle_key)
      g_critical ("'%s' user with device: %s, has no access key, but has pickle",
                  str->str, device_id);
  }

  if (!device_id)
    device_id = "";

  /* We don't use json APIs here so that we can manage memory better (and securely free them)  */
  /* TODO: Use a non-pageable memory */
  /* XXX: We use a dumb string search, so don't change the order or spacing of the format string */
  credentials = g_strdup_printf ("{\"username\": \"%s\",  \"password\": \"%s\","
                                 "\"access-token\": \"%s\", "
                                 "\"pickle-key\": \"%s\", \"device-id\": \"%s\", \"enabled\": \"%s\"}",
                                 cm_client_get_user_id (client) ?: "",
                                 password ? password : "", token ? token : "",
                                 key ? key : "", device_id,
                                 cm_client_get_enabled (client) ? "true" : "false");
  schema = secret_store_get_schema ();
  server = cm_client_get_homeserver (client);

  if (!server)
    {
      g_task_report_new_error (NULL, callback, user_data, NULL,
                               G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Homeserver required to store to db");
      return;
    }

  /* todo: translate the string */
  label = g_strdup_printf ("%s Matrix password for \"%s\"",
                           cm_matrix_get_app_id (), username);

  secret_password_store (schema, NULL, label, credentials,
                         cancellable, callback, user_data,
                         CM_USERNAME_ATTRIBUTE, username,
                         CM_SERVER_ATTRIBUTE, server,
                         CM_PROTOCOL_ATTRIBUTE, PROTOCOL_MATRIX_STR,
                         NULL);

  cm_utils_free_buffer (access_token);
  cm_utils_free_buffer (credentials);
  cm_utils_free_buffer (pickle_key);
  cm_utils_free_buffer (password);
  cm_utils_free_buffer (token);
  cm_utils_free_buffer (key);
}

gboolean
cm_secret_store_save_finish (GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);

  return secret_password_store_finish (result, error);
}

static void
secret_load_cb (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GPtrArray *accounts = NULL;
  GError *error = NULL;
  GList *secrets;

  g_assert_true (G_IS_TASK (task));

  secrets = secret_password_search_finish (result, &error);

  if (error) {
    g_task_return_error (task, error);
    return;
  }

  for (GList *item = secrets; item; item = item->next) {
    g_autofree char *label = NULL;
    g_autofree char *expected = NULL;

    label = secret_retrievable_get_label (item->data);
    expected = g_strconcat (cm_matrix_get_app_id (), " Matrix password", NULL);

    if (!label || !expected)
      continue;

    if (!g_str_has_prefix (label, expected))
      continue;

    if (!accounts)
      accounts = g_ptr_array_new_full (5, g_object_unref);

    if (item->data)
      g_ptr_array_add (accounts, g_object_ref (item->data));
  }

  if (secrets)
    g_list_free_full (secrets, g_object_unref);

  g_task_return_pointer (task, accounts, (GDestroyNotify)g_ptr_array_unref);
}

void
cm_secret_store_load_async (GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  const SecretSchema *schema;
  GTask *task;

  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  schema = secret_store_get_schema ();
  task = g_task_new (NULL, cancellable, callback, user_data);

  /** With using SECRET_SCHEMA_DONT_MATCH_NAME we need some other attribute
   *  (apart from the schema name itself) to use for the lookup.
   *  The protocol attribute seems like a reasonable choice.
   */
  secret_password_search (schema,
                          SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS,
                          cancellable, secret_load_cb, task,
                          CM_PROTOCOL_ATTRIBUTE, PROTOCOL_MATRIX_STR,
                          NULL);
}

GPtrArray *
cm_secret_store_load_finish (GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

void
cm_secret_store_delete_async (CmClient            *client,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  const SecretSchema *schema;
  const char *server, *username;
  CmAccount *account;

  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  account = cm_client_get_account (client);
  username = cm_account_get_login_id (account);

  schema = secret_store_get_schema ();
  server = cm_client_get_homeserver (client);
  secret_password_clear (schema, cancellable, callback, user_data,
                         CM_USERNAME_ATTRIBUTE, username,
                         CM_SERVER_ATTRIBUTE, server,
                         CM_PROTOCOL_ATTRIBUTE, PROTOCOL_MATRIX_STR,
                         NULL);
}

gboolean
cm_secret_store_delete_finish  (GAsyncResult  *result,
                                GError       **error)
{
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), FALSE);

  return secret_password_clear_finish (result, error);
}
