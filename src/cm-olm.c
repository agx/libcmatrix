/* cm-olm.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-olm"

#include "cm-config.h"

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
  GRefString              *sender_id;
  char                    *device_id;
  char                    *account_user_id;
  char                    *account_device_id;
  OlmAccount              *account;

  char                    *curve_key;
  char                    *pickle_key;
  char                    *session_id;
  char                    *session_key;
  uint8_t                 *current_session_key;
  OlmInboundGroupSession  *in_gp_session;
  OlmOutboundGroupSession *out_gp_session;
  OlmSession              *olm_session;

  gint64                   created_time;
  CmSessionType            type;
  CmOlmState               state;
};

G_DEFINE_TYPE (CmOlm, cm_olm, G_TYPE_OBJECT)


static char *
cm_olm_get_olm_session_pickle (CmOlm *self)
{
  g_autofree char *pickle = NULL;
  size_t len;

  g_return_val_if_fail (self->pickle_key, NULL);

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
  else if (self->out_gp_session)
    {
      len = olm_pickle_outbound_group_session_length (self->out_gp_session);
      pickle = g_malloc (len + 1);
      olm_pickle_outbound_group_session (self->out_gp_session, self->pickle_key,
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
  g_clear_pointer (&self->sender_id, g_ref_string_release);
  g_free (self->device_id);
  g_free (self->room_id);

  g_clear_pointer (&self->account_user_id, g_ref_string_release);
  g_free (self->account_device_id);

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

CmOlm *
cm_olm_new_from_pickle (char          *pickle,
                        const char    *pickle_key,
                        const char    *sender_identity_key,
                        CmSessionType  session_type)
{
  CmOlm *self;
  g_autofree OlmInboundGroupSession *in_session = NULL;
  g_autofree OlmOutboundGroupSession *out_session = NULL;
  g_autofree OlmSession *session = NULL;
  g_autofree uint8_t *session_key = NULL;
  size_t err, len;

  if (session_type == SESSION_MEGOLM_V1_IN)
    {
      in_session = g_malloc (olm_inbound_group_session_size ());
      err = olm_unpickle_inbound_group_session (in_session, pickle_key,
                                                strlen (pickle_key),
                                                pickle, strlen (pickle));
      if (err == olm_error ())
        {
          g_debug ("Error in group unpickle: %s",
                   olm_inbound_group_session_last_error (in_session));

          return NULL;
        }
    }
  else if (session_type == SESSION_MEGOLM_V1_OUT)
    {
      out_session = g_malloc (olm_outbound_group_session_size ());
      err = olm_unpickle_outbound_group_session (out_session, pickle_key,
                                                 strlen (pickle_key),
                                                 pickle, strlen (pickle));
      if (err == olm_error ())
        {
          g_debug ("Error in group unpickle: %s",
                   olm_outbound_group_session_last_error (out_session));

          return NULL;
        }

      len = olm_outbound_group_session_key_length (out_session);
      session_key = g_malloc (len + 1);
      len = olm_outbound_group_session_key (out_session, session_key, len);
      if (len == olm_error ())
        {
          g_warning ("Error getting session key: %s",
                     olm_outbound_group_session_last_error (out_session));

          return NULL;
        }
      session_key[len] = '\0';
    }
  else
    {
      session = g_malloc (olm_session_size ());
      olm_session (session);
      err = olm_unpickle_session (session, pickle_key, strlen (pickle_key),
                                  pickle, strlen (pickle));
      if (err == olm_error ())
        return NULL;
    }

  self = g_object_new (CM_TYPE_OLM, NULL);
  self->olm_session = g_steal_pointer (&session);
  self->in_gp_session = g_steal_pointer (&in_session);
  self->out_gp_session = g_steal_pointer (&out_session);
  self->session_key = (char *)g_steal_pointer (&session_key);
  self->curve_key = g_strdup (sender_identity_key);
  self->pickle_key = g_strdup (pickle_key);
  self->type = session_type;

  return self;
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
  self->type = SESSION_OLM_V1_OUT;
  /* time in milliseconds */
  self->created_time = time (NULL) * 1000;

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
  self->type = SESSION_OLM_V1_IN;

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
  self->session_key = g_strdup (session_key);
  self->type = SESSION_MEGOLM_V1_IN;

  return self;
}

