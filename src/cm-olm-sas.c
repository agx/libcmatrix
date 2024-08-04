/* cm-olm-sas.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-olm-sas"

#include "cm-config.h"

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <olm/olm.h>
#include <olm/sas.h>

#include "events/cm-event-private.h"
#include "events/cm-verification-event-private.h"
#include "cm-enc-private.h"
#include "cm-client.h"
#include "cm-client-private.h"
#include "cm-device-private.h"
#include "users/cm-user-private.h"
#include "cm-utils-private.h"
#include "cm-olm-sas-private.h"

/* The order shouldn't be changed */
/* https://github.com/matrix-org/matrix-spec-proposals/blob/old_master/data-definitions/sas-emoji.json */
static const char *emojis[] = {
  "🐶",  /*  "Dog",  "U+1F436" */
  "🐱",  /*  "Cat",  "U+1F431" */
  "🦁",  /*  "Lion",  "U+1F981" */
  "🐎",  /*  "Horse",  "U+1F40E" */
  "🦄",  /*  "Unicorn",  "U+1F984" */
  "🐷",  /*  "Pig",  "U+1F437" */
  "🐘",  /*  "Elephant",  "U+1F418" */
  "🐰",  /*  "Rabbit",  "U+1F430" */
  "🐼",  /*  "Panda",  "U+1F43C" */
  "🐓",  /*  "Rooster",  "U+1F413" */
  "🐧",  /*  "Penguin",  "U+1F427" */
  "🐢",  /*  "Turtle",  "U+1F422" */
  "🐟",  /*  "Fish",  "U+1F41F" */
  "🐙",  /*  "Octopus",  "U+1F419" */
  "🦋",  /*  "Butterfly",  "U+1F98B" */
  "🌷",  /*  "Flower",  "U+1F337" */
  "🌳",  /*  "Tree",  "U+1F333" */
  "🌵",  /*  "Cactus",  "U+1F335" */
  "🍄",  /*  "Mushroom",  "U+1F344" */
  "🌏",  /*  "Globe",  "U+1F30F" */
  "🌙",  /*  "Moon",  "U+1F319" */
  "☁️",  /*  "Cloud",  "U+2601U+FE0F" */
  "🔥",  /*  "Fire",  "U+1F525" */
  "🍌",  /*  "Banana",  "U+1F34C" */
  "🍎",  /*  "Apple",  "U+1F34E" */
  "🍓",  /*  "Strawberry",  "U+1F353" */
  "🌽",  /*  "Corn",  "U+1F33D" */
  "🍕",  /*  "Pizza",  "U+1F355" */
  "🎂",  /*  "Cake",  "U+1F382" */
  "❤️",  /*  "Heart",  "U+2764U+FE0F" */
  "😀",  /*  "Smiley",  "U+1F600" */
  "🤖",  /*  "Robot",  "U+1F916" */
  "🎩",  /*  "Hat",  "U+1F3A9" */
  "👓",  /*  "Glasses",  "U+1F453" */
  "🔧",  /*  "Spanner",  "U+1F527" */
  "🎅",  /*  "Santa",  "U+1F385" */
  "👍",  /*  "Thumbs Up",  "U+1F44D" */
  "☂️",  /*  "Umbrella",  "U+2602U+FE0F" */
  "⌛",  /*  "Hourglass",  "U+231B" */
  "⏰",  /*  "Clock",  "U+23F0" */
  "🎁",  /*  "Gift",  "U+1F381" */
  "💡",  /*  "Light Bulb",  "U+1F4A1" */
  "📕",  /*  "Book",  "U+1F4D5" */
  "✏️",  /*  "Pencil",  "U+270FU+FE0F" */
  "📎",  /*  "Paperclip",  "U+1F4CE" */
  "✂️",  /*  "Scissors",  "U+2702U+FE0F" */
  "🔒",  /*  "Lock",  "U+1F512" */
  "🔑",  /*  "Key",  "U+1F511" */
  "🔨",  /*  "Hammer",  "U+1F528" */
  "☎️",  /*  "Telephone",  "U+260EU+FE0F" */
  "🏁",  /*  "Flag",  "U+1F3C1" */
  "🚂",  /*  "Train",  "U+1F682" */
  "🚲",  /*  "Bicycle",  "U+1F6B2" */
  "✈️",  /*  "Aeroplane",  "U+2708U+FE0F" */
  "🚀",  /*  "Rocket",  "U+1F680" */
  "🏆",  /*  "Trophy",  "U+1F3C6" */
  "⚽",  /*  "Ball",  "U+26BD" */
  "🎸",  /*  "Guitar",  "U+1F3B8" */
  "🎺",  /*  "Trumpet",  "U+1F3BA" */
  "🔔",  /*  "Bell",  "U+1F514" */
  "⚓",  /*  "Anchor",  "U+2693" */
  "🎧",  /*  "Headphones",  "U+1F3A7" */
  "📁",  /*  "Folder",  "U+1F4C1" */
  "📌",  /*  "Pin",  "U+1F4CC" */
};

