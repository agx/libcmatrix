/* cm-pusher.c
 *
 * Copyright 2024 The Phosh Developers
 *
 * Author(s):
 *   Guido Günther <agx@sigxcpu.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-pusher"

#include "cm-config.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "cm-common.h"
#include "cm-enums.h"
#include "cm-pusher.h"
#include "cm-utils-private.h"

/**
 * CmPusher:
 *
 * Configuration for a server side pusher.
 *
 * A pusher is a worker on the homeserver that manages the sending of
 * push notifications for a user. A user can have multiple pushers. This
 * class allows to set the pushers properties (like the push gateway
 * where push notifications should be sent to).
 *
 * Since: 0.0.1
 */

struct _CmPusher
{
  GObject       parent_instance;

  CmPusherKind  kind;
  char         *app_display_name;
  char         *app_id;
  char         *device_display_name;
  char         *lang;
  char         *profile_tag;
  char         *pushkey;
  /* http kind only */
  char         *url;

  SoupSession  *soup_session;
};

G_DEFINE_TYPE (CmPusher, cm_pusher, G_TYPE_OBJECT)

static void
cm_pusher_dispose (GObject *object)
{
  CmPusher *self = CM_PUSHER (object);

  g_clear_object (&self->soup_session);

  G_OBJECT_CLASS (cm_pusher_parent_class)->dispose (object);
}

static void
cm_pusher_finalize (GObject *object)
{
  CmPusher *self = CM_PUSHER (object);

  g_free (self->app_display_name);
  g_free (self->app_id);
  g_free (self->device_display_name);
  g_free (self->lang);
  g_free (self->profile_tag);
  g_free (self->pushkey);
  g_free (self->url);

  G_OBJECT_CLASS (cm_pusher_parent_class)->finalize (object);
}

static void
cm_pusher_class_init (CmPusherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cm_pusher_dispose;
  object_class->finalize = cm_pusher_finalize;
}

static void
cm_pusher_init (CmPusher *self)
{
}

/**
 * cm_pusher_new:
 *
 * Returns: (transfer full): A #CmPusher
 *
 * Since: 0.0.1
 */
CmPusher *
cm_pusher_new (void)
{
  return g_object_new (CM_TYPE_PUSHER, NULL);
}

/**
 * cm_pusher_get_url:
 * @self: The pusher
 *
 * Get the URL for push notifications.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_url (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->url;
}

/**
 * cm_pusher_set_url:
 * @self: The pusher
 * @url: The url to set
 *
 * Set the URL where push notifications should be sent to.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_url (CmPusher *self, const char *url)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->url);
  self->url = g_strdup (url);
}

/**
 * cm_pusher_get_kind:
 * @self: The pusher
 *
 * Get the kind of the pusher.
 *
 * Since: 0.0.1
 */
CmPusherKind
cm_pusher_get_kind (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), CM_PUSHER_KIND_UNKNOWN);

  return self->kind;
}

/**
 * cm_pusher_get_kind_as_string:
 * @self: The pusher
 *
 * Get the kind of the pusher as string. E.g. `http`.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_kind_as_string (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  switch (self->kind) {
  case CM_PUSHER_KIND_HTTP:
    return "http";
  case CM_PUSHER_KIND_EMAIL:
        return "email";
  case CM_PUSHER_KIND_UNKNOWN:
  default:
    return NULL;
  }
}

/**
 * cm_pusher_set_kind:
 * @self: The pusher
 * @kind: The kind of the pusher
 *
 * Set the kind of the pusher.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_kind (CmPusher *self, CmPusherKind kind)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  self->kind = kind;
}


/**
 * cm_pusher_set_kind_from_string:
 * @self: The pusher
 * @kind: The kind of the pusher as string
 *
 * Set the kind of the pusher from a string like `http`.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_kind_from_string (CmPusher *self, const char *kind)
{
  g_return_if_fail (CM_IS_PUSHER (self));
  g_return_if_fail (kind);

  if (g_str_equal (kind, "http")) {
    self->kind = CM_PUSHER_KIND_HTTP;
  } else if (g_str_equal (kind, "email")) {
    self->kind = CM_PUSHER_KIND_EMAIL;
  } else {
    self->kind = CM_PUSHER_KIND_UNKNOWN;
  }
}

/**
 * cm_pusher_get_app_display_name:
 * @self: The pusher
 *
 * Get the display name of the application this pusher belongs to.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_app_display_name (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->app_display_name;
}

/**
 * cm_pusher_set_app_display_name:
 * @self: The pusher
 * @app_display_name: The application's display name
 *
 * Set the display name of the application this pusher belongs to.
 * This should be a user friendly name that applications can show when
 * listing pushers.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_app_display_name (CmPusher *self, const char *app_display_name)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->app_display_name);
  self->app_display_name = g_strdup (app_display_name);
}

/**
 * cm_pusher_get_app_id:
 * @self: The pusher
 *
 * Get the app-id of the application this pusher belongs to.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_app_id (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->app_id;
}

/**
 * cm_pusher_set_app_id:
 * @self: The pusher
 * @app_id: An app-id
 *
 * Pushers can have an app-id associated with them so an app can find
 * the pushers it has configured. This should be in reverse DNS
 * notation like `com.example.Client`.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_app_id (CmPusher *self, const char *app_id)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->app_id);
  self->app_id = g_strdup (app_id);
}

/**
 * cm_pusher_get_device_display_name:
 * @self: The pusher
 *
 * Get the display name of the device this pusher belongs to.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_device_display_name (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->device_display_name;
}

/**
 * cm_pusher_set_device_display_name:
 * @self: The pusher
 * @device_display_name: An display name for a device
 *
 * Set the display name of the device this pusher belongs to.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_device_display_name (CmPusher *self, const char *device_display_name)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->device_display_name);
  self->device_display_name = g_strdup (device_display_name);
}

/**
 * cm_pusher_get_lang:
 * @self: The pusher
 *
 * Get the language of this pusher.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_lang (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->lang;
}

/**
 * cm_pusher_set_lang:
 * @self: The pusher
 * @lang: The language to use
 *
 * Set the preferred language for receiving notifications (e.g. `en-US`).
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_lang (CmPusher *self, const char *lang)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->lang);
  self->lang = g_strdup (lang);
}

/**
 * cm_pusher_get_profile_tag:
 * @self: The pusher
 *
 * Get the profile tag for this pusher.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_profile_tag (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->profile_tag;
}

/**
 * cm_pusher_set_profile_tag:
 * @self: The pusher
 * @profile_tag: The profile tag to use
 *
 * Set the profile tag for this pusher. The profile tag specifies which
 * set of device specific rules this pusher executes.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_profile_tag (CmPusher *self, const char *profile_tag)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->profile_tag);
  self->profile_tag = g_strdup (profile_tag);
}

/**
 * cm_pusher_get_pushkey:
 * @self: The pusher
 *
 * Get the pushkey for this pusher.
 *
 * Since: 0.0.1
 */
