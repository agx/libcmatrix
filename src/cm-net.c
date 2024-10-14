/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-net"

#include "cm-config.h"

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "cm-common.h"
#include "cm-utils-private.h"
#include "cm-enums.h"
#include "cm-enc-private.h"
#include "cm-input-stream-private.h"
#include "cm-net-private.h"

/**
 * CmNet:
 *
 * Matrix Network related methods
 */

#define MAX_CONNECTIONS     4
#define DATA_BLOCK_SIZE     8192

struct _CmNet
{
  GObject         parent_instance;

  SoupSession    *soup_session;
  SoupSession    *file_session;
  GCancellable   *cancellable;
  char           *homeserver;
  char           *access_token;
};


G_DEFINE_TYPE (CmNet, cm_net, G_TYPE_OBJECT)


static void
net_get_file_stream_cb (GObject      *obj,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  CmNet *self;
  g_autoptr(GTask) task = user_data;
  GInputStream *stream;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_NET (self));

  stream = soup_session_send_finish (SOUP_SESSION (obj), result, &error);

  if (error)
    {
      g_task_return_error (task, error);
    }
  else
    {
      CmInputStream *cm_stream;
      CmEncFileInfo *enc_file;

      cm_stream = cm_input_stream_new (stream);

      enc_file = g_object_get_data (user_data, "file");
      cm_input_stream_set_file_enc (cm_stream, enc_file);

      g_task_return_pointer (task, cm_stream, g_object_unref);
    }
}

static void
parse_from_data (GTask        *task,
                 gpointer      source_object,
                 gpointer      task_data,
                 GCancellable *cancellable)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;
  GError *error = NULL;
  GByteArray *content;

  content = g_object_get_data (G_OBJECT (task), "content");
  parser = json_parser_new ();
  json_parser_load_from_data (parser, (char *)content->data, -1, &error);

  if (!error)
    {
      root = json_parser_get_root (parser);
      error = cm_utils_json_node_get_error (root);
    }

  if (error) {
    if (g_error_matches (error, CM_ERROR, CM_ERROR_LIMIT_EXCEEDED) &&
        root &&
        JSON_NODE_HOLDS_OBJECT (root)) {
      JsonObject *obj;
      guint retry = 0;

      obj = json_node_get_object (root);
      retry = cm_utils_json_object_get_int (obj, "retry_after_ms");
      g_object_set_data (G_OBJECT (task), "retry-after", GINT_TO_POINTER (retry));
    } else {
      g_debug ("Error loading from stream: %s", error->message);
    }

    g_task_return_error (task, error);
    return;
  }

  if (JSON_NODE_HOLDS_OBJECT (root))
    g_task_return_pointer (task, json_node_dup_object (root),
                           (GDestroyNotify)json_object_unref);
  else if (JSON_NODE_HOLDS_ARRAY (root))
    g_task_return_pointer (task, json_node_dup_array (root),
                           (GDestroyNotify)json_array_unref);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "Received invalid data");
}

static void
read_from_stream (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GInputStream *stream;
  GByteArray *content;
  GError *error = NULL;
  gssize n_bytes;
  gsize pos;

  g_assert (G_IS_TASK (task));

  stream = g_object_get_data (user_data, "stream");
  content = g_object_get_data (user_data, "content");
  pos = GPOINTER_TO_SIZE (g_object_get_data (user_data, "pos"));
  g_assert (stream);
  g_assert (content);

  n_bytes = g_input_stream_read_finish (stream, result, &error);

  if (n_bytes < 0)
    {
      g_task_return_error (task, error);
    }
  else if (n_bytes > 0)
    {
      GCancellable *cancellable;

      pos += n_bytes;
      g_object_set_data (user_data, "pos", GSIZE_TO_POINTER (pos));
      g_byte_array_set_size (content, pos + DATA_BLOCK_SIZE + 1);

      cancellable = g_task_get_cancellable (task);
      g_input_stream_read_async (stream,
                                 content->data + pos,
                                 DATA_BLOCK_SIZE,
                                 G_PRIORITY_DEFAULT,
                                 cancellable,
                                 read_from_stream,
                                 g_steal_pointer (&task));
    }
  else
    {
      content->data[pos] = 0;

      if (*(content->data) != '{' &&
          content->len < 1024 &&
          g_ascii_isalnum (*(content->data)))
        g_warning ("Invalid data: %s", (char *)content->data);

      g_task_run_in_thread (task, parse_from_data);
    }
}