#define NUM_SAS_BYTES   (6)

struct _CmOlmSas
{
  GObject    parent_instance;

  CmClient  *cm_client;

  OlmSAS    *olm_sas;
  char      *our_pub_key;
  char      *their_pub_key;

  char      *their_user_id;
  char      *their_device_id;
  CmDevice  *their_device;

  char      *cancel_code;

  CmVerificationEvent *key_verification;
  CmEvent   *key_verification_cancel;
  CmEvent   *key_verification_accept;
  CmEvent   *key_verification_ready;
  CmEvent   *key_verification_mac;
  CmEvent   *key_verification_done;
  CmEvent   *verification_key;
  GString   *commitment_str;

  guint8    *sas_bytes;
  guint8    *sas_emoji_indices;
  GPtrArray *sas_emojis;
  guint16   *sas_decimals;

  gboolean   verified;
};

G_DEFINE_TYPE (CmOlmSas, cm_olm_sas, G_TYPE_OBJECT)

static char *
calculate_mac (CmOlmSas   *self,
               const char *input,
               const char *info,
               size_t      info_len)
{
  char *mac;
  size_t len;

  g_assert (CM_IS_OLM_SAS (self));

  len = olm_sas_mac_length (self->olm_sas);
  mac = g_malloc (len + 1);
  olm_sas_calculate_mac (self->olm_sas, input, strlen (input),
                         info, info_len, mac, len);
  mac[len] = '\0';

  return mac;
}

static void
cm_olm_sas_generate_bytes (CmOlmSas *self)
{
  g_autoptr(GString) sas_info = NULL;
  g_autofree char *their_info = NULL;
  g_autofree char *our_info = NULL;
  const char *user_id, *device_id, *transaction_id;
  guint8 *bytes;

  if (self->sas_bytes)
    return;

  user_id = cm_client_get_user_id (self->cm_client);
  device_id = cm_client_get_device_id (self->cm_client);
  our_info = g_strdup_printf ("%s|%s|%s", user_id, device_id, self->our_pub_key);

  user_id = self->their_user_id;
  device_id = self->their_device_id;
  their_info = g_strdup_printf ("%s|%s|%s", user_id, device_id, self->their_pub_key);

  sas_info = g_string_sized_new (1024);
  transaction_id = cm_verification_event_get_transaction_id (self->key_verification);

  g_string_append_printf (sas_info, "MATRIX_KEY_VERIFICATION_SAS|%s|%s|%s",
                          their_info, our_info, transaction_id);

  /* Always generate 6 bytes even if we may use decimal verification */
  /* for which we'll ignore the last byte as it requires only 5 */
  self->sas_bytes = g_malloc (NUM_SAS_BYTES);
  bytes = self->sas_bytes;
  olm_sas_generate_bytes (self->olm_sas, sas_info->str, sas_info->len, self->sas_bytes, NUM_SAS_BYTES);

  /* We have 7 items of 6 bit each */
  self->sas_emoji_indices = g_malloc0 (7);

  /* The indices are of 6 bits, so iterate over every byte and extract
   * those 6 bit indices and store as bytes
   */
  /* Don't complicate by using loops */
  self->sas_emoji_indices[0] = bytes[0] >> 2;
  self->sas_emoji_indices[1] = (bytes[0] & 0b11) << 4 | bytes[1] >> 4;
  self->sas_emoji_indices[2] = (bytes[1] & 0b1111) << 2 | bytes[2] >> 6;
  self->sas_emoji_indices[3] = bytes[2] & 0b111111;
  self->sas_emoji_indices[4] = bytes[3] >> 2;
  self->sas_emoji_indices[5] = (bytes[3] & 0b11) << 4 | bytes[4] >> 4;
  self->sas_emoji_indices[6] = (bytes[4] & 0b1111) << 2 | bytes[5] >> 6;

  /* There are 3 numbers of 13 bits */
  self->sas_decimals = g_malloc0 (4 * sizeof(guint16));
  self->sas_decimals[0] = (bytes[0] << 5 | bytes[1] >> 3) + 1000;
  self->sas_decimals[1] = ((bytes[1] & 0b111) << 10 | bytes[2] << 2 | bytes[3] >> 6) + 1000;
  self->sas_decimals[2] = ((bytes[3] & 0b111111) << 7 | bytes[4] >> 1) + 1000;
}

