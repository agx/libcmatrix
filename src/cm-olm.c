/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-olm.c
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
#include <olm/olm.h>

#include "cm-utils-private.h"
#include "cm-db-private.h"
#include "cm-olm-private.h"

struct _CmOlm
{
  GObject                  parent_instance;

  CmDb                    *cm_db;
  char                    *room_id;
  char                    *sender_id;
  char                    *device_id;
  OlmAccount              *account;

  char                    *curve_key;
  char                    *pickle_key;
  char                    *session_id;
  char                    *session_key;
  OlmInboundGroupSession  *in_gp_session;
  OlmOutboundGroupSession *out_gp_session;
  OlmSession              *olm_session;

  CmSessionType            type;
};

G_DEFINE_TYPE (CmOlm, cm_olm, G_TYPE_OBJECT)


static void
olm_task_bool_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GTask *task = user_data;
  GError *error = NULL;
  gboolean status;

  g_assert_true (G_IS_TASK (task));

  status = g_task_propagate_boolean (G_TASK (result), &error);
  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, status);
}


static char *
cm_olm_get_olm_session_pickle (CmOlm *self)
{
  g_autofree char *pickle = NULL;
  size_t length;

  g_return_val_if_fail (self->pickle_key, NULL);

  if (!self->session_id)
    {
      length = olm_session_id_length (self->olm_session);
      self->session_id = g_malloc (length + 1);
      olm_session_id (self->olm_session, self->session_id, length);
      self->session_id[length] = '\0';
    }

  length = olm_pickle_session_length (self->olm_session);
  pickle = g_malloc (length + 1);
  olm_pickle_session (self->olm_session, self->pickle_key,
                      strlen (self->pickle_key),
                      pickle, length);
  pickle[length] = '\0';

  return g_steal_pointer (&pickle);
}

static void
cm_olm_finalize (GObject *object)
{
  CmOlm *self = (CmOlm *)object;

  g_free (self->pickle_key);
  g_free (self->session_id);
  g_free (self->curve_key);
  g_free (self->sender_id);
  g_free (self->device_id);
  g_free (self->room_id);

  cm_utils_free_buffer (self->session_key);
  cm_utils_free_buffer (self->session_id);

  g_clear_object (&self->cm_db);

  G_OBJECT_CLASS (cm_olm_parent_class)->finalize (object);
}

static void
cm_olm_class_init (CmOlmClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_olm_finalize;
}

static void
cm_olm_init (CmOlm *self)
{
}

gpointer
cm_olm_steal_session (CmOlm         *self,
                      CmSessionType  type)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  if ((type == SESSION_OLM_V1_IN ||
       type == SESSION_OLM_V1_OUT) &&
      self->olm_session)
    return g_steal_pointer (&self->olm_session);

  if (type == SESSION_MEGOLM_V1_IN &&
      self->in_gp_session)
    return g_steal_pointer (&self->in_gp_session);

  if (type == SESSION_MEGOLM_V1_OUT &&
      self->out_gp_session)
    return g_steal_pointer (&self->out_gp_session);

  return NULL;
}

CmOlm *
cm_olm_outbound_new (gpointer    olm_account,
                     const char *curve_key,
                     const char *one_time_key,
                     const char *room_id)
{
  CmOlm *self;
  g_autofree OlmSession *session = NULL;
  cm_gcry_t buffer = NULL;
  size_t length, error;

  g_return_val_if_fail (olm_account, NULL);

  if (!curve_key || !one_time_key)
    return NULL;

  session = g_malloc (olm_session_size ());
  olm_session (session);

  length = olm_create_outbound_session_random_length (session);
  if (length)
    buffer = gcry_random_bytes (length, GCRY_STRONG_RANDOM);

  error = olm_create_outbound_session (session,
                                       olm_account,
                                       curve_key, strlen (curve_key),
                                       one_time_key, strlen (one_time_key),
                                       buffer, length);
  gcry_free (buffer);

  if (error == olm_error ())
    {
      g_warning ("Error creating outbound olm session: %s",
                 olm_session_last_error (session));
      return NULL;
    }

  self = g_object_new (CM_TYPE_OLM, NULL);
  self->olm_session = g_steal_pointer (&session);
  self->curve_key = g_strdup (curve_key);
  self->account = olm_account;

  return self;
}

CmOlm *
cm_olm_out_group_new (void)
{
  CmOlm *self;
  g_autofree OlmOutboundGroupSession *session = NULL;
  g_autofree uint8_t *session_key = NULL;
  g_autofree uint8_t *session_id = NULL;
  uint8_t *random = NULL;
  size_t length, error;

  /* Initialize session */
  session = g_malloc (olm_outbound_group_session_size ());
  olm_outbound_group_session (session);

  /* Feed in random bits */
  length = olm_init_outbound_group_session_random_length (session);
  if (length)
    random = gcry_random_bytes (length, GCRY_STRONG_RANDOM);
  error = olm_init_outbound_group_session (session, random, length);
  gcry_free (random);

  if (error == olm_error ())
    {
      g_warning ("Error init out group session: %s", olm_outbound_group_session_last_error (session));

      return NULL;
    }

  /* Get session id */
  length = olm_outbound_group_session_id_length (session);
  session_id = g_malloc (length + 1);
  length = olm_outbound_group_session_id (session, session_id, length);
  if (length == olm_error ())
    {
      g_warning ("Error getting session id: %s", olm_outbound_group_session_last_error (session));

      return NULL;
    }
  session_id[length] = '\0';

  /* Get session key */
  length = olm_outbound_group_session_key_length (session);
  session_key = g_malloc (length + 1);
  length = olm_outbound_group_session_key (session, session_key, length);
  if (length == olm_error ())
    {
      g_warning ("Error getting session key: %s", olm_outbound_group_session_last_error (session));

      return NULL;
    }
  session_key[length] = '\0';

  self = g_object_new (CM_TYPE_OLM, NULL);
  self->out_gp_session = g_steal_pointer (&session);
  self->session_id = (char *)g_steal_pointer (&session_id);
  self->session_key = (char *)g_steal_pointer (&session_key);

  /*
   * We should also create an inbound session with the same key so
   * that we we'll be able to decrypt the messages we sent (when
   * we receive them via sync requests)
   */
  {
    g_autofree char *session_key_copy = NULL;

    session_key_copy = g_strdup (self->session_key);
    self->in_gp_session = g_malloc (olm_inbound_group_session_size ());
    olm_inbound_group_session (self->in_gp_session);
    olm_init_inbound_group_session (self->in_gp_session,
                                    (gpointer)session_key_copy,
                                    strlen (session_key_copy));
  }

  return self;
}