static void
session_send_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GInputStream *stream = NULL;
  GCancellable *cancellable;
  GByteArray *content;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  stream = soup_session_send_finish (SOUP_SESSION (object), result, &error);

  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug ("Error session send: %s", error->message);
    g_task_return_error (task, error);
    return;
  }

  content = g_byte_array_new ();
  g_byte_array_set_size (content, DATA_BLOCK_SIZE + 1);
  g_object_set_data_full (user_data, "stream", stream, g_object_unref);
  g_object_set_data_full (user_data, "content", content, (GDestroyNotify)g_byte_array_unref);

  cancellable = g_task_get_cancellable (task);
  g_input_stream_read_async (stream,
                             content->data,
                             DATA_BLOCK_SIZE,
                             G_PRIORITY_DEFAULT,
                             cancellable,
                             read_from_stream,
                             g_steal_pointer (&task));
}

/*
 * queue_data:
 * @data: (transfer full)
 * @size: non-zero if @data is not %NULL
 * @task: (transfer full)
 */
static void
queue_data (CmNet      *self,
            char       *data,
            gsize       size,
            const char *uri_path,
            const char *method, /* interned */
            GHashTable *query,
            GTask      *task)
{
  g_autoptr(SoupMessage) message = NULL;
  g_autoptr(GUri) uri = NULL;
  GUri *old_uri;
  GCancellable *cancellable;
  SoupMessagePriority msg_priority;
  int priority = 0;

  g_assert (CM_IS_NET (self));
  g_assert (uri_path && *uri_path);
  g_assert (method && *method);
  g_return_if_fail (self->homeserver && *self->homeserver);

  g_assert (method == SOUP_METHOD_GET ||
            method == SOUP_METHOD_POST ||
            method == SOUP_METHOD_PUT);

  uri = g_uri_parse (self->homeserver, SOUP_HTTP_URI_FLAGS, NULL);
  old_uri = uri;
  uri = soup_uri_copy (old_uri, SOUP_URI_PATH, uri_path, SOUP_URI_NONE);
  g_clear_pointer (&old_uri, g_uri_unref);

  if (self->access_token) {
    if (!query)
      query = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    g_hash_table_replace (query, g_strdup ("access_token"), g_strdup (self->access_token));
    {
      g_autofree char *query_hash = NULL;

      old_uri = uri;
      query_hash = soup_form_encode_hash (query);
      /* TODO: Using ?access_token is deprecated as of Matrix 1.11 */
      uri = soup_uri_copy (old_uri, SOUP_URI_QUERY, query_hash, SOUP_URI_NONE);
      g_clear_pointer (&old_uri, g_uri_unref);
    }
    g_hash_table_unref (query);
  }

  message = soup_message_new_from_uri (method, uri);
  soup_message_headers_append (soup_message_get_request_headers (message), "Accept-Encoding", "gzip");

  priority = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "priority"));

  if (priority <= -2)
    msg_priority = SOUP_MESSAGE_PRIORITY_VERY_LOW;
  else if (priority == -1)
    msg_priority = SOUP_MESSAGE_PRIORITY_LOW;
  else if (priority == 1)
    msg_priority = SOUP_MESSAGE_PRIORITY_HIGH;
  else if (priority >= 2)
    msg_priority = SOUP_MESSAGE_PRIORITY_VERY_HIGH;
  else
    msg_priority = SOUP_MESSAGE_PRIORITY_NORMAL;

  soup_message_set_priority (message, msg_priority);

  if (data) {
    g_autoptr(GBytes) content_data = g_bytes_new_take (data, size);

    soup_message_set_request_body_from_bytes (message, "application/json", content_data);
  }

  cancellable = g_task_get_cancellable (task);
  g_task_set_task_data (task, g_object_ref (message), g_object_unref);

  soup_session_send_async (self->soup_session, message, msg_priority, cancellable,
                           session_send_cb, task);
}