static CmVerificationEvent *
cm_olm_sas_get_start_event (CmOlmSas *self)
{
  CmEvent *event;

  g_assert (CM_IS_OLM_SAS (self));

  event = CM_EVENT (self->key_verification);

  if (cm_event_get_m_type (event) == CM_M_KEY_VERIFICATION_REQUEST)
    return g_object_get_data (G_OBJECT (event), "start");

  return self->key_verification;
}

static JsonObject *
olm_sas_get_message_json (CmOlmSas    *self,
                          JsonObject **content)
{
  JsonObject *root, *child;
  const char *value;

  root = json_object_new ();

  child = json_object_new ();
  json_object_set_object_member (root, "messages", child);

  value = cm_event_get_sender_id (CM_EVENT (self->key_verification));
  json_object_set_object_member (child, value, json_object_new ());
  child = cm_utils_json_object_get_object (child, value);

  value = cm_event_get_sender_device_id (CM_EVENT (self->key_verification));
  json_object_set_object_member (child, value, json_object_new ());
  child = cm_utils_json_object_get_object (child, value);

  value = cm_verification_event_get_transaction_id (self->key_verification);
  json_object_set_string_member (child, "transaction_id", value);

  if (content)
    *content = child;

  return root;
}

static void
cm_olm_sas_create_commitment (CmOlmSas *self)
{
  g_autofree OlmUtility *olm_util = NULL;
  g_autofree char *sha256 = NULL;
  g_autoptr(JsonObject) json = NULL;
  g_autoptr(GString) str = NULL;
  CmVerificationEvent *event;
  JsonObject *content;
  size_t len;

  g_return_if_fail (CM_IS_OLM_SAS (self));
  g_return_if_fail (self->key_verification);

  if (self->commitment_str->len)
    return;

  /* We should have an m.key.verification.start event to get commitment */
  event = cm_olm_sas_get_start_event (self);
  g_return_if_fail (event);

  str = g_string_sized_new (1024);
  if (!self->our_pub_key)
    {
      len = olm_sas_pubkey_length (self->olm_sas);
      self->our_pub_key = g_malloc (len + 1);
      olm_sas_get_pubkey (self->olm_sas, self->our_pub_key, len);
      self->our_pub_key[len] = '\0';
    }

  g_string_append_len (str, self->our_pub_key, strlen (self->our_pub_key));
  json = cm_event_get_json (CM_EVENT (event));
  content = cm_utils_json_object_get_object (json, "content");
  cm_utils_json_get_canonical (content, str);

  olm_util = g_malloc (olm_utility_size ());
  olm_utility (olm_util);

  len = olm_sha256_length (olm_util);
  sha256 = g_malloc (len);
  olm_sha256 (olm_util, str->str, str->len, sha256, len);
  g_string_append_len (self->commitment_str, sha256, len);
}

