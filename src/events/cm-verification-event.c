/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-verification-event"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-client-private.h"
#include "cm-olm-sas-private.h"
#include "cm-utils-private.h"
#include "users/cm-user-list-private.h"
#include "cm-verification-event-private.h"
#include "cm-verification-event.h"

struct _CmVerificationEvent
{
  CmEvent        parent_instance;

  CmClient      *client;

  GTask         *pending_task;

  /* transaction_id in key verification */
  char          *transaction_id;
  char          *verification_key;
};

G_DEFINE_TYPE (CmVerificationEvent, cm_verification_event, CM_TYPE_EVENT)

static void
key_verification_continue_cb (GObject      *obj,
                              GAsyncResult *result,
                              gpointer      user_data);

static void
verification_event_updated_cb (CmVerificationEvent *self)
{
  g_assert (CM_IS_VERIFICATION_EVENT (self));

  if (cm_event_get_m_type (CM_EVENT (self)) == CM_M_KEY_VERIFICATION_REQUEST &&
      g_object_get_data (G_OBJECT (self), "start") &&
      g_object_get_data (G_OBJECT (self), "ready-complete") &&
      self->pending_task)
    {
      g_autofree char *uri = NULL;
      GCancellable *cancellable;
      CmOlmSas *olm_sas = NULL;
      CmEvent *reply_event;
      JsonObject *root;

      cancellable = g_task_get_cancellable (self->pending_task);
      olm_sas = g_object_get_data (G_OBJECT (self), "olm-sas");
      reply_event = cm_olm_sas_get_accept_event (olm_sas);
      root = cm_event_get_json (reply_event);
      uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.accept/%s",
                             cm_event_get_txn_id (reply_event));
      cm_net_send_json_async (cm_client_get_net (self->client),
                              0, root, uri, SOUP_METHOD_PUT,
                              NULL, cancellable,
                              key_verification_continue_cb,
                              g_object_ref (self->pending_task));
    }
}

static void
cm_verification_event_finalize (GObject *object)
{
  CmVerificationEvent *self = (CmVerificationEvent *)object;

  g_clear_object (&self->pending_task);
  g_free (self->transaction_id);
  g_free (self->verification_key);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (cm_verification_event_parent_class)->finalize (object);
}

static void
cm_verification_event_class_init (CmVerificationEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_verification_event_finalize;
}

static void
cm_verification_event_init (CmVerificationEvent *self)
{
}

CmVerificationEvent *
cm_verification_event_new (gpointer client)
{
  CmVerificationEvent *self;

  g_return_val_if_fail (CM_IS_CLIENT (client), NULL);

  self = g_object_new (CM_TYPE_VERIFICATION_EVENT, NULL);
  self->client = g_object_ref (client);

  return self;
}

void
cm_verification_event_set_json (CmVerificationEvent *self,
                                JsonObject          *root)
{
  JsonObject *child;
  CmEventType type;

  g_return_if_fail (CM_IS_VERIFICATION_EVENT (self));
  g_return_if_fail (root);
  g_return_if_fail (self->client);
  g_return_if_fail (!self->transaction_id);

  cm_event_set_json (CM_EVENT (self), root, NULL);

  type = cm_event_get_m_type (CM_EVENT (self));

  g_return_if_fail (type >= CM_M_KEY_VERIFICATION_ACCEPT &&
                    type <= CM_M_KEY_VERIFICATION_START);

  child = cm_utils_json_object_get_object (root, "content");
  self->transaction_id = cm_utils_json_object_dup_string (child, "transaction_id");

  if (type == CM_M_KEY_VERIFICATION_KEY)
    self->verification_key = cm_utils_json_object_dup_string (child, "key");

  g_signal_connect_object (self, "updated",
                           G_CALLBACK (verification_event_updated_cb),
                           self, G_CONNECT_SWAPPED);
}

const char *
cm_verification_event_get_transaction_id (CmVerificationEvent *self)
{
  g_return_val_if_fail (CM_IS_VERIFICATION_EVENT (self), NULL);

  return self->transaction_id;
}

const char *
cm_verification_event_get_verification_key (CmVerificationEvent *self)
{
  g_return_val_if_fail (CM_IS_VERIFICATION_EVENT (self), NULL);

  return self->verification_key;
}

