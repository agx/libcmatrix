/* cm-enc.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-enc"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <json-glib/json-glib.h>
#include <olm/olm.h>
#include <sys/random.h>

#include "cm-utils-private.h"
#include "users/cm-user-private.h"
#include "users/cm-user-list-private.h"
#include "users/cm-room-member-private.h"
#include "cm-room-private.h"
#include "cm-device.h"
#include "cm-device-private.h"
#include "cm-db-private.h"
#include "cm-olm-private.h"
#include "cm-olm-sas-private.h"
#include "cm-enc-private.h"

#define KEY_LABEL_SIZE    6
#define STRING_ALLOCATION 512

/*
 * SECTION: cm-enc
 * @title: CmEnc
 * @short_description: An abstraction for E2EE
 * @include: "cm-enc.h"
 */

/*
 * Documentations:
 *   https://matrix.org/docs/guides/end-to-end-encryption-implementation-guide
 *
 * Other:
 *  * We use g_malloc(size) instead of g_malloc(size * sizeof(type)) for all ‘char’
 *    and ‘[u]int8_t’, unless there is a possibility that the type can change.
 */
struct _CmEnc
{
  GObject parent_instance;

  CmDb       *cm_db;

  OlmAccount *account;
  OlmUtility *utility;
  char       *pickle_key;

  /* FIXME: is there a way to limit hashtable entries to-say-1000,
   * and when new items are added older ones are deleted?
   * Or any other data structure with fast lookup?
   */
  GHashTable *enc_files;
  GHashTable *in_olm_sessions;
  GHashTable *out_olm_sessions;
  GHashTable *in_group_sessions;
  GHashTable *out_group_sessions;
  GHashTable *out_group_room_session;

  GRefString *user_id;
  char *device_id;

  char *curve_key; /* Public part of Curve25519 identity key */
  char *ed_key;    /* Public part of Ed25519 fingerprint key */
};

G_DEFINE_TYPE (CmEnc, cm_enc, G_TYPE_OBJECT)

static void
free_all_details (CmEnc *self)
{
  if (self->account)
    olm_clear_account (self->account);

  g_clear_pointer (&self->account, g_free);
  g_hash_table_remove_all (self->in_olm_sessions);
  g_hash_table_remove_all (self->out_olm_sessions);
  g_hash_table_remove_all (self->in_group_sessions);
  g_hash_table_remove_all (self->out_group_sessions);
  g_hash_table_remove_all (self->out_group_room_session);
}

static CmOlm *
ma_enc_lookup_out_group_session (CmEnc       *self,
                                 CmRoom      *room,
                                 const char **out_session_id)
{
  const char *session_id;

  g_assert (CM_IS_ENC (self));
  g_assert (CM_IS_ROOM (room));

  session_id = g_hash_table_lookup (self->out_group_room_session, room);
  if (!session_id)
    return NULL;

  if (out_session_id)
    *out_session_id = session_id;

  return g_hash_table_lookup (self->out_group_sessions, session_id);
}

static CmOlm *
ma_create_olm_out_session (CmEnc      *self,
                           const char *curve_key,
                           const char *one_time_key,
                           const char *room_id)
{
  CmOlm *session;

  g_assert (CM_ENC (self));

  session = cm_olm_outbound_new (self->account, curve_key, one_time_key, room_id);

  if (!session)
    return NULL;

  cm_olm_set_db (session, self->cm_db);
  cm_olm_set_key (session, self->pickle_key);
  cm_olm_set_sender_details (session, room_id, self->user_id);
  cm_olm_set_account_details (session, self->user_id, self->device_id);
  cm_olm_save (session);

  return session;
}

/*
 * cm_enc_load_identity_keys:
 * @self: A #CmEnc
 *
 * Load the public part of Ed25519 fingerprint
 * key pair and Curve25519 identity key pair.
 */
static gboolean
cm_enc_load_identity_keys (CmEnc *self)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *key = NULL;
  JsonObject *object;
  JsonNode *node;
  size_t length, err;

  length = olm_account_identity_keys_length (self->account);
  key = malloc (length + 1);
  err = olm_account_identity_keys (self->account, key, length);
  key[length] = '\0';

  if (err == olm_error ())
    {
      g_warning ("error getting identity keys: %s", olm_account_last_error (self->account));
      return FALSE;
    }

  parser = json_parser_new ();
  json_parser_load_from_data (parser, key, length, &error);

  if (error)
    {
      g_warning ("error parsing keys: %s", error->message);
      return FALSE;
    }

  node = json_parser_get_root (parser);
  object = json_node_get_object (node);

  g_free (self->curve_key);
  g_free (self->ed_key);

  self->curve_key = g_strdup (json_object_get_string_member (object, "curve25519"));
  self->ed_key = g_strdup (json_object_get_string_member (object, "ed25519"));

  return TRUE;
}

static void
create_new_details (CmEnc *self)
{
  char *pickle_key;
  cm_gcry_t buffer;
  size_t length, err;

  g_assert (CM_ENC (self));

  g_debug ("(%p) Creating new encryption keys", self);

  free_all_details (self);

  self->account = g_malloc (olm_account_size ());
  olm_account (self->account);

  gcry_free (self->pickle_key);
  buffer = gcry_random_bytes_secure (64, GCRY_STRONG_RANDOM);
  pickle_key = g_base64_encode (buffer, 64);
  gcry_free (buffer);

  /* Copy and free pickle_key as it's not an mlock() memory */
  self->pickle_key = gcry_malloc_secure (strlen (pickle_key) + 1);
  strcpy (self->pickle_key, pickle_key);
  cm_utils_free_buffer (pickle_key);

  length = olm_create_account_random_length (self->account);
  if (length)
    buffer = gcry_random_bytes (length, GCRY_STRONG_RANDOM);
  err = olm_create_account (self->account, buffer, length);
  gcry_free (buffer);
  if (err == olm_error ())
    g_warning ("Error creating account: %s", olm_account_last_error (self->account));
}