static void
cm_olm_sas_finalize (GObject *object)
{
  CmOlmSas *self = (CmOlmSas *)object;

  g_clear_object (&self->key_verification);
  olm_clear_sas (self->olm_sas);
  g_free (self->olm_sas);

  if (self->commitment_str)
    g_string_free (self->commitment_str, TRUE);

  g_free (self->our_pub_key);
  g_free (self->their_pub_key);
  g_free (self->their_user_id);
  g_free (self->their_device_id);

  g_free (self->sas_bytes);

  g_clear_weak_pointer (&self->cm_client);

  G_OBJECT_CLASS (cm_olm_sas_parent_class)->finalize (object);
}

static void
cm_olm_sas_class_init (CmOlmSasClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_olm_sas_finalize;
}

static void
cm_olm_sas_init (CmOlmSas *self)
{
  size_t len;

  self->olm_sas = g_malloc (olm_sas_size ());
  self->commitment_str = g_string_sized_new (256);
  olm_sas (self->olm_sas);

  len = olm_create_sas_random_length (self->olm_sas);
  if (len)
    {
      cm_gcry_t buffer;

      buffer = gcry_random_bytes (len, GCRY_STRONG_RANDOM);
      olm_create_sas (self->olm_sas, buffer, len);
      gcry_free (buffer);
    }
}

CmOlmSas *
cm_olm_sas_new (void)
{
  return g_object_new (CM_TYPE_OLM_SAS, NULL);
}

void
cm_olm_sas_set_client (CmOlmSas *self,
                       gpointer  cm_client)
{
  g_return_if_fail (CM_IS_OLM_SAS (self));
  g_return_if_fail (CM_IS_CLIENT (cm_client));

  g_set_weak_pointer (&self->cm_client, cm_client);
}