static void
key_verification_cancel_cb (GObject      *obj,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  CmVerificationEvent *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_VERIFICATION_EVENT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("(%p) Key verification %p cancel %s", self->client,
           self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_debug ("(%p) Key verification %p cancel error: %s", self->client, self, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      GListModel *verifications;

      verifications = cm_client_get_key_verifications (self->client);
      cm_utils_remove_list_item (G_LIST_STORE (verifications), self);

      g_task_return_boolean (task, TRUE);
    }
}

void
cm_verification_event_cancel_async (CmVerificationEvent *self,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  CmOlmSas *olm_sas = NULL;
  CmEvent *reply_event;
  g_autoptr(GTask) task = NULL;
  g_autofree char *uri = NULL;
  const char *cancel_code;
  JsonObject *root;

  g_return_if_fail (CM_IS_VERIFICATION_EVENT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (CM_IS_CLIENT (self->client));

  olm_sas = g_object_get_data (G_OBJECT (self), "olm-sas");
  g_return_if_fail (CM_IS_OLM_SAS (olm_sas));

  task = g_task_new (self, cancellable, callback, user_data);
  g_debug ("(%p) Key verification %p cancel", self->client, self);

  cancel_code = cm_olm_sas_get_cancel_code (olm_sas);
  reply_event = cm_olm_sas_get_cancel_event (olm_sas, cancel_code);
  root = cm_event_get_json (reply_event);

  uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.cancel/%s",
                         cm_event_get_txn_id (reply_event));
  cm_net_send_json_async (cm_client_get_net (self->client),
                          0, root, uri, SOUP_METHOD_PUT,
                          NULL, cancellable,
                          key_verification_cancel_cb,
                          g_steal_pointer (&task));
}

gboolean
cm_verification_event_cancel_finish (CmVerificationEvent *self,
                                     GAsyncResult        *result,
                                     GError             **error)
{
  g_return_val_if_fail (CM_IS_VERIFICATION_EVENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
verification_load_user_devices_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GPtrArray) users = NULL;
  GError *error = NULL;

  users = cm_user_list_load_devices_finish (CM_USER_LIST (object), result, &error);

  if (error)
    g_debug ("Load user devices error: %s", error->message);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, !users || users->len == 0);
}

static void
key_verification_continue_cb (GObject      *obj,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  CmVerificationEvent *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_VERIFICATION_EVENT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);

  g_debug ("(%p) Key verification %p continue %s", self->client,
           self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_debug ("(%p) Key verification continue error: %s", self, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      /* Reset ready-complete marker */
      if (cm_event_get_m_type (CM_EVENT (self)) == CM_M_KEY_VERIFICATION_REQUEST &&
          g_object_get_data (G_OBJECT (self), "start") &&
          g_object_get_data (G_OBJECT (self), "ready-complete"))
        {
          g_object_set_data (G_OBJECT (self), "ready-complete", GINT_TO_POINTER (FALSE));
          g_clear_object (&self->pending_task);
        }

      /* Cache the task, we shall complete the task once we have "start" event */
      if (cm_event_get_m_type (CM_EVENT (self)) == CM_M_KEY_VERIFICATION_REQUEST &&
          !g_object_get_data (G_OBJECT (self), "start") &&
          !g_object_get_data (G_OBJECT (self), "ready-complete"))
        {
          g_object_set_data (G_OBJECT (self), "ready-complete", GINT_TO_POINTER (TRUE));
          g_set_object (&self->pending_task, task);
        }
      else
        {
          g_autoptr(GPtrArray) users = NULL;
          CmUserList *user_list;
          CmUser *user;

          users = g_ptr_array_new_full (1, g_object_unref);
          user = cm_event_get_sender (CM_EVENT (self));;
          g_ptr_array_add (users, g_object_ref (user));

          user_list = cm_client_get_user_list (self->client);
          cm_user_list_load_devices_async (user_list, users,
                                           verification_load_user_devices_cb,
                                           g_steal_pointer (&task));
        }
    }
}

void
cm_verification_event_continue_async (CmVerificationEvent *self,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  g_autofree char *uri = NULL;
  CmOlmSas *olm_sas = NULL;
  CmEvent *reply_event;
  JsonObject *root;
  GTask *task;

  g_return_if_fail (CM_IS_VERIFICATION_EVENT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  g_debug ("(%p) Key verification %p continue", self->client, self);

  olm_sas = g_object_get_data (G_OBJECT (self), "olm-sas");
  g_return_if_fail (CM_IS_OLM_SAS (olm_sas));

  if (cm_event_get_m_type (CM_EVENT (self)) == CM_M_KEY_VERIFICATION_REQUEST &&
      (!g_object_get_data (G_OBJECT (self), "start") &&
       !g_object_get_data (G_OBJECT (self), "ready-complete")))
    {
      reply_event = cm_olm_sas_get_ready_event (olm_sas);
      root = cm_event_get_json (reply_event);
      uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.ready/%s",
                             cm_event_get_txn_id (reply_event));
      cm_net_send_json_async (cm_client_get_net (self->client),
                              0, root, uri, SOUP_METHOD_PUT,
                              NULL, cancellable,
                              key_verification_continue_cb,
                              g_steal_pointer (&task));
    }
  else
    {
      reply_event = cm_olm_sas_get_accept_event (olm_sas);
      root = cm_event_get_json (reply_event);
      uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.accept/%s",
                             cm_event_get_txn_id (reply_event));
      cm_net_send_json_async (cm_client_get_net (self->client),
                              0, root, uri, SOUP_METHOD_PUT,
                              NULL, cancellable,
                              key_verification_continue_cb,
                              g_steal_pointer (&task));
    }
}