static void
cm_enc_sign_json_object (CmEnc      *self,
                         JsonObject *object)
{
  g_autoptr(GString) str = NULL;
  g_autofree char *signature = NULL;
  g_autofree char *label = NULL;
  JsonObject *sign, *child;

  g_assert (CM_IS_ENC (self));
  g_assert (object);

  /* The JSON is in canonical form.  Required for signing */
  /* https://matrix.org/docs/spec/appendices#signing-json */
  str = cm_utils_json_get_canonical (object, NULL);
  signature = cm_enc_sign_string (self, str->str, str->len);

  sign = json_object_new ();
  label = g_strconcat ("ed25519:", self->device_id, NULL);
  json_object_set_string_member (sign, label, signature);

  child = json_object_new ();
  json_object_set_object_member (child, self->user_id, sign);
  json_object_set_object_member (object, "signatures", child);
}

static void
cm_enc_finalize (GObject *object)
{
  CmEnc *self = (CmEnc *)object;

  olm_clear_account (self->account);
  g_free (self->account);

  olm_clear_utility (self->utility);
  g_free (self->utility);

  g_hash_table_unref (self->enc_files);
  g_hash_table_unref (self->in_olm_sessions);
  g_hash_table_unref (self->out_olm_sessions);
  g_hash_table_unref (self->in_group_sessions);
  g_hash_table_unref (self->out_group_sessions);
  g_hash_table_unref (self->out_group_room_session);

  g_clear_pointer (&self->user_id, g_ref_string_release);
  g_free (self->device_id);
  gcry_free (self->pickle_key);
  cm_utils_free_buffer (self->curve_key);
  cm_utils_free_buffer (self->ed_key);
  g_clear_object (&self->cm_db);

  G_OBJECT_CLASS (cm_enc_parent_class)->finalize (object);
}


static void
cm_enc_class_init (CmEncClass *klass)
{
  GObjectClass *object_class  = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_enc_finalize;
}


static void
cm_enc_init (CmEnc *self)
{
  self->utility = g_malloc (olm_utility_size ());
  olm_utility (self->utility);

  self->enc_files = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, cm_enc_file_info_free);
  /* We use hashtable of hashtables, each value of hashtable indexed with
     sender's curve25519 key */
  /* in_olm_sessions = g_hash_table_new (g_str_hash, g_str_equal, */
  /*                                     g_free, free_olm_session); */
  self->in_olm_sessions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                 (GDestroyNotify)g_hash_table_unref);
  self->out_olm_sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, g_object_unref);
  self->in_group_sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free, g_object_unref);
  self->out_group_sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, g_object_unref);
  self->out_group_room_session = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                        g_object_unref, g_free);
}

/**
 * cm_enc_new:
 * @pickle: (nullable): The account pickle
 * @key: @pickle key, can be %NULL if @pickle is %NULL
 *
 * If @pickle is non-null, the olm account is created
 * using the pickled data.  Otherwise a new olm account
 * is created. If @pickle is non-null and invalid
 * %NULL is returned.
 *
 * For @self to be ready for use, the details of @self
 * should be set with cm_enc_set_details().
 *
 * Also see cm_enc_get_pickle().
 *
 * Returns: (transfer full) (nullable): A new #CmEnc.
 * Free with g_object_unref()
 */
CmEnc *
cm_enc_new (gpointer    matrix_db,
            const char *pickle,
            const char *key)
{
  g_autoptr(CmEnc) self = NULL;

  g_return_val_if_fail (!pickle || (*pickle && key && *key), NULL);

  self = g_object_new (CM_TYPE_ENC, NULL);
  g_set_object (&self->cm_db, matrix_db);

  /* Deserialize the pickle to create the account */
  if (pickle && *pickle)
    {
      g_autofree char *duped = NULL;
      size_t err;

      g_debug ("(%p) Create from pickle", self);
      self->pickle_key = gcry_malloc_secure (strlen (pickle) + 1);
      strcpy (self->pickle_key, key);
      self->account = g_malloc (olm_account_size ());
      olm_account (self->account);

      duped = g_strdup (pickle);
      err = olm_unpickle_account (self->account, key, strlen (key),
                                  duped, strlen (duped));

      if (err == olm_error ())
        {
          g_warning ("Error account unpickle: %s", olm_account_last_error (self->account));
          return NULL;
        }
    }
  else
    {
      create_new_details (self);
    }

  if (!cm_enc_load_identity_keys (self))
    return NULL;

  return g_steal_pointer (&self);
}

gpointer
cm_enc_get_sas_for_event (CmEnc               *self,
                          CmVerificationEvent *event)
{
  CmOlmSas *sas;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);
  g_return_val_if_fail (CM_IS_EVENT (event), NULL);

  sas = g_object_get_data (G_OBJECT (event), "olm-sas");

  if (sas)
    return sas;

  sas = cm_olm_sas_new ();
  cm_olm_sas_set_key_verification (sas, event);
  g_object_set_data_full (G_OBJECT (event), "olm-sas", sas, g_object_unref);

  return sas;
}

/**
 * cm_enc_set_details:
 * @self: A #CmEnc
 * @user_id: (nullable): Fully qualified Matrix user ID
 * @device_id: (nullable): The device id string
 *
 * Set user id and device id of @self.  @user_id
 * should be fully qualified Matrix user ID
 * (ie, @user:example.com)
 */
void
cm_enc_set_details (CmEnc      *self,
                    GRefString *user_id,
                    const char *device_id)
{
  g_autoptr(GRefString) old_user = NULL;
  g_autofree char *old_device = NULL;

  g_return_if_fail (CM_IS_ENC (self));
  g_return_if_fail (!user_id || *user_id == '@');

  old_user = g_steal_pointer (&self->user_id);
  old_device = self->device_id;

  if (user_id)
    self->user_id = g_ref_string_acquire (user_id);
  self->device_id = g_strdup (device_id);

  if (self->user_id && old_device &&
      g_strcmp0 (device_id, old_device) == 0)
    {
      create_new_details (self);
      cm_enc_load_identity_keys (self);
    }
}