static gboolean
cm_olm_array_has_string (JsonObject *content,
                         const char *array_str,
                         const char *value)
{
  JsonArray *array;
  const char *element;

  if (!content)
    return FALSE;

  g_assert (array_str && *array_str);
  g_assert (value && *value);

  array = cm_utils_json_object_get_array (content, array_str);
  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      element = json_array_get_string_element (array, i);

      if (g_strcmp0 (element, value) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
cm_olm_sas_parse_verification_start (CmOlmSas *self,
                                     CmEvent  *event)
{
  g_autoptr(JsonObject) root = NULL;
  JsonObject *content;

  g_assert (CM_IS_OLM_SAS (self));
  g_assert (CM_IS_EVENT (event));

  root = cm_event_get_json (event);
  content = cm_utils_json_object_get_object (root, "content");

  if (g_strcmp0 (cm_utils_json_object_get_string (content, "method"), "m.sas.v1") != 0 ||
      !cm_olm_array_has_string (content, "key_agreement_protocols", "curve25519-hkdf-sha256") ||
      !cm_olm_array_has_string (content, "hashes", "sha256") ||
      !cm_olm_array_has_string (content, "message_authentication_codes", "hkdf-hmac-sha256") ||
      !cm_olm_array_has_string (content, "short_authentication_string", "decimal"))
    self->cancel_code = g_strdup ("m.unknown_method");
}

static void
cm_olm_sas_parse_verification_mac (CmOlmSas *self,
                                   CmEvent  *event)
{
  g_autoptr(GString) base_info = NULL;
  g_autoptr(GString) key_ids = NULL;
  g_autoptr(GList) members = NULL;
  g_autofree char *mac = NULL;
  JsonObject *root, *content, *mac_json;

  g_assert (CM_IS_OLM_SAS (self));
  g_assert (CM_IS_EVENT (event));

  if (!g_object_get_data (G_OBJECT (self->key_verification), "key") &&
      !self->cancel_code)
    {
      self->cancel_code = g_strdup ("m.unexpected_message");
      return;
    }

  if (self->verified || self->cancel_code)
    return;

  root = cm_event_get_json (event);
  content = cm_utils_json_object_get_object (root, "content");
  mac_json = cm_utils_json_object_get_object (content, "mac");

  if (!mac_json)
    {
      self->cancel_code = g_strdup ("m.key_mismatch");
      return;
    }

  key_ids = g_string_sized_new (1024);
  members = json_object_get_members (mac_json);

  members = g_list_sort (members, (GCompareFunc)g_strcmp0);

  for (GList *item = members; item && item->data; item = item->next)
    {
      g_string_append (key_ids, (char *)item->data);
      g_string_append_c (key_ids, ',');
    }

  if (!key_ids->len)
    {
      self->cancel_code = g_strdup ("m.key_mismatch");
      return;
    }

  /* Remove the trailing ',' */
  g_string_set_size (key_ids, key_ids->len - 1);

  base_info = g_string_sized_new (1024);
  g_string_printf (base_info, "MATRIX_KEY_VERIFICATION_MAC%s%s%s%s%s",
                   self->their_user_id, self->their_device_id,
                   cm_client_get_user_id (self->cm_client),
                   cm_client_get_device_id (self->cm_client),
                   cm_verification_event_get_transaction_id (self->key_verification));
  g_string_append (base_info, "KEY_IDS");

  mac = calculate_mac (self, key_ids->str, base_info->str, base_info->len);

  if (g_strcmp0 (mac, cm_utils_json_object_get_string (content, "keys")) != 0)
    {
      g_debug ("(%p) key mismatch, mac != keys", self);
      self->cancel_code = g_strdup ("m.key_mismatch");
      return;
    }

  for (GList *item = members; item && item->data; item = item->next)
    {
      g_auto(GStrv) strv = NULL;
      const char *key_mac;
      CmDevice *device;
      CmUser *user;

      strv = g_strsplit (item->data, ":", -1);

      if (g_strcmp0 (strv[0], "ed25519") != 0)
        {
          g_debug ("(%p) key mismatch, '%s' is not ed25519", self, strv[0]);
          self->cancel_code = g_strdup ("m.key_mismatch");
          return;
        }

      if (g_strcmp0 (strv[1], self->their_device_id) != 0)
        continue;

      key_mac = cm_utils_json_object_get_string (mac_json, item->data);
      user = cm_event_get_sender (CM_EVENT (self->key_verification));
      device = cm_user_find_device (user, self->their_device_id);

      if (!device || !key_mac || !cm_device_get_ed_key (device))
        {
          self->cancel_code = g_strdup ("m.key_mismatch");
          return;
        }

      g_free (mac);
      g_string_truncate (base_info, base_info->len - strlen ("KEY_IDS"));
      g_string_append (base_info, item->data);

      mac = calculate_mac (self, cm_device_get_ed_key (device),
                           base_info->str, base_info->len);

      /* Currently we handle only one device */
      if (g_strcmp0 (key_mac, mac) != 0)
        {
          self->cancel_code = g_strdup ("m.key_mismatch");
          return;
        }

      self->verified = TRUE;
      self->their_device = g_object_ref (device);
      cm_device_set_verified (device, TRUE);
    }
}

void
cm_olm_sas_set_key_verification (CmOlmSas            *self,
                                 CmVerificationEvent *event)
{
  CmEventType type;
  gint64 minutes;

  g_return_if_fail (CM_IS_OLM_SAS (self));
  g_return_if_fail (CM_IS_EVENT (event));
  g_return_if_fail (!self->key_verification);

  type = cm_event_get_m_type (CM_EVENT (event));
  g_return_if_fail (type == CM_M_KEY_VERIFICATION_REQUEST ||
                    type == CM_M_KEY_VERIFICATION_START);
  self->key_verification = g_object_ref (event);

  /* fixme: We now only accepts verification requests */
  self->their_user_id = g_strdup (cm_event_get_sender_id (CM_EVENT (event)));
  self->their_device_id = g_strdup (cm_event_get_sender_device_id (CM_EVENT (event)));

  minutes = (time (NULL) - cm_event_get_time_stamp (CM_EVENT (event)) / 1000) % 60;

  if (type == CM_M_KEY_VERIFICATION_START)
    cm_olm_sas_parse_verification_start (self, CM_EVENT (event));

  if (self->cancel_code)
    return;

  /* Cancel if request is 10+ minutes from the past or 5+ minutes from the future */
  if (minutes < -5 || minutes > 10)
    self->cancel_code = g_strdup ("m.timeout");
}

gboolean
cm_olm_sas_matches_event (CmOlmSas            *self,
                          CmVerificationEvent *event)
{
  GObject *obj = (GObject *)self->key_verification;
  const char *item_txn_id, *event_txn_id;
  CmEventType type;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), FALSE);
  g_return_val_if_fail (CM_IS_EVENT (event), FALSE);
  g_return_val_if_fail (self->key_verification, FALSE);

  if (event == self->key_verification)
    return TRUE;

  item_txn_id = cm_verification_event_get_transaction_id (event);
  event_txn_id = cm_verification_event_get_transaction_id (self->key_verification);

  if (g_strcmp0 (item_txn_id, event_txn_id) != 0)
    return FALSE;

  g_object_ref (event);
  type = cm_event_get_m_type (CM_EVENT (event));

  if (type == CM_M_KEY_VERIFICATION_KEY)
    {
      g_autofree char *key = NULL;

      key = g_strdup (cm_verification_event_get_verification_key (event));

      if (olm_sas_is_their_key_set (self->olm_sas))
        {
          g_warning ("Key was already set");
        }
      else
        {
          self->their_pub_key = g_strdup (key);
          olm_sas_set_their_key (self->olm_sas, key, strlen (key));
        }
    }

  if (type == CM_M_KEY_VERIFICATION_CANCEL)
    g_object_set_data_full (obj, "cancel", event, g_object_unref);
  else if (type == CM_M_KEY_VERIFICATION_DONE)
    g_object_set_data_full (obj, "done", event, g_object_unref);
  else if (type == CM_M_KEY_VERIFICATION_KEY)
    g_object_set_data_full (obj, "key", event, g_object_unref);
  else if (type == CM_M_KEY_VERIFICATION_MAC)
    g_object_set_data_full (obj, "mac", event, g_object_unref);
  else if (type == CM_M_KEY_VERIFICATION_READY)
    g_object_set_data_full (obj, "ready", event, g_object_unref);
  else if (type == CM_M_KEY_VERIFICATION_REQUEST)
    g_object_set_data_full (obj, "request", event, g_object_unref);
  else if (type == CM_M_KEY_VERIFICATION_START)
    g_object_set_data_full (obj, "start", event, g_object_unref);

  if (type == CM_M_KEY_VERIFICATION_START)
    cm_olm_sas_parse_verification_start (self, CM_EVENT (event));

  if (type == CM_M_KEY_VERIFICATION_MAC)
    cm_olm_sas_parse_verification_mac (self, CM_EVENT (event));

  if (type == CM_M_KEY_VERIFICATION_CANCEL && !self->cancel_code)
    self->cancel_code = g_strdup ("m.timeout");

  /* generate emojis */
  if (type == CM_M_KEY_VERIFICATION_KEY)
    cm_olm_sas_get_emojis (self);

  g_signal_emit_by_name (obj, "updated", 0);

  return TRUE;
}