static void
cm_net_finalize (GObject *object)
{
  CmNet *self = (CmNet *)object;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);

  soup_session_abort (self->soup_session);
  soup_session_abort (self->file_session);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->soup_session);
  g_clear_object (&self->file_session);

  g_free (self->homeserver);
  g_clear_pointer (&self->access_token, gcry_free);

  G_OBJECT_CLASS (cm_net_parent_class)->finalize (object);
}

static void
cm_net_class_init (CmNetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_net_finalize;
}

static void
cm_net_init (CmNet *self)
{
  self->soup_session = g_object_new (SOUP_TYPE_SESSION,
                                     "max-conns-per-host", MAX_CONNECTIONS,
                                     NULL);
  self->file_session = g_object_new (SOUP_TYPE_SESSION,
                                     "max-conns-per-host", MAX_CONNECTIONS,
                                     NULL);
  self->cancellable = g_cancellable_new ();
}

CmNet *
cm_net_new (void)
{
  return g_object_new (CM_TYPE_NET, NULL);
}

void
cm_net_set_homeserver (CmNet      *self,
                       const char *homeserver)
{
  g_return_if_fail (CM_IS_NET (self));
  g_return_if_fail (homeserver && *homeserver);

  g_free (self->homeserver);
  self->homeserver = g_strdup (homeserver);
}

void
cm_net_set_access_token (CmNet      *self,
                         const char *access_token)
{
  g_return_if_fail (CM_IS_NET (self));

  g_clear_pointer (&self->access_token, gcry_free);

  if (access_token && *access_token)
    {
      self->access_token = gcry_malloc_secure (strlen (access_token) + 1);
      strcpy (self->access_token, access_token);
    }
}

const char *
cm_net_get_access_token (CmNet *self)
{
  g_return_val_if_fail (CM_IS_NET (self), NULL);

  return self->access_token;
}

/**
 * cm_net_send_data_async:
 * @self: A #CmNet
 * @priority: The priority of request, 0 for default
 * @data: (nullable) (transfer full): The data to send
 * @size: The @data size in bytes
 * @uri_path: A string of the matrix uri path
 * @method: An interned string for GET, PUT, POST, etc.
 * @query: (nullable): A query to pass to internal #GUri
 * @cancellable: (nullable): A #GCancellable
 * @callback: The callback to run when completed
 * @user_data: user data for @callback
 *
 * Send a JSON data @object to the @uri_path endpoint.
 * @method should be one of %SOUP_METHOD_GET, %SOUP_METHOD_PUT
 * or %SOUP_METHOD_POST.
 * If @cancellable is %NULL, the internal cancellable
 * shall be used
 */
void
cm_net_send_data_async (CmNet               *self,
                        int                  priority,
                        char                *data,
                        gsize                size,
                        const char          *uri_path,
                        const char          *method, /* interned */
                        GHashTable          *query,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_NET (self));
  g_return_if_fail (uri_path && *uri_path);
  g_return_if_fail (method && *method);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback);
  g_return_if_fail (self->homeserver && *self->homeserver);

  if (data && *data)
    g_return_if_fail (size);

  if (!cancellable)
    cancellable = self->cancellable;

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data (G_OBJECT (task), "priority", GINT_TO_POINTER (priority));

  queue_data (self, data, size, uri_path, method, query, task);
}

