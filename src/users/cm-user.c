/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-user.c
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

#include "cm-utils-private.h"
#include "cm-client-private.h"
#include "cm-user-private.h"
#include "cm-user.h"

typedef struct
{
  CmClient *cm_client;

  char *user_id;
  char *display_name;
  char *avatar_url;
  gboolean info_loaded;
} CmUserPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (CmUser, cm_user, G_TYPE_OBJECT)

static void
cm_user_finalize (GObject *object)
{
  CmUser *self = (CmUser *)object;
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_free (priv->user_id);
  g_free (priv->display_name);
  g_free (priv->avatar_url);

  G_OBJECT_CLASS (cm_user_parent_class)->finalize (object);
}

static void
cm_user_class_init (CmUserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_user_finalize;
}

static void
cm_user_init (CmUser *self)
{
}

void
cm_user_set_client (CmUser   *self,
                    CmClient *client)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (!priv->cm_client);

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
                     const char *user_id)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (!priv->user_id);

  priv->user_id = g_strdup (user_id);
}

void
cm_user_set_details (CmUser     *self,
                     const char *display_name,
                     const char *avatar_url)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);

  g_return_if_fail (CM_IS_USER (self));

  g_free (priv->display_name);
  g_free (priv->avatar_url);

  priv->display_name = g_strdup (display_name);
  priv->avatar_url = g_strdup (avatar_url);
}

const char *
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
  g_autoptr(GTask) task = user_data;
  GInputStream *stream;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  stream = cm_client_get_file_finish (CM_CLIENT (object), result, &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, stream, g_object_unref);
}

void
cm_user_get_avatar_async (CmUser              *self,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  GTask *task;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, cm_user_get_avatar_async);

  if ((!priv->display_name && !priv->avatar_url) || !priv->info_loaded)
    cm_user_load_info_async (self, cancellable,
                             user_get_avatar_cb, task);
  else if (priv->avatar_url)
    cm_client_get_file_async (priv->cm_client, priv->avatar_url, cancellable,
                              NULL, NULL,
                              user_get_avatar_cb, g_steal_pointer (&task));
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

  if (g_task_get_source_tag (task) == cm_user_get_avatar_async)
    {
      GCancellable *cancellable;

      cancellable = g_task_get_cancellable (task);

      if (priv->avatar_url)
        cm_client_get_file_async (priv->cm_client, priv->avatar_url, cancellable,
                                  NULL, NULL,
                                  user_get_avatar_cb, g_steal_pointer (&task));
      else
        g_task_return_pointer (task, NULL, NULL);
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }
}

void
cm_user_load_info_async (CmUser              *self,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  CmUserPrivate *priv = cm_user_get_instance_private (self);
  g_autofree char *uri = NULL;
  GTask *task;

  g_return_if_fail (CM_IS_USER (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  uri = g_strdup_printf ("/_matrix/client/r0/profile/%s", priv->user_id);
  cm_net_send_json_async (cm_client_get_net (priv->cm_client),
                          1, NULL, uri, SOUP_METHOD_GET,
                          NULL, cancellable, user_get_user_info_cb, task);
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