void
cm_olm_set_details (CmOlm      *self,
                    const char *room_id,
                    const char *sender_id,
                    const char *device_id)
{
  g_return_if_fail (CM_IS_OLM (self));
  g_return_if_fail (sender_id);
  g_return_if_fail (device_id);
  g_return_if_fail (!self->sender_id);
  g_return_if_fail (!self->device_id);

  self->room_id = g_strdup (room_id);
  self->sender_id = g_strdup (sender_id);
  self->device_id = g_strdup (device_id);
}

void
cm_olm_set_db (CmOlm    *self,
               gpointer  cm_db)
{
  g_return_if_fail (CM_IS_OLM (self));
  g_return_if_fail (CM_IS_DB (cm_db));
  g_return_if_fail (!self->cm_db);

  self->cm_db = g_object_ref (cm_db);
}

void
cm_olm_set_key (CmOlm      *self,
                const char *key)
{
  g_return_if_fail (CM_IS_OLM (self));
  g_return_if_fail (key && *key);
  g_return_if_fail (!self->pickle_key);

  self->pickle_key = g_strdup (key);
}

gboolean
cm_olm_save (CmOlm *self)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  char *pickle;
  gboolean success;

  g_return_val_if_fail (CM_IS_OLM (self), FALSE);
  g_return_val_if_fail (self->cm_db, FALSE);
  g_return_val_if_fail (self->pickle_key, FALSE);
  g_return_val_if_fail (self->sender_id, FALSE);
  g_return_val_if_fail (self->device_id, FALSE);

  pickle = cm_olm_get_olm_session_pickle (self);
  g_return_val_if_fail (pickle && *pickle, FALSE);

  task = g_task_new (self, NULL, NULL, NULL);
  cm_db_add_session_async (self->cm_db, self->sender_id, self->device_id,
                           self->room_id, self->session_id, self->curve_key,
                           pickle, SESSION_OLM_V1_IN,
                           olm_task_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  success = g_task_propagate_boolean (task, &error);

  if (error)
    g_warning ("Failed to save olm session with id: %s", self->session_id);

  return success;
}

const char *
cm_olm_get_session_id (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  return self->session_id;
}

const char *
cm_olm_get_session_key (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  return self->session_key;
}

gpointer
cm_olm_match_olm_session (const char  *body,
                          gsize        body_len,
                          size_t       message_type,
                          const char  *pickle,
                          const char  *pickle_key,
                          char       **out_decrypted)
{
  g_autofree char *body_copy = NULL;
  g_autofree char *pickle_copy = NULL;
  g_autofree OlmSession *session = NULL;
  g_autofree char *plaintext = NULL;
  size_t match = 0, length, err;

  g_assert (out_decrypted);

  session = g_malloc (olm_session_size ());
  olm_session (session);
  pickle_copy = g_strdup (pickle);
  err = olm_unpickle_session (session, pickle_key, strlen (pickle_key),
                              pickle_copy, strlen (pickle_copy));
  if (err == olm_error ())
    goto end;

  body_copy = g_malloc (body_len + 1);
  memcpy (body_copy, body, body_len + 1);
  length = olm_decrypt_max_plaintext_length (session, message_type, body_copy, body_len);
  g_clear_pointer (&body_copy, g_free);

  plaintext = g_malloc (length + 1);
  if (length != olm_error ())
    {
      body_copy = g_malloc (body_len + 1);
      memcpy (body_copy, body, body_len + 1);
      length = olm_decrypt (session, message_type, body_copy, body_len, plaintext, length);
      g_clear_pointer (&body_copy, g_free);
    }

  if (length != olm_error ())
    {
      plaintext[length] = '\0';
      *out_decrypted = g_steal_pointer (&plaintext);

      return g_steal_pointer (&session);
    }

  if (message_type == OLM_MESSAGE_TYPE_PRE_KEY)
    {
      body_copy = g_malloc (body_len + 1);
      memcpy (body_copy, body, body_len + 1);
      match = olm_matches_inbound_session (session, body_copy, body_len);
      if (match == 1)
        {
          length = olm_decrypt (session, message_type, body_copy, body_len, plaintext, length);

          if (length != olm_error ())
            {
              plaintext[length] = '\0';
              *out_decrypted = g_steal_pointer (&plaintext);

              return g_steal_pointer (&session);
            }
        }

      g_clear_pointer (&body_copy, g_free);
    }

 end:
  olm_clear_session (session);

  return NULL;
}