char *
cm_enc_get_pickle (CmEnc *self)
{
  g_autofree char *pickle = NULL;
  size_t length, err;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  length = olm_pickle_account_length (self->account);
  pickle = malloc (length + 1);
  err = olm_pickle_account (self->account, self->pickle_key,
                            strlen (self->pickle_key), pickle, length);
  pickle[length] = '\0';

  if (err == olm_error ())
    {
      g_warning ("Error getting account pickle: %s", olm_account_last_error (self->account));

      return NULL;
    }

  return g_steal_pointer (&pickle);
}

char *
cm_enc_get_pickle_key (CmEnc *self)
{
  g_return_val_if_fail (CM_ENC (self), NULL);

  return g_strdup (self->pickle_key);
}

/**
 * cm_enc_sign_string:
 * @self: A #CmEnc
 * @str: A string to sign
 * @len: The length of @str, or -1
 *
 * Sign @str and return the signature.
 * Returns %NULL on error.
 *
 * Returns: (transfer full): The signature string.
 * Free with g_free()
 */
char *
cm_enc_sign_string (CmEnc  *self,
                    const char *str,
                    size_t      len)
{
  char *signature;
  size_t length, err;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);
  g_return_val_if_fail (str, NULL);
  g_return_val_if_fail (*str, NULL);

  if (len == (size_t) -1)
    len = strlen (str);

  length = olm_account_signature_length (self->account);
  signature = malloc (length + 1);
  err = olm_account_sign (self->account, str, len, signature, length);
  signature[length] = '\0';

  if (err == olm_error ())
    {
      g_warning ("Error signing data: %s", olm_account_last_error (self->account));

      return NULL;
    }

  return signature;
}

/**
 * cm_enc_verify:
 * @self: A #CmEnc
 * @object: A #JsonObject
 * @matrix_id: A Fully qualified Matrix ID
 * @device_id: The device id string.
 * @ed_key: The ED25519 key of @matrix_id
 *
 * Verify if the content in @object is signed by
 * the user @matrix_id with device @device_id.
 *
 * This function may modify @object by removing
 * "signatures" and "unsigned" members.
 *
 * Returns; %TRUE if verification succeeded.  Or
 * %FALSE otherwise.
 */
gboolean
cm_enc_verify (CmEnc      *self,
               JsonObject *object,
               const char *matrix_id,
               const char *device_id,
               const char *ed_key)
{
  JsonNode *signatures, *non_signed;
  g_autoptr(GString) json_str = NULL;
  g_autofree char *signature = NULL;
  g_autofree char *key_name = NULL;
  JsonObject *child;
  size_t error;

  if (!object)
    return FALSE;

  g_return_val_if_fail (CM_IS_ENC (self), FALSE);
  g_return_val_if_fail (matrix_id && *matrix_id == '@', FALSE);
  g_return_val_if_fail (device_id && *device_id, FALSE);
  g_return_val_if_fail (ed_key && *ed_key, FALSE);

  /* https://matrix.org/docs/spec/appendices#checking-for-a-signature */
  key_name = g_strconcat ("ed25519:", device_id, NULL);
  child = cm_utils_json_object_get_object (object, "signatures");
  child = cm_utils_json_object_get_object (child, matrix_id);
  signature = g_strdup (cm_utils_json_object_get_string (child, key_name));

  if (!signature)
    return FALSE;

  signatures = json_object_dup_member (object, "signatures");
  non_signed = json_object_dup_member (object, "signatures");
  /* Remove the non signed members before verification */
  json_object_remove_member (object, "signatures");
  json_object_remove_member (object, "unsigned");

  json_str = cm_utils_json_get_canonical (object, NULL);

  /* Revert the changes we made to the JSON object */
  if (signatures)
    json_object_set_member (object, "signatures", signatures);
  if (non_signed)
    json_object_set_member (object, "unsigned", non_signed);

  error = olm_ed25519_verify (self->utility,
                              ed_key, strlen (ed_key),
                              json_str->str, json_str->len,
                              signature, strlen (signature));

  if (error == olm_error ())
    {
      g_debug ("Error verifying signature: %s", olm_utility_last_error (self->utility));
      return FALSE;
    }

  return TRUE;
}

/**
 * cm_enc_max_one_time_keys:
 * @self: A #CmEnc
 *
 * Get the maximum number of one time keys Olm
 * library can handle.
 *
 * Returns: The number of maximum one-time keys.
 */
size_t
cm_enc_max_one_time_keys (CmEnc *self)
{
  g_return_val_if_fail (CM_IS_ENC (self), 0);

  return olm_account_max_number_of_one_time_keys (self->account);
}

/**
 * cm_enc_create_one_time_keys:
 * @self: A #CmEnc
 * @count: A non-zero number
 *
 * Generate @count number of curve25519 one time keys.
 * @count is capped to the half of what Olm library
 * can handle.
 *
 * Returns: The number of one-time keys generated.
 * It will be <= @count.
 */
size_t
cm_enc_create_one_time_keys (CmEnc  *self,
                             size_t  count)
{
  cm_gcry_t buffer = NULL;
  size_t length, err;

  g_return_val_if_fail (CM_IS_ENC (self), 0);
  g_return_val_if_fail (count, 0);

  /* doc: The maximum number of active keys supported by libolm
     is returned by olm_account_max_number_of_one_time_keys.
     The client should try to maintain about half this number on the homeserver. */
  count = MIN (count, olm_account_max_number_of_one_time_keys (self->account) / 2);

  length = olm_account_generate_one_time_keys_random_length (self->account, count);
  if (length)
    buffer = gcry_random_bytes (length, GCRY_STRONG_RANDOM);
  err = olm_account_generate_one_time_keys (self->account, count, buffer, length);

  if (err == olm_error ())
    {
      g_warning ("Error creating one time keys: %s", olm_account_last_error (self->account));

      return 0;
    }

  return count;
}

/**
 * cm_enc_publish_one_time_keys:
 * @self: A #CmEnc
 *
 * Mark current set of one-time keys as published,
 * So that they won't be returned again when requested
 * with cm_enc_get_one_time_keys() or so.
 */