CmOlm *
cm_olm_in_group_new_from_out (CmOlm      *out_group,
                              const char *sender_identity_key)
{
  CmOlm *self;

  g_assert (CM_IS_OLM (out_group));
  g_assert (out_group->out_gp_session);

  self = cm_olm_in_group_new (out_group->session_key,
                              sender_identity_key,
                              out_group->session_id);
  cm_olm_set_account_details (self, out_group->account_user_id,
                              out_group->account_device_id);
  cm_olm_set_sender_details (self, out_group->room_id, out_group->sender_id);
  cm_olm_set_key (self, out_group->pickle_key);
  cm_olm_set_db (self, out_group->cm_db);
  self->created_time = out_group->created_time;

  return self;
}

CmOlm *
cm_olm_out_group_new (const char *sender_identity_key)
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
  self->curve_key = g_strdup (sender_identity_key);
  self->out_gp_session = g_steal_pointer (&session);
  self->session_id = (char *)g_steal_pointer (&session_id);
  self->session_key = (char *)g_steal_pointer (&session_key);
  self->created_time = time (NULL) * 1000;
  self->type = SESSION_MEGOLM_V1_OUT;

  return self;
}

CmSessionType
cm_olm_get_session_type (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), 0);

  return self->type;
}

size_t
cm_olm_get_message_index (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), 0);
  g_return_val_if_fail (self->out_gp_session, 0);

  return olm_outbound_group_session_message_index (self->out_gp_session);
}

gint64
cm_olm_get_created_time (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), 0);

  return self->created_time;
}

/**
 * cm_olm_set_state:
 * @self: A #CmOlm
 * @state: A #CmOlmState
 *
 * Set the usability state of @self.
 */
void
cm_olm_set_state (CmOlm      *self,
                  CmOlmState  state)
{
  g_return_if_fail (CM_IS_OLM (self));

  if (state == self->state)
    return;

  g_return_if_fail (self->state == OLM_STATE_USABLE);
  self->state = state;
}

CmOlmState
cm_olm_get_state (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), OLM_STATE_NOT_SET);

  return self->state;
}

void
cm_olm_update_validity (CmOlm  *self,
                        guint   count,
                        gint64  duration)
{
  g_return_if_fail (CM_IS_OLM (self));
  g_return_if_fail (count);
  g_return_if_fail (duration > 0);

  if (cm_olm_get_message_index (self) >= count ||
      cm_olm_get_created_time (self) + duration <= time (NULL) * 1000)
    cm_olm_set_state (self, OLM_STATE_ROTATED);
}

void
cm_olm_set_sender_details (CmOlm      *self,
                           const char *room_id,
                           GRefString *sender_id)
{
  g_return_if_fail (CM_IS_OLM (self));
  g_return_if_fail (sender_id && *sender_id == '@');
  g_return_if_fail (!self->sender_id);

  self->room_id = g_strdup (room_id);
  self->sender_id = g_ref_string_acquire (sender_id);
}

void
cm_olm_set_account_details (CmOlm      *self,
                            GRefString *account_user_id,
                            const char *account_device_id)
{
  g_return_if_fail (CM_IS_OLM (self));
  g_return_if_fail (account_user_id && *account_user_id == '@');
  g_return_if_fail (account_device_id && *account_device_id);
  g_return_if_fail (!self->account_user_id);
  g_return_if_fail (!self->account_device_id);

  self->account_user_id = g_ref_string_acquire (account_user_id);
  self->account_device_id = g_strdup (account_device_id);
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
  char *pickle;

  g_return_val_if_fail (CM_IS_OLM (self), FALSE);
  g_return_val_if_fail (self->cm_db, FALSE);
  g_return_val_if_fail (self->pickle_key, FALSE);
  g_return_val_if_fail (self->account_user_id, FALSE);
  g_return_val_if_fail (self->account_device_id, FALSE);

  pickle = cm_olm_get_olm_session_pickle (self);
  g_return_val_if_fail (pickle && *pickle, FALSE);

  return cm_db_add_session (self->cm_db, self, pickle);
}