/**
 * cm_olm_sas_get_cancel_reason:
 * @self: A #CmOlmSas
 *
 * Get the error 'code' to be used for m.key.verification.cancel.
 * This is to be checked after every update and the verification
 * should be cancelled if a non-%NULL value is return.
 *
 * Returns: (nullable): A string
 */
const char *
cm_olm_sas_get_cancel_code (CmOlmSas *self)
{
  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);

  return self->cancel_code;
}

CmEvent *
cm_olm_sas_get_cancel_event (CmOlmSas   *self,
                             const char *cancel_code)
{
  JsonObject *root, *child;
  CmEvent *event;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);
  g_return_val_if_fail (self->cm_client, NULL);

  if (self->key_verification_cancel)
    return self->key_verification_cancel;

  if (!cancel_code)
    cancel_code = "m.user";

  if (g_strcmp0 (cancel_code, "m.user") != 0 &&
      g_strcmp0 (cancel_code, "m.timeout") != 0 &&
      g_strcmp0 (cancel_code, "m.unknown_method") != 0 &&
      g_strcmp0 (cancel_code, "m.key_mismatch") != 0 &&
      g_strcmp0 (cancel_code, "m.user_mismatch") != 0 &&
      g_strcmp0 (cancel_code, "m.unexpected_message") != 0)
    g_return_val_if_reached (NULL);

  event = cm_event_new (CM_M_KEY_VERIFICATION_CANCEL);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->cm_client));
  self->key_verification_cancel = event;

  root = olm_sas_get_message_json (self, &child);
  cm_event_set_json (event, root, NULL);

  json_object_set_string_member (child, "code", cancel_code);

  return self->key_verification_cancel;
}