void
cm_enc_publish_one_time_keys (CmEnc *self)
{
  g_return_if_fail (CM_IS_ENC (self));

  olm_account_mark_keys_as_published (self->account);
}

/**
 * cm_enc_get_one_time_keys:
 * @self: A #CmEnc
 *
 * Get public part of unpublished Curve25519 one-time keys in @self.
 *
 * The returned data is a JSON-formatted object with the single
 * property curve25519, which is itself an object mapping key id
 * to base64-encoded Curve25519 key. For example:
 *
 * {
 *     "curve25519": {
 *         "AAAAAA": "wo76WcYtb0Vk/pBOdmduiGJ0wIEjW4IBMbbQn7aSnTo",
 *         "AAAAAB": "LRvjo46L1X2vx69sS9QNFD29HWulxrmW11Up5AfAjgU"
 *     }
 * }
 *
 * Returns: (nullable) (transfer full): The unpublished one time keys.
 * Free with g_free()
 */
JsonObject *
cm_enc_get_one_time_keys (CmEnc *self)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *buffer = NULL;
  size_t length, err;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  length = olm_account_one_time_keys_length (self->account);
  buffer = g_malloc (length + 1);
  err = olm_account_one_time_keys (self->account, buffer, length);
  buffer[length] = '\0';

  if (err == olm_error ())
    {
      g_warning ("Error getting one time keys: %s", olm_account_last_error (self->account));

      return NULL;
    }

  /* Return NULL if there are no keys */
  if (g_str_equal (buffer, "{\"curve25519\":{}}"))
    return NULL;

  parser = json_parser_new ();
  json_parser_load_from_data (parser, buffer, length, &error);

  if (error)
    {
      g_warning ("error parsing keys: %s", error->message);
      return NULL;
    }

  return json_node_dup_object (json_parser_get_root (parser));
}

/**
 * cm_enc_get_one_time_keys_json:
 * @self: A #CmEnc
 *
 * Get the signed Curve25519 one-time keys JSON.  The JSON shall
 * be in the following format:
 *
 * {
 *   "signed_curve25519:AAAAHg": {
 *     "key": "zKbLg+NrIjpnagy+pIY6uPL4ZwEG2v+8F9lmgsnlZzs",
 *     "signatures": {
 *       "@alice:example.com": {
 *         "ed25519:JLAFKJWSCS": "FLWxXqGbwrb8SM3Y795eB6OA8bwBcoMZFXBqnTn58AYWZSqiD45tlBVcDa2L7RwdKXebW/VzDlnfVJ+9jok1Bw"
 *       }
 *     }
 *   },
 *   "signed_curve25519:AAAAHQ": {
 *     "key": "j3fR3HemM16M7CWhoI4Sk5ZsdmdfQHsKL1xuSft6MSw",
 *     "signatures": {
 *       "@alice:example.com": {
 *         "ed25519:JLAFKJWSCS": "IQeCEPb9HFk217cU9kw9EOiusC6kMIkoIRnbnfOh5Oc63S1ghgyjShBGpu34blQomoalCyXWyhaaT3MrLZYQAA"
 *       }
 *     }
 *   }
 * }
 *
 * Returns: (transfer full): A JSON encoded string.
 * Free with g_free()
 */
char *
cm_enc_get_one_time_keys_json (CmEnc *self)
{
  g_autoptr(JsonObject) object = NULL;
  g_autoptr(JsonObject) root = NULL;
  g_autoptr(GList) members = NULL;
  JsonObject *keys, *child;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  object = cm_enc_get_one_time_keys (self);

  if (!object)
    return NULL;

  keys = json_object_new ();
  object = json_object_get_object_member (object, "curve25519");
  members = json_object_get_members (object);

  for (GList *item = members; item; item = item->next)
    {
      g_autofree char *label = NULL;
      const char *value;

      child = json_object_new ();
      value = json_object_get_string_member (object, item->data);
      json_object_set_string_member (child, "key", value);
      cm_enc_sign_json_object (self, child);

      label = g_strconcat ("signed_curve25519:", item->data, NULL);
      json_object_set_object_member (keys, label, child);
    }

  root = json_object_new ();
  json_object_set_object_member (root, "one_time_keys", keys);

  return cm_utils_json_object_to_string (root, FALSE);
}

/**
 * cm_enc_get_device_keys_json:
 * @self: A #CmEnc
 *
 * Get the signed device key JSON.  The JSON shall
 * be in the following format:
 *
 * {
 *   "user_id": "@alice:example.com",
 *   "device_id": "JLAFKJWSCS",
 *   "algorithms": [
 *     "m.olm.curve25519-aes-sha256",
 *     "m.megolm.v1.aes-sha2"
 *   ],
 *   "keys": {
 *     "curve25519:JLAFKJWSCS": "3C5BFWi2Y8MaVvjM8M22DBmh24PmgR0nPvJOIArzgyI",
 *     "ed25519:JLAFKJWSCS": "lEuiRJBit0IG6nUf5pUzWTUEsRVVe/HJkoKuEww9ULI"
 *   },
 *   "signatures": {
 *     "@alice:example.com": {
 *       "ed25519:JLAFKJWSCS": "dSO80A01XiigH3uBiDVx/EjzaoycHcjq9lfQX0uWsqxl2giMIiSPR8a4d291W1ihKJL/a+myXS367WT6NAIcBA"
 *     }
 *   }
 * }
 *
 * Returns: (nullable): A JSON encoded string.
 * Free with g_free()
 */