gboolean
cm_verification_event_continue_finish (CmVerificationEvent *self,
                                       GAsyncResult        *result,
                                       GError             **error)
{
  g_return_val_if_fail (CM_IS_VERIFICATION_EVENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
mac_sent_done_cb (GObject      *obj,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  GError *error = NULL;
  gboolean success;

  g_assert (G_IS_TASK (task));

  success = g_task_propagate_boolean (G_TASK (result), &error);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, success);
}

static void
key_verification_match_cb (GObject      *obj,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  CmVerificationEvent *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_VERIFICATION_EVENT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("(%p) Key verification %p match %s", self->client,
           self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_debug ("(%p) Key verification match error: %s", self, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      g_object_set_data (G_OBJECT (self), "mac-sent", GINT_TO_POINTER (TRUE));
      if (g_object_get_data (G_OBJECT (self), "mac"))
        {
          GCancellable *cancellable;
          CmOlmSas *olm_sas;

          cancellable = g_task_get_cancellable (task);
          olm_sas = g_object_get_data (G_OBJECT (self), "olm-sas");

          if (cm_olm_sas_get_cancel_code (olm_sas))
            cm_verification_event_cancel_async (self, cancellable,
                                                mac_sent_done_cb,
                                                g_steal_pointer (&task));
          else
            cm_verification_event_done_async (self, cancellable,
                                              mac_sent_done_cb,
                                              g_steal_pointer (&task));
        }
      else
        g_task_return_boolean (task, TRUE);
    }
}

void
cm_verification_event_match_async (CmVerificationEvent *self,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_autofree char *uri = NULL;
  CmOlmSas *olm_sas = NULL;
  CmEvent *reply_event;
  JsonObject *root;
  GTask *task;

  g_return_if_fail (CM_IS_VERIFICATION_EVENT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  g_debug ("(%p) Key verification %p match", self->client, self);

  olm_sas = g_object_get_data (G_OBJECT (self), "olm-sas");
  g_return_if_fail (CM_IS_OLM_SAS (olm_sas));

  reply_event = cm_olm_sas_get_mac_event (olm_sas);
  root = cm_event_get_json (reply_event);
  uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.mac/%s",
                         cm_event_get_txn_id (reply_event));
  cm_net_send_json_async (cm_client_get_net (self->client),
                          0, root, uri, SOUP_METHOD_PUT,
                          NULL, cancellable,
                          key_verification_match_cb,
                          g_steal_pointer (&task));
}

gboolean
cm_verification_event_match_finish (CmVerificationEvent *self,
                                    GAsyncResult        *result,
                                    GError             **error)
{
  g_return_val_if_fail (CM_IS_VERIFICATION_EVENT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
key_verification_done_cb (GObject      *obj,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  CmVerificationEvent *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonObject) object = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_VERIFICATION_EVENT (self));

  object = g_task_propagate_pointer (G_TASK (result), &error);
  g_debug ("(%p) Key verification %p done %s", self->client,
           self, CM_LOG_SUCCESS (!error));

  if (error)
    {
      g_debug ("(%p) Key verification done error: %s", self, error->message);
      g_task_return_error (task, error);
    }
  else
    {
      GListModel *verifications;

      verifications = cm_client_get_key_verifications (self->client);
      cm_utils_remove_list_item (G_LIST_STORE (verifications), self);

      g_object_set_data (G_OBJECT (self), "completed", GINT_TO_POINTER (TRUE));
      g_signal_emit_by_name (self, "updated", 0);
      g_task_return_boolean (task, TRUE);
    }
}

void
cm_verification_event_done_async (CmVerificationEvent *self,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_autofree char *uri = NULL;
  CmOlmSas *olm_sas = NULL;
  CmEvent *reply_event;
  JsonObject *root;
  GTask *task;

  g_return_if_fail (CM_IS_VERIFICATION_EVENT (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);

  g_debug ("(%p) Key verification %p done", self->client, self);

  olm_sas = g_object_get_data (G_OBJECT (self), "olm-sas");
  g_return_if_fail (CM_IS_OLM_SAS (olm_sas));

  reply_event = cm_olm_sas_get_done_event (olm_sas);
  root = cm_event_get_json (reply_event);

  uri = g_strdup_printf ("/_matrix/client/r0/sendToDevice/m.key.verification.done/%s",
                         cm_event_get_txn_id (reply_event));
  cm_net_send_json_async (cm_client_get_net (self->client),
                          0, root, uri, SOUP_METHOD_PUT,
                          NULL, cancellable,
                          key_verification_done_cb,
                          g_steal_pointer (&task));
}