/**
 * cm_net_send_json_async:
 * @self: A #CmNet
 * @priority: The priority of request, 0 for default
 * @object: (nullable) (transfer full): The data to send
 * @uri_path: A string of the matrix uri path
 * @method: An interned string for GET, PUT, POST, etc.
 * @query: (nullable): A query to pass to internal #GUri
 * @cancellable: (nullable): A #GCancellable
 * @callback: The callback to run when completed
 * @user_data: user data for @callback
 *
 * Send a JSON data @object to the @uri_path endpoint.
 * @method should be one of %SOUP_METHOD_GET, %SOUP_METHOD_PUT
 * or %SOUP_METHOD_POST.
 * If @cancellable is %NULL, the internal cancellable
 * shall be used
 */
void
cm_net_send_json_async (CmNet               *self,
                        int                  priority,
                        JsonObject          *object,
                        const char          *uri_path,
                        const char          *method, /* interned */
                        GHashTable          *query,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GTask *task;
  char *data = NULL;
  gsize size = 0;

  g_return_if_fail (CM_IS_NET (self));
  g_return_if_fail (uri_path && *uri_path);
  g_return_if_fail (method && *method);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback);
  g_return_if_fail (self->homeserver && *self->homeserver);

  if (object)
    {
      data = cm_utils_json_object_to_string (object, FALSE);
      json_object_unref (object);
    }

  if (data && *data)
    size = strlen (data);

  if (!cancellable)
    cancellable = self->cancellable;

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data (G_OBJECT (task), "priority", GINT_TO_POINTER (priority));

  queue_data (self, data, size, uri_path, method, query, task);
}

/**
 * cm_net_get_file_async:
 * @self: A #CmNet
 * @message: (nullable) (transfer full): A #ChattyMessage
 * @file: A #ChattyFileInfo
 * @cancellable: (nullable): A #GCancellable
 * @progress_callback: (nullable): A #GFileProgressCallback
 * @callback: The callback to run when completed
 * @user_data: user data for @callback
 *
 * Download the file @file.  @file path shall be updated
 * after download is completed, and if @file is encrypted
 * and has keys to decrypt the file, the file shall be
 * stored decrypted.
 */
void
cm_net_get_file_async (CmNet                 *self,
                       const char            *uri,
                       CmEncFileInfo         *enc_file,
                       const char * const    *versions,
                       GCancellable          *cancellable,
                       GAsyncReadyCallback    callback,
                       gpointer               user_data)
{
  g_autofree char *url = NULL;
  SoupMessage *msg;
  GTask *task;
  gboolean needs_auth = FALSE;

  g_return_if_fail (CM_IS_NET (self));
  g_return_if_fail (uri && *uri);

  if (!cancellable)
    cancellable = self->cancellable;

  if (g_str_has_prefix (uri, "mxc://")) {
    const char *file_url;

    file_url = uri + strlen ("mxc://");
    if (versions && g_strv_contains (versions, "v1.11")) {
      /* Endpoints for authenticated media
       * https://matrix.org/blog/2024/06/20/matrix-v1.11-release/ */
      url = g_strconcat (self->homeserver,
                         "/_matrix/client/v1/media/download/", file_url, NULL);
      needs_auth = TRUE;
    } else {
      url = g_strconcat (self->homeserver,
                         "/_matrix/media/r0/download/", file_url, NULL);
    }
  }

  if (!url)
    url = g_strdup (uri);

  msg = soup_message_new (SOUP_METHOD_GET, url);

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data_full (G_OBJECT (task), "url", g_strdup (url), g_free);
  g_object_set_data (G_OBJECT (task), "file", enc_file);
  g_object_set_data_full (G_OBJECT (task), "msg", msg, g_object_unref);

  if (needs_auth && self->access_token) {
    SoupMessageHeaders *headers = soup_message_get_request_headers (msg);
    g_autofree char *auth = g_strdup_printf ("Bearer %s", self->access_token);

    soup_message_headers_replace (headers, "Authorization", auth);
  }

  soup_session_send_async (self->file_session, msg, 0, cancellable,
                           net_get_file_stream_cb, task);
}