char *
cm_enc_get_device_keys_json (CmEnc *self)
{
  g_autoptr(JsonObject) root = NULL;
  JsonObject *keys, *device_keys;
  JsonArray *array;
  char *label;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);
  g_return_val_if_fail (self->user_id, NULL);
  g_return_val_if_fail (self->device_id, NULL);

  device_keys = json_object_new ();
  json_object_set_string_member (device_keys, "user_id", self->user_id);
  json_object_set_string_member (device_keys, "device_id", self->device_id);

  array = json_array_new ();
  json_array_add_string_element (array, ALGORITHM_OLM);
  json_array_add_string_element (array, ALGORITHM_MEGOLM);
  json_object_set_array_member (device_keys, "algorithms", array);

  keys = json_object_new ();

  label = g_strconcat ("curve25519:", self->device_id, NULL);
  json_object_set_string_member (keys, label, self->curve_key);
  g_free (label);

  label = g_strconcat ("ed25519:", self->device_id, NULL);
  json_object_set_string_member (keys, label, self->ed_key);
  g_free (label);

  json_object_set_object_member (device_keys, "keys", keys);
  cm_enc_sign_json_object (self, device_keys);

  root = json_object_new ();
  json_object_set_object_member (root, "device_keys", device_keys);

  return cm_utils_json_object_to_string (root, FALSE);
}

static gboolean
in_olm_matches (gpointer key,
                gpointer value,
                gpointer user_data)
{
  g_autofree char *body = NULL;
  size_t match;

  body = g_strdup (user_data);
  match = olm_matches_inbound_session (value, body, strlen (body));

  if (match == olm_error ())
    g_warning ("Error matching inbound session: %s", olm_session_last_error (key));

  return match == 1;
}

static void
handle_m_room_key (CmEnc      *self,
                   JsonObject *root,
                   const char *sender_key)
{
  CmOlm *session;
  JsonObject *object;
  const char *session_key, *session_id, *room_id;

  g_assert (CM_IS_ENC (self));
  g_assert (root);

  object = cm_utils_json_object_get_object (root, "content");
  session_key = cm_utils_json_object_get_string (object, "session_key");
  session_id = cm_utils_json_object_get_string (object, "session_id");
  room_id = cm_utils_json_object_get_string (object, "room_id");

  /* The documentation recommends to look if the session already exists */
  if (!session_key ||
      g_hash_table_lookup (self->in_group_sessions, session_id))
    return;

  session = cm_olm_in_group_new (session_key, sender_key, session_id);
  g_debug ("(%p) Create new in group olm session %p", self, session);
  cm_olm_set_sender_details (session, room_id, self->user_id);
  cm_olm_set_account_details (session, self->user_id, self->device_id);
  cm_olm_set_key (session, self->pickle_key);
  cm_olm_set_db (session, self->cm_db);
  cm_olm_save (session);
  g_hash_table_insert (self->in_group_sessions, g_strdup (session_id), session);
}

void
cm_enc_handle_room_encrypted (CmEnc      *self,
                              JsonObject *object)
{
  g_autoptr(GRefString) sender = NULL;
  const char *algorithm, *sender_key;
  g_autofree char *plaintext = NULL;
  g_autofree char *body = NULL;
  CmOlm *session = NULL;
  size_t type;
  gboolean force_save = FALSE;

  g_return_if_fail (CM_IS_ENC (self));
  g_return_if_fail (object);

  if (cm_utils_json_object_get_string (object, "sender"))
    sender = g_ref_string_new_intern (cm_utils_json_object_get_string (object, "sender"));
  object = cm_utils_json_object_get_object (object, "content");
  algorithm = cm_utils_json_object_get_string (object, "algorithm");
  /* sender_key is the Curve25519 identity key of the sender */
  sender_key = cm_utils_json_object_get_string (object, "sender_key");

  if (!algorithm || !sender_key || !sender)
    g_return_if_reached ();

  if (!g_str_equal (algorithm, ALGORITHM_MEGOLM) &&
      !g_str_equal (algorithm, ALGORITHM_OLM))
    g_return_if_reached ();

  object = cm_utils_json_object_get_object (object, "ciphertext");
  object = cm_utils_json_object_get_object (object, self->curve_key);

  body = g_strdup (cm_utils_json_object_get_string (object, "body"));
  type = (size_t)cm_utils_json_object_get_int (object, "type");

  if (!body)
    return;

  if (self->cm_db)
    session = cm_db_lookup_olm_session (self->cm_db, self->user_id, self->device_id,
                                        sender_key, body, self->pickle_key,
                                        SESSION_OLM_V1_IN, type, &plaintext);

  if (!session && type == OLM_MESSAGE_TYPE_MESSAGE)
    session = cm_db_lookup_olm_session (self->cm_db, self->user_id, self->device_id,
                                        sender_key, body, self->pickle_key,
                                        SESSION_OLM_V1_OUT, type, &plaintext);

  if (!session && type == OLM_MESSAGE_TYPE_PRE_KEY)
    {
      GHashTable *in_olm_sessions;

      in_olm_sessions = g_hash_table_lookup (self->in_olm_sessions, sender_key);
      if (in_olm_sessions)
        session = g_hash_table_find (in_olm_sessions, in_olm_matches, body);
      g_debug ("(%p) Message with pre-key received, has session: %p", self, session);

      if (!session)
        {
          session = cm_olm_inbound_new (self->account, sender_key, body);
          g_debug ("(%p) New inbound session created %p", self, session);
          cm_olm_set_db (session, self->cm_db);
          cm_olm_set_key (session, self->pickle_key);

          force_save = TRUE;
        }
    }

  g_debug ("(%p) Handle decrypted, session: %p", self, session);

  if (!session)
    return;

  if (!plaintext)
    plaintext = cm_olm_decrypt (session, type, body);

  {
    g_autoptr(JsonObject) content = NULL;
    JsonObject *data;
    const char *message_type;

    content = cm_utils_string_to_json_object (plaintext);
    message_type = cm_utils_json_object_get_string (content, "type");

    g_debug ("(%p) Message decrypted. type: %s", self, message_type);

    if (g_strcmp0 (sender, cm_utils_json_object_get_string (content, "sender")) != 0)
      {
        g_warning ("(%p) Sender mismatch in encrypted content", self);
        return;
      }

    /* The content is not meant for us */
    if (g_strcmp0 (self->user_id, cm_utils_json_object_get_string (content, "recipient")) != 0)
      return;

    data = cm_utils_json_object_get_object (content, "recipient_keys");
    if (g_strcmp0 (self->ed_key, cm_utils_json_object_get_string (data, "ed25519")) != 0)
      {
        g_warning ("(%p) ed25519 in content doesn't match to ours", self);
        return;
      }

    if (force_save)
      {
        GHashTable *in_olm_sessions;
        const char *id, *room_id;

        data = cm_utils_json_object_get_object (content, "content");
        room_id = cm_utils_json_object_get_string (data, "room_id");
        in_olm_sessions = g_hash_table_lookup (self->in_olm_sessions, sender_key);

        if (!in_olm_sessions)
          {
            in_olm_sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, g_object_unref);
            g_hash_table_insert (self->in_olm_sessions, g_strdup (sender_key),
                                 in_olm_sessions);
          }

        g_debug ("(%p) Save in olm session %p", self, session);

        id = cm_olm_get_session_id (session);
        g_hash_table_insert (in_olm_sessions, g_strdup (id), session);
        cm_olm_set_sender_details (session, room_id, sender);
        cm_olm_set_account_details (session, self->user_id, self->device_id);
        cm_olm_save (session);
      }

    if (g_strcmp0 (message_type, "m.room_key") == 0)
      handle_m_room_key (self, content, sender_key);
  }
}