char *
cm_olm_encrypt (CmOlm      *self,
                const char *plain_text)
{
  g_autofree char *encrypted = NULL;
  size_t len;

  g_return_val_if_fail (CM_IS_OLM (self), NULL);
  g_assert (self->olm_session || self->out_gp_session);

  if (!plain_text)
    return NULL;

  if (self->olm_session)
    {
      cm_gcry_t random = NULL;
      size_t rand_len;

      rand_len = olm_encrypt_random_length (self->olm_session);
      if (rand_len)
        random = gcry_random_bytes (rand_len, GCRY_STRONG_RANDOM);

      len = olm_encrypt_message_length (self->olm_session, strlen (plain_text));
      encrypted = g_malloc (len + 1);
      len = olm_encrypt (self->olm_session, plain_text, strlen (plain_text),
                         random, rand_len, encrypted, len);
      gcry_free (random);
    }
  else if (self->out_gp_session)
    {
      len = olm_group_encrypt_message_length (self->out_gp_session, strlen (plain_text));
      encrypted = g_malloc (len + 1);
      len = olm_group_encrypt (self->out_gp_session,
                               (gpointer)plain_text, strlen (plain_text),
                               (gpointer)encrypted, len);
    }
  else
    g_return_val_if_reached (NULL);

  if (len == olm_error ())
    {
      const char *error = NULL;

      if (self->olm_session)
        error = olm_session_last_error (self->olm_session);
      else if (self->out_gp_session)
        error = olm_outbound_group_session_last_error (self->out_gp_session);

      if (error)
        g_warning ("Error encrypting: %s", error);

      return NULL;
    }
  encrypted[len] = '\0';

  return g_steal_pointer (&encrypted);
}

static char *
session_decrypt (CmOlm      *self,
                 size_t      type,
                 const char *ciphertext)
{
  g_autofree char *plaintext = NULL;
  char *copy;
  size_t len;

  g_assert (CM_IS_OLM (self));
  g_assert (self->olm_session);

  copy = g_strdup (ciphertext);
  len = olm_decrypt_max_plaintext_length (self->olm_session,
                                          type, copy, strlen (copy));
  g_free (copy);

  if (len == olm_error ())
    {
      g_warning ("Error getting max length: %s",
                 olm_session_last_error (self->olm_session));

      return NULL;
    }

  copy = g_strdup (ciphertext);
  plaintext = g_malloc (len + 1);
  len = olm_decrypt (self->olm_session, type, copy,
                     strlen (copy), plaintext, len);
  g_free (copy);

  if (len == olm_error ())
    {
      g_warning ("Error decrypting: %s",
                 olm_session_last_error (self->olm_session));

      return NULL;
    }

  plaintext[len] = '\0';

  return g_steal_pointer (&plaintext);
}

static char *
group_session_decrypt (CmOlm      *self,
                       const char *ciphertext)
{
  g_autofree char *plaintext = NULL;
  char *copy;
  size_t len;

  g_assert (CM_IS_OLM (self));
  g_assert (self->in_gp_session);

  copy = g_strdup (ciphertext);
  len = olm_group_decrypt_max_plaintext_length (self->in_gp_session,
                                                (gpointer)copy, strlen (copy));
  g_free (copy);

  plaintext = g_malloc (len + 1);
  copy = g_strdup (ciphertext);
  len = olm_group_decrypt (self->in_gp_session, (gpointer)copy, strlen (copy),
                           (gpointer)plaintext, len, NULL);
  g_free (copy);

  if (len == olm_error ())
    {
      g_warning ("Error decrypting: %s",
                 olm_inbound_group_session_last_error (self->in_gp_session));
      return NULL;
    }

  plaintext[len] = '\0';

  return g_steal_pointer (&plaintext);
}

