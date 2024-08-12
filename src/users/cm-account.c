/* cm-account.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-account"

#include "cm-config.h"

#include <libsoup/soup.h>

#include "cm-client-private.h"
#include "cm-net-private.h"
#include "cm-utils-private.h"
#include "cm-user-private.h"
#include "cm-user.h"
#include "cm-account.h"

/**
 * SECTION: cm-account
 * @title: CmAccount
 * @short_description: A high-level API to manage the client owner’s account.
 * @include: "cm-account.h"
 */

struct _CmAccount
{
  CmUser        parent_instance;

  /* @login_username can be email/[incomplete] matrix-id/phone-number etc */
  char         *login_id;
};

G_DEFINE_TYPE (CmAccount, cm_account, CM_TYPE_USER)

static void
cm_account_finalize (GObject *object)
{
  CmAccount *self = (CmAccount *)object;

  g_free (self->login_id);

  G_OBJECT_CLASS (cm_account_parent_class)->finalize (object);
}

static void
cm_account_class_init (CmAccountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_account_finalize;
}

static void
cm_account_init (CmAccount *self)
{
}

/**
 * cm_account_set_login_id:
 * @self: A #CmAccount
 * @login_id: A login ID for the client.
 *
 * Set login ID for the account.  The login can be a
 * fully qualified matrix ID, or an email address
 * which shall be used to log in to the server
 * in to the server.
 *
 * Returns: %TRUE if @login_id was successfully set,
 * %FALSE otherwise.
 */
gboolean
cm_account_set_login_id (CmAccount  *self,
                         const char *login_id)
{
  g_autoptr(GString) str = NULL;
  CmClient *client;

  g_return_val_if_fail (CM_IS_ACCOUNT (self), FALSE);

  client = cm_user_get_client (CM_USER (self));
  g_return_val_if_fail (CM_IS_CLIENT (client), FALSE);
  g_return_val_if_fail (!cm_client_get_logged_in (client), FALSE);
  g_return_val_if_fail (!cm_client_get_logged_in (client), FALSE);
  g_return_val_if_fail (!cm_client_is_sync (client), FALSE);

  if (login_id && g_strcmp0 (login_id, self->login_id) == 0)
    return TRUE;

  str = g_string_new (NULL);

  if (cm_utils_user_name_valid (login_id) ||
      cm_utils_user_name_is_email (login_id))
    {
      g_free (self->login_id);
      self->login_id = g_strdup (login_id);
      g_debug ("(%p) New login id set: '%s'", client,
               cm_utils_anonymize (str, login_id));

      return TRUE;
    }

  g_debug ("(%p) New login id failed to set: '%s'", client,
           cm_utils_anonymize (str, login_id));

  return FALSE;
}

/**
 * cm_account_get_login_id:
 * @self: A #CmAccount
 *
 * Get the login ID set for the account
 *
 * Returns: (nullable): The login ID of the account
 */
const char *
cm_account_get_login_id (CmAccount *self)
{
  g_return_val_if_fail (CM_IS_ACCOUNT (self), FALSE);

  return self->login_id;
}