static CmEncFileInfo *
cm_enc_get_json_file_enc_info (JsonObject *root)
{
  g_autoptr(CmEncFileInfo) file = NULL;
  JsonObject *child;

  if (!root)
    return NULL;

  file = g_new0 (CmEncFileInfo, 1);
  file->mxc_uri = g_strdup (cm_utils_json_object_get_string (root, "url"));
  file->version = g_strdup (cm_utils_json_object_get_string (root, "v"));
  file->aes_iv_base64 = g_strdup (cm_utils_json_object_get_string (root, "iv"));

  child = cm_utils_json_object_get_object (root, "hashes");
  file->sha256_base64 = g_strdup (cm_utils_json_object_get_string (child, "sha256"));

  child = cm_utils_json_object_get_object (root, "key");
  file->algorithm = g_strdup (cm_utils_json_object_get_string (child, "alg"));
  file->extractable = cm_utils_json_object_get_bool (child, "ext");
  file->kty = g_strdup (cm_utils_json_object_get_string (child, "kty"));
  file->aes_key_base64 = g_strdup (cm_utils_json_object_get_string (child, "k"));

  if (file->mxc_uri && g_str_has_prefix (file->mxc_uri, "mxc://") &&
      file->aes_key_base64)
    return g_steal_pointer (&file);

  return NULL;
}

static void
cm_enc_save_file_enc (CmEnc      *self,
                      const char *json_str)
{
  g_autoptr(JsonObject) root = NULL;
  CmEncFileInfo *file_info;
  JsonObject *child;

  g_assert (CM_IS_ENC (self));

  root = cm_utils_string_to_json_object (json_str);

  if (!root)
    return;

  child = cm_utils_json_object_get_object (root, "content");
  child = cm_utils_json_object_get_object (child, "file");
  file_info = cm_enc_get_json_file_enc_info (child);

  if (file_info && file_info->mxc_uri &&
      !g_hash_table_contains (self->enc_files, file_info->mxc_uri))
    {
      g_debug ("(%p) Save file keys", self);
      g_hash_table_insert (self->enc_files, g_strdup (file_info->mxc_uri), file_info);
      cm_db_save_file_enc_async (self->cm_db, file_info, NULL, NULL);
    }

  /* todo: handle encrypted thumbnails */
}

char *
cm_enc_handle_join_room_encrypted (CmEnc      *self,
                                   CmRoom     *room,
                                   JsonObject *object)
{
  CmOlm *session = NULL;
  const char *sender_key;
  const char *ciphertext, *session_id;
  g_autofree char *plaintext = NULL;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);
  g_return_val_if_fail (object, NULL);

  sender_key = cm_utils_json_object_get_string (object, "sender_key");

  ciphertext = cm_utils_json_object_get_string (object, "ciphertext");
  session_id = cm_utils_json_object_get_string (object, "session_id");

  /* the ciphertext can be absent, eg: in redacted events */
  if (!ciphertext)
    return NULL;

  if (session_id)
    session = g_hash_table_lookup (self->in_group_sessions, session_id);

  g_debug ("(%p) Got room encrypted, room: %p. session: %p", self, room, session);

  if (!session && self->cm_db)
    {
      session = cm_db_lookup_session (self->cm_db, self->user_id,
                                      self->device_id, session_id,
                                      sender_key, self->pickle_key,
                                      cm_room_get_id (room),
                                      SESSION_MEGOLM_V1_IN);

      g_debug ("(%p) Got in group session %p from matrix db", self, session);

      if (session)
        g_hash_table_insert (self->in_group_sessions, g_strdup (session_id), session);
    }

  if (!session)
    return NULL;

  g_return_val_if_fail (session, NULL);

  plaintext = cm_olm_decrypt (session, 0, ciphertext);

  if (strstr (plaintext, "\"key_ops\""))
    cm_enc_save_file_enc (self, plaintext);

  return g_steal_pointer (&plaintext);
}

JsonObject *
cm_enc_encrypt_for_chat (CmEnc      *self,
                         CmRoom     *room,
                         const char *message)
{
  CmOlm *session;
  g_autofree char *encrypted = NULL;
  const char *session_id;
  JsonObject *root;

  g_return_val_if_fail (CM_IS_ENC (self), NULL);
  g_return_val_if_fail (CM_IS_ROOM (room), NULL);
  g_return_val_if_fail (message && *message, NULL);

  session = ma_enc_lookup_out_group_session (self, room, NULL);
  g_return_val_if_fail (session, NULL);

  encrypted = cm_olm_encrypt (session, message);
  g_debug ("(%p) Enrypt for room %p, session: %p, chain-index: %zu",
           self, room, session,
           cm_olm_get_message_index (session));

  cm_olm_update_validity (session,
                          cm_room_get_encryption_msg_count (room),
                          cm_room_get_encryption_rotation_time (room));
  cm_olm_save (session);
  session_id = cm_olm_get_session_id (session);

  root = json_object_new ();
  json_object_set_string_member (root, "algorithm", ALGORITHM_MEGOLM);
  json_object_set_string_member (root, "sender_key", self->curve_key);
  json_object_set_string_member (root, "ciphertext", encrypted);
  json_object_set_string_member (root, "session_id", session_id);
  json_object_set_string_member (root, "device_id", self->device_id);

  return root;
}