CmEvent *
cm_olm_sas_get_ready_event (CmOlmSas *self)
{
  JsonObject *root, *child;
  JsonArray *array;
  CmEvent *event;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);

  if (self->key_verification_ready)
    return self->key_verification_ready;

  event = cm_event_new (CM_M_KEY_VERIFICATION_READY);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->cm_client));
  self->key_verification_ready = event;

  root = olm_sas_get_message_json (self, &child);
  cm_event_set_json (event, root, NULL);

  array = json_array_new ();
  json_array_add_string_element (array, "m.sas.v1");
  json_object_set_array_member (child, "methods", array);

  json_object_set_string_member (child, "from_device",
                                 cm_client_get_device_id (self->cm_client));

  return self->key_verification_ready;
}

CmEvent *
cm_olm_sas_get_accept_event (CmOlmSas *self)
{
  JsonObject *root, *child;
  JsonArray *array;
  CmEvent *event;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);

  if (self->key_verification_accept)
    return self->key_verification_accept;

  /* We should have an m.key.verification.start event to get commitment */
  g_return_val_if_fail (cm_olm_sas_get_start_event (self), NULL);
  cm_olm_sas_create_commitment (self);

  event = cm_event_new (CM_M_KEY_VERIFICATION_ACCEPT);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->cm_client));
  self->key_verification_accept = event;

  root = olm_sas_get_message_json (self, &child);
  cm_event_set_json (event, root, NULL);

  json_object_set_string_member (child, "hash", "sha256");
  json_object_set_string_member (child, "method", "m.sas.v1");
  json_object_set_string_member (child, "key_agreement_protocol", "curve25519-hkdf-sha256");
  json_object_set_string_member (child, "commitment", self->commitment_str->str);
  json_object_set_string_member (child, "message_authentication_code", "hkdf-hmac-sha256");

  array = json_array_new ();
  json_array_add_string_element (array, "emoji");
  json_array_add_string_element (array, "decimal");
  json_object_set_array_member (child, "short_authentication_string", array);

  return self->key_verification_accept;
}

CmEvent *
cm_olm_sas_get_key_event (CmOlmSas *self)
{
  JsonObject *root, *child;
  CmEvent *event;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);

  if (self->verification_key)
    return self->verification_key;

  /* We should have an m.key.verification.start event to get key event */
  g_return_val_if_fail (cm_olm_sas_get_start_event (self), NULL);
  cm_olm_sas_create_commitment (self);

  event = cm_event_new (CM_M_KEY_VERIFICATION_KEY);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->cm_client));
  self->verification_key = event;

  root = olm_sas_get_message_json (self, &child);
  cm_event_set_json (event, root, NULL);

  json_object_set_string_member (child, "key", self->our_pub_key);

  return self->verification_key;
}