static void
account_set_display_name_cb (GObject      *obj,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  CmAccount *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ACCOUNT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

/**
 * cm_account_set_name_async:
 * @self: A #CmAccount
 * @name: (nullable): The display name for the account user
 * @cancellable: A #GCancellable
 * @callback: A callback to run when finished
 * @user_data: The user data for @callback
 *
 * Set the user's name of @self to @name.  If
 * @name is %NULL, the currently set name is
 * unset
 *
 * Finish the call with cm_account_set_name_finish().
 */
void
cm_account_set_display_name_async (CmAccount           *self,
                                   const char          *name,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_autofree char *uri = NULL;
  JsonObject *root = NULL;
  CmClient *client;
  GTask *task;

  g_return_if_fail (CM_IS_ACCOUNT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  if (name && *name)
    {
      root = json_object_new ();
      json_object_set_string_member (root, "displayname", name);
    }

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (name), g_free);

  client = cm_user_get_client (CM_USER (self));
  uri = g_strdup_printf ("/_matrix/client/r0/profile/%s/displayname",
                         cm_client_get_user_id (client));
  cm_net_send_json_async (cm_client_get_net (client), 1,
                          root, uri, SOUP_METHOD_PUT,
                          NULL, cancellable, account_set_display_name_cb, task);
}

/**
 * cm_account_set_name_finish:
 * @self: A #CmAccount
 * @result: A #GAsyncResult
 * @error: A #GError
 *
 * Finish the call started with cm_account_set_name_async().
 *
 * Returns: %TRUE if setting name was a success.
 * %FALSE otherwise with @error set.
 */
gboolean
cm_account_set_display_name_finish (CmAccount     *self,
                                    GAsyncResult  *result,
                                    GError       **error)
{
  g_return_val_if_fail (CM_IS_ACCOUNT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
account_set_user_avatar_cb (GObject      *obj,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  CmAccount *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ACCOUNT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

void
cm_account_set_user_avatar_async (CmAccount           *self,
                                  GFile               *image_file,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  CmClient *client;
  CmNet *cm_net;

  g_return_if_fail (CM_IS_ACCOUNT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  client = cm_user_get_client (CM_USER (self));
  cm_net = cm_client_get_net (client);

  if (!image_file)
    {
      g_autofree char *uri = NULL;
      const char *data;

      data = "{\"avatar_url\":\"\"}";
      uri = g_strdup_printf ("/_matrix/client/r0/profile/%s/avatar_url",
                             cm_client_get_user_id (client));
      cm_net_send_data_async (cm_net, 2, g_strdup (data), strlen (data),
                              uri, SOUP_METHOD_PUT, NULL, cancellable,
                              account_set_user_avatar_cb, g_steal_pointer (&task));
    }
  else
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               "Setting new user avatar not implemented");
    }
}

gboolean
cm_account_set_user_avatar_finish (CmAccount     *self,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  g_return_val_if_fail (CM_IS_ACCOUNT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
account_get_3pid_cb (GObject      *obj,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  CmAccount *self;
  g_autoptr(GTask) task = user_data;
  GPtrArray *emails, *phones;
  GError *error = NULL;
  g_autoptr(JsonObject) object = NULL;
  JsonArray *array;
  guint length;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ACCOUNT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);
  array = cm_utils_json_object_get_array (object, "threepids");

  g_debug ("Getting 3pid, success: %d, user: %s", !error,
           cm_client_get_user_id (cm_user_get_client (CM_USER (self))));

  if (!array)
    {
      if (error)
        g_task_return_error (task, error);
      else
        g_task_return_boolean (task, FALSE);

      return;
    }

  emails = g_ptr_array_new_full (1, g_free);
  phones = g_ptr_array_new_full (1, g_free);

  length = json_array_get_length (array);

  for (guint i = 0; i < length; i++)
    {
      const char *type, *value;

      object = json_array_get_object_element (array, i);
      value = cm_utils_json_object_get_string (object, "address");
      type = cm_utils_json_object_get_string (object, "medium");

      if (g_strcmp0 (type, "email") == 0)
        g_ptr_array_add (emails, g_strdup (value));
      else if (g_strcmp0 (type, "msisdn") == 0)
        g_ptr_array_add (phones, g_strdup (value));
    }

  g_object_set_data_full (G_OBJECT (task), "email", emails,
                          (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full (G_OBJECT (task), "phone", phones,
                          (GDestroyNotify)g_ptr_array_unref);

  g_task_return_boolean (task, TRUE);
}

void
cm_account_get_3pids_async (CmAccount           *self,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  const char *user_id;
  CmClient *client;

  g_return_if_fail (CM_IS_ACCOUNT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  client = cm_user_get_client (CM_USER (self));
  user_id = cm_client_get_user_id (client);

  if (!user_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "user hasn't logged in yet");
      return;
    }

  g_debug ("Getting 3pid of user '%s'", user_id);

  cm_net_send_json_async (cm_client_get_net (client), 1, NULL,
                          "/_matrix/client/r0/account/3pid", SOUP_METHOD_GET,
                          NULL, cancellable, account_get_3pid_cb,
                          g_steal_pointer (&task));
}

/**
 * cm_account_get_3pids_finish:
 * @self: The account
 * @emails:(out)(element-type char*): The emails
 * @phones:(out)(element-type char*): The phone numbers
 * @result: `GAsyncResult`
 * @error: The return location for a recoverable error.
 *
 * Finishes an asynchronous operation started with [method@Account.get_3pids_async].
 *
 * Returns: `TRUE` if the operation was successful
 */
gboolean
cm_account_get_3pids_finish (CmAccount     *self,
                             GPtrArray    **emails,
                             GPtrArray    **phones,
                             GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (CM_IS_ACCOUNT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  if (emails)
    *emails = g_object_steal_data (G_OBJECT (result), "email");
  if (phones)
    *phones = g_object_steal_data (G_OBJECT (result), "phone");

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
account_delete_3pid_cb (GObject      *obj,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  CmAccount *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  g_autoptr(JsonObject) object = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ACCOUNT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);

  if (error)
    {
      g_task_return_error (task, error);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

void
cm_account_delete_3pid_async (CmAccount           *self,
                              const char          *value,
                              const char          *type,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  CmClient *client;
  JsonObject *root;
  GTask *task;

  g_return_if_fail (CM_IS_ACCOUNT (self));
  g_return_if_fail (value && *value);
  g_return_if_fail (g_strcmp0 (type, "email") == 0 || g_strcmp0 (type, "msisdn") == 0);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  root = json_object_new ();
  json_object_set_string_member (root, "address", value);
  if (g_strcmp0 (type, "msisdn") == 0)
    json_object_set_string_member (root, "medium", "msisdn");
  else
    json_object_set_string_member (root, "medium", "email");

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data (G_OBJECT (task), "type", GINT_TO_POINTER (type));
  g_object_set_data_full (G_OBJECT (task), "value",
                          g_strdup (value), g_free);

  client = cm_user_get_client (CM_USER (self));
  cm_net_send_json_async (cm_client_get_net (client), 2, root,
                          "/_matrix/client/r0/account/3pid/delete", SOUP_METHOD_POST,
                          NULL, cancellable, account_delete_3pid_cb, task);
}

gboolean
cm_account_delete_3pid_finish (CmAccount      *self,
                               GAsyncResult  *result,
                               GError       **error)
{
  g_return_val_if_fail (CM_IS_ACCOUNT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
