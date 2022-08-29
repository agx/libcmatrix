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
  size_t length, len;

  g_return_val_if_fail (self->pickle_key, NULL);

  if (!self->session_id)
    {
      length = olm_session_id_length (self->olm_session);
      self->session_id = g_malloc (length + 1);
      olm_session_id (self->olm_session, self->session_id, length);
      self->session_id[length] = '\0';
    }

  if (self->olm_session)
    {
      len = olm_pickle_session_length (self->olm_session);
      pickle = g_malloc (len + 1);
      olm_pickle_session (self->olm_session, self->pickle_key,
                          strlen (self->pickle_key),
                          pickle, len);
    }
  else if (self->in_gp_session)
    {
      len = olm_pickle_inbound_group_session_length (self->in_gp_session);
      pickle = g_malloc (len + 1);
      olm_pickle_inbound_group_session (self->in_gp_session, self->pickle_key,
                                        strlen (self->pickle_key),
                                        pickle, len);
    }
  else
    g_return_val_if_reached (NULL);

  pickle[len] = '\0';

  return g_steal_pointer (&pickle);
}

static void
cm_olm_finalize (GObject *object)
{
  CmOlm *self = (CmOlm *)object;

  g_free (self->pickle_key);
  g_free (self->curve_key);
  g_free (self->sender_id);
  g_free (self->device_id);
  g_free (self->room_id);

  if (self->olm_session)
    olm_clear_session (self->olm_session);
  if (self->in_gp_session)
    olm_clear_inbound_group_session (self->in_gp_session);
  if (self->out_gp_session)
    olm_clear_outbound_group_session (self->out_gp_session);

  g_free (self->olm_session);
  g_free (self->in_gp_session);
  g_free (self->out_gp_session);

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
cm_olm_inbound_new (gpointer    olm_account,
                    const char *sender_identity_key,
                    const char *one_time_key_message)
{
  CmOlm *self = NULL;
  OlmSession *session = NULL;
  g_autofree char *message_copy = NULL;
  size_t err;

  message_copy = g_strdup (one_time_key_message);

  session = g_malloc (olm_session_size ());
  olm_session (session);

  err = olm_create_inbound_session_from (session, olm_account,
                                         sender_identity_key, strlen (sender_identity_key),
                                         message_copy, strlen (message_copy));
  if (err == olm_error ())
    {
      g_warning ("Error creating session: %s", olm_session_last_error (session));
      olm_clear_session (session);
      g_free (session);

      return NULL;
    }

  /* Remove one time keys that are used */
  err = olm_remove_one_time_keys (olm_account, session);
  if (err == olm_error ())
    g_warning ("Error removing key: %s", olm_account_last_error (olm_account));

  self = g_object_new (CM_TYPE_OLM, NULL);
  self->olm_session = g_steal_pointer (&session);
  self->curve_key = g_strdup (sender_identity_key);
  self->account = olm_account;

  return self;
}

CmOlm *
cm_olm_in_group_new (const char *session_key,
                     const char *sender_identity_key,
                     const char *session_id)
{
  CmOlm *self;
  g_autofree OlmInboundGroupSession *session = NULL;
  size_t err;

  session = g_malloc (olm_inbound_group_session_size ());
  olm_inbound_group_session (session);

  err = olm_init_inbound_group_session (session, (gpointer)session_key,
                                        strlen (session_key));
  if (err == olm_error ())
    {
      g_warning ("Error creating group session from key: %s",
                 olm_inbound_group_session_last_error (session));
      return NULL;
    }

  self = g_object_new (CM_TYPE_OLM, NULL);
  self->in_gp_session = g_steal_pointer (&session);
  self->curve_key = g_strdup (sender_identity_key);
  self->session_id = g_strdup (session_id);

  return self;
}

CmOlm *
cm_olm_in_group_new_from_out (CmOlm      *out_group,
                              const char *sender_identity_key)
{
  g_assert (CM_IS_OLM (out_group));
  g_assert (out_group->out_gp_session);

  return cm_olm_in_group_new (out_group->session_key,
                              sender_identity_key,
                              out_group->session_id);
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
  CmSessionType type;
  char *pickle;
  gboolean success;

  g_return_val_if_fail (CM_IS_OLM (self), FALSE);
  g_return_val_if_fail (self->cm_db, FALSE);
  g_return_val_if_fail (self->pickle_key, FALSE);
  g_return_val_if_fail (self->sender_id, FALSE);
  g_return_val_if_fail (self->device_id, FALSE);

  pickle = cm_olm_get_olm_session_pickle (self);
  g_return_val_if_fail (pickle && *pickle, FALSE);

  if (self->in_gp_session)
    type = SESSION_MEGOLM_V1_IN;
  else
    type = SESSION_OLM_V1_IN;

  task = g_task_new (self, NULL, NULL, NULL);
  cm_db_add_session_async (self->cm_db, self->sender_id, self->device_id,
                           self->room_id, self->session_id, self->curve_key,
                           pickle, type,
                           olm_task_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  success = g_task_propagate_boolean (task, &error);

  if (error)
    g_warning ("Failed to save olm session with id: %s", self->session_id);

  return success;
}

char *
cm_olm_encrypt (CmOlm      *self,
                const char *plain_text)
{
  g_autofree char *encrypted = NULL;
  cm_gcry_t random = NULL;
  size_t len, rand_len;

  g_return_val_if_fail (CM_IS_OLM (self), NULL);
  g_assert (self->olm_session);

  if (!plain_text)
    return NULL;

  rand_len = olm_encrypt_random_length (self->olm_session);
  if (rand_len)
    random = gcry_random_bytes (rand_len, GCRY_STRONG_RANDOM);

  len = olm_encrypt_message_length (self->olm_session, strlen (plain_text));
  encrypted = g_malloc (len + 1);
  len = olm_encrypt (self->olm_session, plain_text, strlen (plain_text),
                     random, rand_len, encrypted, len);
  gcry_free (random);

  if (len == olm_error ())
    {
      g_warning ("Error encrypting: %s", olm_session_last_error (self->olm_session));

      return NULL;
    }
  encrypted[len] = '\0';

  return g_steal_pointer (&encrypted);
}

char *
cm_olm_decrypt (CmOlm      *self,
                size_t      type,
                const char *message)
{
  g_autofree char *plaintext = NULL;
  g_autofree char *copy = NULL;
  size_t len;

  g_assert (CM_IS_OLM (self));
  g_return_val_if_fail (message, NULL);

  copy = g_strdup (message);
  len = olm_decrypt_max_plaintext_length (self->olm_session, type, copy, strlen (copy));

  if (len == olm_error ())
    {
      g_warning ("Error getting max length: %s", olm_session_last_error (self->olm_session));

      return NULL;
    }

  g_free (copy);
  copy = g_strdup (message);
  plaintext = g_malloc (len + 1);
  len = olm_decrypt (self->olm_session, type, copy, strlen (copy), plaintext, len);

  if (len == olm_error ())
    {
      g_warning ("Error decrypting: %s", olm_session_last_error (self->olm_session));

      return NULL;
    }

  plaintext[len] = '\0';

  return g_steal_pointer (&plaintext);
}

size_t
cm_olm_get_message_type (CmOlm *self)
{
  g_assert (CM_IS_OLM (self));
  g_assert (self->olm_session);

  return olm_encrypt_message_type (self->olm_session);
}

const char *
cm_olm_get_session_id (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  if (!self->session_id && self->olm_session)
    {
      size_t len;

      len = olm_session_id_length (self->olm_session);
      self->session_id = g_malloc (len + 1);
      olm_session_id (self->olm_session, self->session_id, len);
      self->session_id[len] = '\0';
    }

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
  CmOlm *self = NULL;
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

      self = g_object_new (CM_TYPE_OLM, NULL);
      self->olm_session = g_steal_pointer (&session);
      return self;
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

              self = g_object_new (CM_TYPE_OLM, NULL);
              self->olm_session = g_steal_pointer (&session);
              return self;
            }
        }

      g_clear_pointer (&body_copy, g_free);
    }

 end:
  olm_clear_session (session);

  return NULL;
}