JsonObject *
cm_enc_create_out_group_keys (CmEnc      *self,
                              CmRoom     *room,
                              GPtrArray  *one_time_keys,
                              gpointer   *out_session)
{
  CmOlm *session = NULL;
  const char *session_key, *session_id;
  JsonObject *root, *child;

  g_return_val_if_fail (CM_IS_ENC (self), FALSE);
  g_return_val_if_fail (CM_IS_ROOM (room), FALSE);
  g_return_val_if_fail (one_time_keys && one_time_keys->len, NULL);
  g_return_val_if_fail (out_session, NULL);

  session = ma_enc_lookup_out_group_session (self, room, NULL);

  if (session)
    {
      g_object_ref (session);
    }
  else
    {
      session = cm_olm_out_group_new (self->curve_key);
      cm_olm_set_account_details (session, self->user_id, self->device_id);
      cm_olm_set_sender_details (session, cm_room_get_id (room), self->user_id);
      cm_olm_set_key (session, self->pickle_key);
      cm_olm_set_db (session, self->cm_db);
      g_debug ("(%p) Create out group keys, room: %p, session: %p", self, room, session);
    }

  if (!session)
    g_return_val_if_reached (NULL);

  session_id = cm_olm_get_session_id (session);
  session_key = cm_olm_get_session_key (session);
  *out_session = session;

  root = json_object_new ();

  /* https://matrix.org/docs/spec/client_server/r0.6.1#m-room-key */
  for (guint i = 0; i < one_time_keys->len; i++)
    {
      CmUser *member;
      const char *curve_key;
      JsonObject *user;
      CmUserKey *key;

      key = one_time_keys->pdata[i];
      member = key->user;

      user = json_object_new ();
      json_object_set_object_member (root, cm_user_get_id (member), user);

      for (guint j = 0; j < key->devices->len; j++)
        {
          CmDevice *device;
          CmOlm *olm_session = NULL;
          char *one_time_key = NULL;
          JsonObject *content;

          device = key->devices->pdata[j];
          curve_key = cm_device_get_curve_key (device);

          one_time_key = key->keys->pdata[j];
          olm_session = ma_create_olm_out_session (self, curve_key, one_time_key,
                                                   cm_room_get_id (room));

          if (!one_time_key || !curve_key || !olm_session)
            continue;

          /* xxx: Do we want to store only the keys */
          /* g_hash_table_insert (self->out_olm_sessions, g_strdup (curve_key), olm_session); */

          /* Create per device object */
          child = json_object_new ();
          json_object_set_object_member (user, cm_device_get_id (device), child);

          json_object_set_string_member (child, "algorithm", ALGORITHM_OLM);
          json_object_set_string_member (child, "sender_key", self->curve_key);
          json_object_set_object_member (child, "ciphertext", json_object_new ());

          content = json_object_new ();
          child = json_object_get_object_member (child, "ciphertext");
          g_assert (child);
          json_object_set_object_member (child, curve_key, content);

          /* Body to be encrypted */
          {
            g_autoptr(JsonObject) object = NULL;
            g_autofree char *encrypted = NULL;
            g_autofree char *data = NULL;

            /* Create a json object with common data */
            object = json_object_new ();
            json_object_set_string_member (object, "type", "m.room_key");
            json_object_set_string_member (object, "sender", self->user_id);
            json_object_set_string_member (object, "sender_device", self->device_id);

            child = json_object_new ();
            json_object_set_string_member (child, "ed25519", self->ed_key);
            json_object_set_object_member (object, "keys", child);

            child = json_object_new ();
            json_object_set_string_member (child, "algorithm", "m.megolm.v1.aes-sha2");
            json_object_set_string_member (child, "room_id", cm_room_get_id (room));
            json_object_set_string_member (child, "session_id", session_id);
            json_object_set_string_member (child, "session_key", session_key);
            json_object_set_int_member (child, "chain_index", cm_olm_get_message_index (session));
            json_object_set_object_member (object, "content", child);

            /* User specific data */
            json_object_set_string_member (object, "recipient", cm_user_get_id (member));

            /* Device specific data */
            child = json_object_new ();
            json_object_set_string_member (child, "ed25519", cm_device_get_ed_key (device));
            json_object_set_object_member (object, "recipient_keys", child);

            /* Now encrypt the above JSON */
            data = cm_utils_json_object_to_string (object, FALSE);
            encrypted = cm_olm_encrypt (olm_session, data);

            /* Add the encrypted data as the content */
            json_object_set_int_member (content, "type", cm_olm_get_message_type (olm_session));
            json_object_set_string_member (content, "body", encrypted);
          }
        }
    }

  return root;
}

/**
 * cm_enc_has_room_group_key:
 * @self: A #CmEnc
 * @room_id: A matrix room id
 *
 * Check if any valid outgoing session is present
 * for the given room @room_id.  This should be
 * checked before sending each message as @self
 * may rotate the key after certain count or time
 *
 * Returns: %TRUE if a valid group session is
 * present.  %FALSE otherwise.
 */
