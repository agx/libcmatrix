/*
 * Copyright (C) 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-room-message-event"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-utils-private.h"
#include "cm-common.h"
#include "cm-matrix-private.h"
#include "cm-enums.h"
#include "cm-client-private.h"
#include "cm-room-private.h"
#include "cm-input-stream-private.h"
#include "cm-event-private.h"
#include "cm-room-message-event-private.h"

struct _CmRoomMessageEvent
{
  CmRoomEvent     parent_instance;

  JsonObject     *json;
  CmContentType   type;

  char           *body;
  char           *file_path;    /* Local path to which file is saved */
  GFile          *file;
  GInputStream   *file_istream;
  char           *mxc_uri;

  gboolean       downloading_file;
};

G_DEFINE_TYPE (CmRoomMessageEvent, cm_room_message_event, CM_TYPE_ROOM_EVENT)

static JsonObject *
room_message_generate_json (CmRoomMessageEvent *self,
                            CmRoom             *room)
{
  g_autofree char *uri = NULL;
  const char *body, *room_id;
  CmClient *client;
  JsonObject *root;
  GFile *file;

  g_assert (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_assert (CM_IS_ROOM (room));

  body = cm_room_message_event_get_body (self);
  file = cm_room_message_event_get_file (self);
  client = cm_room_get_client (room);
  room_id = cm_room_get_id (room);

  root = json_object_new ();
  if (file)
    {
      g_autofree char *name = NULL;

      name = g_file_get_basename (file);
      json_object_set_string_member (root, "msgtype", "m.file");
      json_object_set_string_member (root, "body", name);
      json_object_set_string_member (root, "filename", name);
      if (!cm_room_is_encrypted (room))
        {
          const char *mxc_uri;

          mxc_uri = g_object_get_data (G_OBJECT (file), "uri");
          if (mxc_uri)
            json_object_set_string_member (root, "url", mxc_uri);
          else
            g_warn_if_reached ();
        }
    }
  else
    {
      json_object_set_string_member (root, "msgtype", "m.text");
      json_object_set_string_member (root, "body", body);
    }

  if (cm_room_is_encrypted (room))
    {
      g_autofree char *text = NULL;
      JsonObject *object;

      object = json_object_new ();
      json_object_set_string_member (object, "type", "m.room.message");
      json_object_set_string_member (object, "room_id", room_id);
      json_object_set_object_member (object, "content", root);

      if (file)
        {
          JsonObject *file_json;
          CmInputStream *stream;

          stream = g_object_get_data (G_OBJECT (file), "stream");
          file_json = cm_input_stream_get_file_json (stream);
          json_object_set_object_member (root, "file", file_json);
        }

      text = cm_utils_json_object_to_string (object, FALSE);
      json_object_unref (object);
      object = cm_enc_encrypt_for_chat (cm_client_get_enc (client),
                                        room, text);
      return object;
    }
  else
    {
      return root;
    }
}

static gpointer
cm_room_message_event_generate_json (CmEvent  *event,
                                     gpointer  room)
{
  CmRoomMessageEvent *self = (CmRoomMessageEvent *)event;

  g_assert (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_val_if_fail (CM_IS_ROOM (room), NULL);

  return room_message_generate_json (self, room);
}

static char *
cm_room_message_event_get_api_url (CmEvent  *event,
                                   gpointer  room)
{
  CmRoomMessageEvent *self = (CmRoomMessageEvent *)event;
  char *uri;

  g_assert (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_val_if_fail (CM_IS_ROOM (room), NULL);
  g_return_val_if_fail (cm_event_get_txn_id (event), NULL);

  if (cm_room_is_encrypted (room))
    uri = g_strdup_printf ("/_matrix/client/r0/rooms/%s/send/m.room.encrypted/%s",
                           cm_room_get_id (room),
                           cm_event_get_txn_id (event));
  else
    uri = g_strdup_printf ("/_matrix/client/r0/rooms/%s/send/m.room.message/%s",
                           cm_room_get_id (room),
                           cm_event_get_txn_id (event));

  return uri;
}

static void
cm_room_message_event_finalize (GObject *object)
{
  CmRoomMessageEvent *self = (CmRoomMessageEvent *)object;

  g_clear_pointer (&self->json, json_object_unref);
  g_free (self->body);
  g_free (self->mxc_uri);
  g_free (self->file_path);

  G_OBJECT_CLASS (cm_room_message_event_parent_class)->finalize (object);
}

static void
cm_room_message_event_class_init (CmRoomMessageEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CmEventClass *event_class = CM_EVENT_CLASS (klass);

  object_class->finalize = cm_room_message_event_finalize;

  event_class->generate_json = cm_room_message_event_generate_json;
  event_class->get_api_url = cm_room_message_event_get_api_url;
}

static void
cm_room_message_event_init (CmRoomMessageEvent *self)
{
}

CmRoomMessageEvent *
cm_room_message_event_new (CmContentType type)
{
  CmRoomMessageEvent *self;

  self = g_object_new (CM_TYPE_ROOM_MESSAGE_EVENT, NULL);
  self->type = type;

  return self;
}

CmRoomEvent *
cm_room_message_event_new_from_json (JsonObject *root)
{
  CmRoomMessageEvent *self;
  const char *type, *body;
  JsonObject *child;

  g_return_val_if_fail (root, NULL);

  type = cm_utils_json_object_get_string (root, "type");

  if (g_strcmp0 (type, "m.room.message") != 0)
    g_return_val_if_reached (NULL);

  self = g_object_new (CM_TYPE_ROOM_MESSAGE_EVENT, NULL);
  self->json = json_object_ref (root);

  child = cm_utils_json_object_get_object (root, "content");
  type = cm_utils_json_object_get_string (child, "msgtype");
  body = cm_utils_json_object_get_string (child, "body");
  self->body = g_strdup (body);
  self->mxc_uri = g_strdup (cm_utils_json_object_get_string (child, "url"));

  if (g_strcmp0 (type, "m.text") == 0)
    self->type = CM_CONTENT_TYPE_TEXT;
  else if (g_strcmp0 (type, "m.file") == 0)
    self->type = CM_CONTENT_TYPE_FILE;
  else if (g_strcmp0 (type, "m.image") == 0)
    self->type = CM_CONTENT_TYPE_IMAGE;
  else if (g_strcmp0 (type, "m.audio") == 0)
    self->type = CM_CONTENT_TYPE_AUDIO;
  else if (g_strcmp0 (type, "m.location") == 0)
    self->type = CM_CONTENT_TYPE_LOCATION;
  else if (g_strcmp0 (type, "m.emote") == 0)
    self->type = CM_CONTENT_TYPE_EMOTE;
  else if (g_strcmp0 (type, "m.notice") == 0)
    self->type = CM_CONTENT_TYPE_NOTICE;

  return CM_ROOM_EVENT (self);
}

CmContentType
cm_room_message_event_get_msg_type (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), CM_CONTENT_TYPE_UNKNOWN);

  return self->type;
}

void
cm_room_message_event_set_body (CmRoomMessageEvent *self,
                                const char         *text)
{
  g_return_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_if_fail (self->type == CM_CONTENT_TYPE_TEXT);

  g_free (self->body);
  self->body = g_strdup (text);
}

const char *
cm_room_message_event_get_body (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), NULL);

  return self->body;
}