const char *
cm_pusher_get_pushkey (CmPusher *self)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), NULL);

  return self->pushkey;
}

/**
 * cm_pusher_set_pushkey:
 * @self: The pusher
 * @pushkey: The pushkey to set
 *
 * Set the pushkey for this pusher. The pushkey is a unique identifier
 * for this pusher.
 *
 * Since: 0.0.1
 */
void
cm_pusher_set_pushkey (CmPusher *self, const char *pushkey)
{
  g_return_if_fail (CM_IS_PUSHER (self));

  g_free (self->pushkey);
  self->pushkey = g_strdup (pushkey);
}

static void
cm_pusher_check_valid_cb (GObject *object, GAsyncResult *result, gpointer user_data)
{
  g_autoptr(GTask) task = G_TASK (user_data);
  g_autoptr(JsonParser) parser = json_parser_new ();
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  JsonNode *root;
  JsonObject *obj;
  const char *data, *val;
  gboolean success;
  gsize len;

  g_assert (G_IS_TASK (task));

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (object),
                                             result,
                                             &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  data = g_bytes_get_data (bytes, &len);
  success = json_parser_load_from_data (parser, data, len, &error);
  if (!success) {
    g_debug ("Failed to parse JSON: %s", error->message);
    g_task_return_new_error (task, CM_ERROR, CM_ERROR_BAD_PUSH_GATEWAY,
                             "%s", "Endpoint didn't return valid JSON");
    return;
  }

  root = json_parser_get_root (parser);
  error = cm_utils_json_node_get_error (root);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  if (!JSON_NODE_HOLDS_OBJECT (root)) {
    g_task_return_new_error (task, CM_ERROR, CM_ERROR_BAD_PUSH_GATEWAY,
                             "%s", "Endpoint didn't return a JSON object");
    return;
  }

  obj = json_node_get_object (root);
  obj = cm_utils_json_object_get_object (obj, "unifiedpush");
  if (!obj) {
    g_task_return_new_error (task, CM_ERROR, CM_ERROR_BAD_PUSH_GATEWAY,
                             "%s", "Not a UP gateway");
    return;
  }

  val = cm_utils_json_object_get_string (obj, "gateway");
  if (g_strcmp0 (val, "matrix")) {
    g_task_return_new_error (task, CM_ERROR, CM_ERROR_BAD_PUSH_GATEWAY,
                             "%s", "Not a UP matrix gateway");
    return;
  }

  g_task_return_boolean (task, TRUE);
}

/**
 * cm_pusher_check_valid:
 * @self: The pusher
 * @cancellable: (nullable): A #GCancellable
 * @callback: A #GAsyncReadyCallback
 * @user_data: The user data for @callback.
 *
 * Checks if the given pusher is valid. For http pushers this means checking
 * the remote endpoint.
 *
 * Since: 0.0.1
 */
void
cm_pusher_check_valid (CmPusher            *self,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GUri) uri = NULL;
  g_autoptr(SoupMessage) message = NULL;

  g_return_if_fail (CM_IS_PUSHER (self));
  g_return_if_fail (self->url);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  uri = g_uri_parse (self->url, SOUP_HTTP_URI_FLAGS, NULL);
  if (!uri) {
    g_task_return_new_error (task, CM_ERROR, CM_ERROR_BAD_PUSH_GATEWAY,
                             "%s", "Invalid URI");
    return;
  }

  message = soup_message_new_from_uri (SOUP_METHOD_GET, uri);

  self->soup_session = g_object_new (SOUP_TYPE_SESSION,
                                     "max-conns-per-host", 10,
                                     NULL);

  soup_session_send_and_read_async (g_steal_pointer (&self->soup_session),
                                    message,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    cm_pusher_check_valid_cb,
                                    g_steal_pointer (&task));
}

/**
 * cm_pusher_check_valid_finish:
 * @self: The pusher
 * @result: `GAsyncResult`
 * @error: The return location for a recoverable error.
 *
 * Finishes an asynchronous operation started with
 * [method@Pusher.check_valid]. If the operation failed or the
 * pusher is not valid `FALSE` is returned and `error` indicates
 * the kind of error.
 *
 * Returns: `TRUE` if the pusher is valid otherwise `FALSE`
 *
 * Since: 0.0.1
 */
gboolean
cm_pusher_check_valid_finish (CmPusher      *self,
                              GAsyncResult  *result,
                              GError       **error)
{
  g_return_val_if_fail (CM_IS_PUSHER (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