GPtrArray *
cm_olm_sas_get_emojis (CmOlmSas *self)
{
  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);

  if (!g_object_get_data (G_OBJECT (self->key_verification), "key"))
    return NULL;

  /* We need to have both keys to have a drink */
  g_return_val_if_fail (self->our_pub_key, NULL);
  g_return_val_if_fail (self->their_pub_key, NULL);

  cm_olm_sas_generate_bytes (self);

  if (!self->sas_emojis)
    {
      self->sas_emojis = g_ptr_array_sized_new (7);

      for (guint i = 0; i < 7; i++)
        g_ptr_array_add (self->sas_emojis, g_strdup (emojis[self->sas_emoji_indices[i]]));

      g_object_set_data (G_OBJECT (self->key_verification), "emoji", self->sas_emojis);
      g_object_set_data (G_OBJECT (self->key_verification), "decimal", self->sas_decimals);
    }

  return self->sas_emojis;
}

CmEvent *
cm_olm_sas_get_mac_event (CmOlmSas *self)
{
  g_autoptr(GString) base_info = NULL;
  g_autofree char *key_id = NULL;
  g_autofree char *keys = NULL;
  g_autofree char *mac = NULL;
  JsonObject *root, *content, *mac_json;
  const char *ed25519;
  CmEvent *event;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);
  g_return_val_if_fail (self->verification_key, NULL);

  if (self->key_verification_mac)
    return self->key_verification_mac;

  base_info = g_string_sized_new (1024);
  g_string_printf (base_info, "MATRIX_KEY_VERIFICATION_MAC%s%s%s%s%s",
                   cm_client_get_user_id (self->cm_client),
                   cm_client_get_device_id (self->cm_client),
                   self->their_user_id, self->their_device_id,
                   cm_verification_event_get_transaction_id (self->key_verification));
  key_id = g_strconcat ("ed25519:", cm_client_get_device_id (self->cm_client), NULL);
  g_string_append (base_info, key_id);

  ed25519 = cm_client_get_ed25519_key (self->cm_client);
  mac = calculate_mac (self, ed25519, base_info->str, base_info->len);

  g_string_truncate (base_info, base_info->len - strlen (key_id));
  g_string_append (base_info, "KEY_IDS");
  keys = calculate_mac (self, key_id, base_info->str, base_info->len);

  event = cm_event_new (CM_M_KEY_VERIFICATION_MAC);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->cm_client));
  self->key_verification_mac = event;

  root = olm_sas_get_message_json (self, &content);
  mac_json = json_object_new ();
  json_object_set_string_member (mac_json, key_id, mac);
  json_object_set_object_member (content, "mac", mac_json);
  json_object_set_string_member (content, "keys", keys);
  cm_event_set_json (event, root, NULL);

  return self->key_verification_mac;
}

CmEvent *
cm_olm_sas_get_done_event (CmOlmSas *self)
{
  JsonObject *root;
  CmEvent *event;

  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);
  g_return_val_if_fail (self->key_verification, NULL);
  g_return_val_if_fail (self->cm_client, NULL);

  if (self->key_verification_done)
    return self->key_verification_done;

  event = cm_event_new (CM_M_KEY_VERIFICATION_DONE);
  cm_event_create_txn_id (event, cm_client_pop_event_id (self->cm_client));
  self->key_verification_done = event;

  root = olm_sas_get_message_json (self, NULL);
  cm_event_set_json (event, root, NULL);

  return self->key_verification_done;
}

gboolean
cm_olm_sas_is_verified (CmOlmSas *self)
{
  g_return_val_if_fail (CM_IS_OLM_SAS (self), FALSE);

  return self->verified;
}

CmDevice *
cm_olm_sas_get_device (CmOlmSas *self)
{
  g_return_val_if_fail (CM_IS_OLM_SAS (self), NULL);

  return self->their_device;
}