const char *
cm_room_message_event_get_file_path (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), NULL);

  return self->file_path;
}

void
cm_room_message_event_set_file (CmRoomMessageEvent *self,
                                const char         *body,
                                GFile              *file)
{
  g_return_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_if_fail (self->type == CM_CONTENT_TYPE_FILE);
  g_return_if_fail (!self->file);

  g_set_object (&self->file, file);
  g_free (self->body);

  if (body && *body)
    self->body = g_strdup (body);
  else if (file)
    self->body = g_file_get_basename (file);
}

GFile *
cm_room_message_event_get_file (CmRoomMessageEvent *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), NULL);

  return self->file;
}

static void
message_file_stream_cb (GObject      *obj,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  CmRoomMessageEvent *self;
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  GFile *out_file;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_ROOM_MESSAGE_EVENT (self));

  out_file = cm_utils_save_url_to_path_finish (result, &error);
  self->downloading_file = FALSE;

  if (error)
    g_task_return_error (task, error);
  else if (!out_file)
    g_task_return_pointer (task, NULL, NULL);
  else
    {
      g_autofree char *file_name = NULL;

      self->file_istream = (GInputStream *)g_file_read (out_file, NULL, NULL);
      g_object_set_data_full (G_OBJECT (self->file_istream), "out-file",
                              out_file, g_object_unref);
      g_task_return_pointer (task, g_object_ref (self->file_istream), g_object_unref);
    }
}

void
cm_room_message_event_get_file_async (CmRoomMessageEvent    *self,
                                      GCancellable          *cancellable,
                                      GFileProgressCallback  progress_callback,
                                      gpointer               progress_user_data,
                                      GAsyncReadyCallback    callback,
                                      gpointer               user_data)
{
  g_autofree char *file_name = NULL;
  const char *path;
  CmRoom *room;
  GTask *task;

  g_return_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (self->type == CM_CONTENT_TYPE_FILE ||
                    self->type == CM_CONTENT_TYPE_AUDIO ||
                    self->type == CM_CONTENT_TYPE_IMAGE);

  task = g_task_new (self, cancellable, callback, user_data);
  g_object_set_data (G_OBJECT (task), "progress-cb", progress_callback);
  g_object_set_data (G_OBJECT (task), "progress-cb-data", progress_user_data);

  if (self->downloading_file)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "File already being downloaded");
      return;
    }

  if (self->file && !self->mxc_uri && !self->file_istream)
    self->file_istream = (gpointer)g_file_read (self->file, NULL, NULL);

  if (self->file_istream)
    {
      g_task_return_pointer (task, g_object_ref (self->file_istream), g_object_unref);
      return;
    }

  g_return_if_fail (self->mxc_uri);

  self->downloading_file = TRUE;
  room = cm_room_event_get_room (CM_ROOM_EVENT (self));

  path = cm_matrix_get_data_dir ();
  file_name = g_path_get_basename (self->mxc_uri);
  self->file_path = cm_utils_get_path_for_m_type (path, CM_M_ROOM_MESSAGE, FALSE, file_name);
  cm_utils_save_url_to_path_async (cm_room_get_client (room),
                                   self->mxc_uri,
                                   g_strdup (self->file_path),
                                   cancellable,
                                   NULL, NULL,
                                   message_file_stream_cb,
                                   g_steal_pointer (&task));
}

GInputStream *
cm_room_message_event_get_file_finish (CmRoomMessageEvent  *self,
                                       GAsyncResult        *result,
                                       GError             **error)
{
  g_return_val_if_fail (CM_IS_ROOM_MESSAGE_EVENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}