gboolean
cm_enc_has_room_group_key (CmEnc  *self,
                           CmRoom *room)
{
  CmOlm *session;
  const char *session_id = NULL;

  g_return_val_if_fail (CM_IS_ENC (self), FALSE);
  g_return_val_if_fail (CM_IS_ROOM (room), FALSE);

  session = ma_enc_lookup_out_group_session (self, room, &session_id);

    if (!session_id && self->cm_db &&
        !g_object_get_data (G_OBJECT (room), "olm-checked"))
      {
        session = cm_db_lookup_session (self->cm_db, self->user_id,
                                        self->device_id, NULL,
                                        self->curve_key, self->pickle_key,
                                        cm_room_get_id (room),
                                        SESSION_MEGOLM_V1_OUT);

        g_object_set_data (G_OBJECT (room), "olm-checked", GINT_TO_POINTER (TRUE));
        g_debug ("(%p) Got out group session %p from matrix db", self, session);

        if (session)
          {
            CmOlm *in_session;

            cm_olm_set_db (session, self->cm_db);
            cm_olm_set_sender_details (session, cm_room_get_id (room), self->user_id);
            cm_olm_set_account_details (session, self->user_id, self->device_id);

            session_id = cm_olm_get_session_id (session);

            g_hash_table_insert (self->out_group_room_session,
                                 g_object_ref (room), g_strdup (session_id));
            g_hash_table_insert (self->out_group_sessions,
                                 g_strdup (session_id), g_object_ref (session));

            in_session = cm_olm_in_group_new_from_out (session, self->curve_key);
            g_hash_table_insert (self->in_group_sessions,
                                 g_strdup (session_id), in_session);
          }
      }

  return !!session;
}

/**
 * cm_enc_set_room_group_key:
 * @self: A #CmEnc
 * @room_id: The room id the session should be added to
 * @out_session: A megolm #CmOlm
 *
 * Set the outgoing group encryption session for the room
 * @room_id.  All future messages shall be encrypted with
 * @out_session for the given room until it's invalidated
 */
void
cm_enc_set_room_group_key (CmEnc    *self,
                           CmRoom   *room,
                           gpointer  out_session)
{
  CmOlm *in_session = NULL;
  const char *session_id;

  g_return_if_fail (CM_IS_ENC (self));
  g_return_if_fail (CM_IS_ROOM (room));
  g_return_if_fail (CM_IS_OLM (out_session));
  g_return_if_fail (cm_olm_get_session_type (out_session) == SESSION_MEGOLM_V1_OUT);

  if (ma_enc_lookup_out_group_session (self, room, NULL) == out_session)
    return;

  /* There should be no existing sessions for the room */
  g_warn_if_fail (!g_hash_table_contains (self->out_group_room_session, room));

  g_debug ("(%p) Set out group key, room: %p, session: %p", self, room, out_session);

  session_id = cm_olm_get_session_id (out_session);
  in_session = cm_olm_in_group_new_from_out (out_session, self->curve_key);
  g_hash_table_insert (self->out_group_room_session,
                       g_object_ref (room), g_strdup (session_id));
  g_hash_table_insert (self->out_group_sessions,
                       g_strdup (session_id), g_object_ref (out_session));
  g_hash_table_insert (self->in_group_sessions,
                       g_strdup (session_id), in_session);
  cm_olm_save (out_session);
  cm_olm_save (in_session);
}

/**
 * cm_enc_rm_room_group_key:
 * @self: A #CmEnc
 * @room_id: The room id for which the session should be removed
 *
 * Invalidate any out group session for the given room.
 * The in session pair of the same is not removed as it
 * may be useful to decrypt yet to receive messages.
 */
void
cm_enc_rm_room_group_key (CmEnc  *self,
                          CmRoom *room)
{
  g_autoptr(CmOlm) session = NULL;
  const char *session_id = NULL;

  g_return_if_fail (CM_IS_ENC (self));
  g_return_if_fail (CM_IS_ROOM (room));

  session = ma_enc_lookup_out_group_session (self, room, &session_id);
  g_debug ("(%p) Remove out group key, room: %p, session: %p", self, room, session);

  if (session)
    {
      g_object_ref (session);
      cm_olm_set_state (session, OLM_STATE_INVALIDATED);
      g_hash_table_remove (self->out_group_sessions, session_id);
      cm_olm_save (session);
    }

  g_hash_table_remove (self->out_group_room_session, room);
}

static void
enc_find_file_enc_cb (GObject      *obj,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  CmEnc *self;
  g_autoptr(GTask) task = user_data;
  CmEncFileInfo *file;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);

  file = cm_db_find_file_enc_finish (CM_DB (obj), result, NULL);
  g_debug ("(%p) Find file key done, has key: %s", self, CM_LOG_SUCCESS (!!file));

  g_task_return_pointer (task, file, cm_enc_file_info_free);
}

void
cm_enc_find_file_enc_async (CmEnc               *self,
                            const char          *uri,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  CmEncFileInfo *file;

  g_return_if_fail (uri && *uri);

  task = g_task_new (self, NULL, callback, user_data);
  file = g_hash_table_lookup (self->enc_files, uri);
  g_debug ("(%p) Find file key", self);

  if (file)
    {
      g_debug ("(%p) Find file key %s from cache", self, CM_LOG_SUCCESS (TRUE));
      g_task_return_pointer (task, file, NULL);
      return;
    }

  cm_db_find_file_enc_async (self->cm_db, uri,
                             enc_find_file_enc_cb,
                             g_steal_pointer (&task));
}

CmEncFileInfo *
cm_enc_find_file_enc_finish (CmEnc         *self,
                             GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (CM_IS_ENC (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

void
cm_enc_file_info_free (gpointer data)
{
  CmEncFileInfo *file = data;

  if (!file)
    return;

  cm_utils_free_buffer (file->aes_iv_base64);
  cm_utils_free_buffer (file->aes_key_base64);
  cm_utils_free_buffer (file->sha256_base64);

  cm_utils_free_buffer (file->mxc_uri);
  cm_utils_free_buffer (file->algorithm);
  cm_utils_free_buffer (file->version);
  cm_utils_free_buffer (file->kty);

  g_free (file);
}

GRefString *
cm_enc_get_user_id (CmEnc *self)
{
  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  return self->user_id;
}

const char *
cm_enc_get_device_id (CmEnc *self)
{
  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  return self->device_id;
}

const char *
cm_enc_get_curve25519_key (CmEnc *self)
{
  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  return self->curve_key;
}

const char *
cm_enc_get_ed25519_key (CmEnc *self)
{
  g_return_val_if_fail (CM_IS_ENC (self), NULL);

  return self->ed_key;
}