char *
cm_olm_decrypt (CmOlm      *self,
                size_t      type,
                const char *message)
{
  g_assert (CM_IS_OLM (self));
  g_return_val_if_fail (message, NULL);

  if (self->olm_session)
    return session_decrypt (self, type, message);

  if (self->in_gp_session)
    return group_session_decrypt (self, message);

  return NULL;
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
  size_t len;

  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  if (!self->session_id)
    {
      void *session_id = NULL;

      if (self->olm_session)
        {
          len = olm_session_id_length (self->olm_session);
          session_id = g_malloc (len + 1);
          olm_session_id (self->olm_session, session_id, len);
        }
      else if (self->out_gp_session)
        {
          len = olm_outbound_group_session_id_length (self->out_gp_session);
          session_id = g_malloc (len + 1);
          olm_outbound_group_session_id (self->out_gp_session, session_id, len);
        }
      else if (self->in_gp_session)
        {
          len = olm_inbound_group_session_id_length (self->in_gp_session);
          session_id = g_malloc (len + 1);
          olm_inbound_group_session_id (self->in_gp_session, session_id, len);
        }

      if (session_id)
        ((char *)session_id)[len] = '\0';
      self->session_id = session_id;
    }

  return self->session_id;
}

/**
 * cm_olm_get_session_key:
 * @self: A #CmOlm of type %SESSION_MEGOLM_V1_OUT
 *
 * Get the session for the next message to be sent.
 * The session key shall change after a message sent
 *
 * The session key can be used to decrypt future
 * messages, but not the past ones.
 *
 * Returns: The session key string
 */
const char *
cm_olm_get_session_key (CmOlm *self)
{
  size_t len;

  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  /* Each message is sent with a different ratchet key.  So regenerate the
   * so that ratchet key that will be used for the next message shall be
   * returned.  We want let the other party see only the messages sent
   * after this session key, not the past ones.
  */
  cm_utils_free_buffer ((char *)self->current_session_key);
  self->current_session_key = NULL;

  if (self->out_gp_session)
    {
      len = olm_outbound_group_session_key_length (self->out_gp_session);
      self->current_session_key = g_malloc (len + 1);
      olm_outbound_group_session_key (self->out_gp_session,
                                      self->current_session_key, len);
      self->current_session_key[len] = '\0';
    }

  return (char *)self->current_session_key;
}

const char *
cm_olm_get_room_id (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  return self->room_id;
}

const char *
cm_olm_get_sender_key (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  return self->curve_key;
}

GRefString *
cm_olm_get_account_id (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  return self->account_user_id;
}

const char *
cm_olm_get_account_device (CmOlm *self)
{
  g_return_val_if_fail (CM_IS_OLM (self), NULL);

  return self->account_device_id;
}

gpointer
cm_olm_match_olm_session (const char     *body,
                          gsize           body_len,
                          size_t          message_type,
                          const char     *pickle,
                          const char     *pickle_key,
                          const char     *sender_identify_key,
                          CmSessionType   session_type,
                          char          **out_decrypted)
{
  g_autoptr(CmOlm) self = NULL;
  g_autofree char *pickle_copy = NULL;

  g_assert (out_decrypted);

  pickle_copy = g_strdup (pickle);
  self = cm_olm_new_from_pickle (pickle_copy, pickle_key, sender_identify_key, session_type);

  if (!self)
    return NULL;

  /* If it's a pre key message, check if the session matches */
  if (message_type == OLM_MESSAGE_TYPE_PRE_KEY)
    {
      g_autofree char *body_copy = NULL;
      size_t match = 0;

      body_copy = g_malloc (body_len + 1);
      memcpy (body_copy, body, body_len + 1);
      match = olm_matches_inbound_session (self->olm_session, body_copy, body_len);
      /* If it doesn't match, don't consider using it for decryption */
      if (match != 1)
        return NULL;
    }

  /* Try decrypting with the given session */
  *out_decrypted = cm_olm_decrypt (self, message_type, body);

  if (*out_decrypted)
    return g_steal_pointer (&self);

  return NULL;
}