GInputStream *
cm_net_get_file_finish (CmNet         *self,
                        GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (CM_IS_NET (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
put_file_async_cb (GObject      *obj,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  CmNet *self = user_data;
  g_autoptr(JsonObject) root = NULL;
  GError *error = NULL;
  GTask *task, *local_task;

  local_task = G_TASK (result);
  g_assert (G_IS_TASK (local_task));

  self = g_task_get_source_object (local_task);
  task = g_task_get_task_data (local_task);

  g_assert (G_IS_TASK (task));
  g_assert (CM_IS_NET (self));

  root = g_task_propagate_pointer (G_TASK (result), &error);
  g_assert_no_error (error);

  if (root)
    {
      const char *file_url;

      file_url = cm_utils_json_object_get_string (root, "content_uri");
      g_task_return_pointer (task, g_strdup (file_url), g_free);
    }
  else
    {
      if (error)
        g_task_return_error (task, error);
      else
        g_task_return_pointer (task, NULL, NULL);
    }
}

static void
wrote_body_data_cb (GTask       *task,
                    SoupMessage *msg,
                    guint       chunk_size)
{
  GFileProgressCallback progress_cb;
  gpointer progress_user_data;

  progress_cb = g_object_get_data (G_OBJECT (task), "progress-cb");
  progress_user_data = g_object_get_data (G_OBJECT (task), "progress-cb-data");
  g_assert (progress_cb);

  progress_cb (0, 0, progress_user_data);
}

void
cm_net_put_file_async (CmNet                 *self,
                       GFile                 *file,
                       gboolean               encrypt,
                       GFileProgressCallback  progress_callback,
                       gpointer               progress_user_data,
                       GCancellable          *cancellable,
                       GAsyncReadyCallback    callback,
                       gpointer               user_data)
{
  g_autoptr(GHashTable) query = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *url = NULL;
  CmInputStream *cm_stream;
  GTask *task, *local_task;
  SoupMessage *msg;

  if (!cancellable)
    cancellable = self->cancellable;

  cm_stream = cm_input_stream_new_from_file (file, encrypt, cancellable, &error);
  task = g_task_new (self, cancellable, callback, user_data);

  if (!cm_stream)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to create stream: %s", error->message ?: "");
      return;
    }

  query = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_replace (query, g_strdup ("filename"), g_file_get_basename (file));
  g_hash_table_replace (query, g_strdup ("access_token"), g_strdup (self->access_token));

  url = g_strconcat (self->homeserver, "/_matrix/media/r0/upload", NULL);
  msg = soup_message_new (SOUP_METHOD_POST, url);

  soup_message_set_uri (msg, soup_uri_copy (soup_message_get_uri (msg), SOUP_URI_QUERY,
                                            soup_form_encode_hash (query), SOUP_URI_NONE));

  /* We're uploading files in chunk */
  soup_message_set_request_body (msg,
                                 cm_input_stream_get_content_type (cm_stream),
                                 G_INPUT_STREAM (cm_stream),
                                 cm_input_stream_get_size (cm_stream));

  g_task_set_task_data (task, g_object_ref (file), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "msg", msg, g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "stream", cm_stream, g_object_unref);

  local_task = g_task_new (self, cancellable, put_file_async_cb, self);
  g_task_set_task_data (local_task, task, g_object_unref);

  if (progress_callback)
    g_signal_connect_object (msg, "wrote-body-data",
                             G_CALLBACK (wrote_body_data_cb), task, G_CONNECT_SWAPPED);

  soup_session_send_async (self->file_session, msg, 0, cancellable,
                           session_send_cb, local_task);
}

char *
cm_net_put_file_finish (CmNet         *self,
                        GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (CM_IS_NET (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}
