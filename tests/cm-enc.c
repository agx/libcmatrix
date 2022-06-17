/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* matrix-utils.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#undef NDEBUG
#undef G_DISABLE_ASSERT
#undef G_DISABLE_CHECKS
#undef G_DISABLE_CAST_CHECKS
#undef G_LOG_DOMAIN

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <olm/olm.h>
#include <sys/random.h>

#include "cm-utils-private.h"
#include "cm-enc-private.h"

typedef struct EncData {
  char *user_id;
  char *device_id;
  char *pickle_key;
  char *olm2_pickle;
  char *olm3_pickle;
  char *olm3_v4_pickle;
  char *curve_key;
  char *ed_key;
  /* These are not marked as published */
  char *one_time_key1;
  char *one_time_key2;
  char *one_time_key3;
} EncData;

EncData enc[] = {
  {
    "@neo:example.com",
    "JOJOAREBZY",
    "cefdef40-3b16-4d71-8685-2740833c3297",
    "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",
  },
  {
    "@neo:example.org",
    "JDFVAREDFE",
    "bc80e9a7-f175-4352-9b07-7178b63546ff",
    "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",
  },
};


static void
test_cm_enc_verify (void)
{
  g_autoptr(CmEnc) enc1 = NULL;
  g_autoptr(CmEnc) enc2 = NULL;
  JsonObject *object, *root;
  char *sign, *json, *key_label;
  const char *message;
  EncData data;

  data = enc[0];
  enc1 = cm_enc_new (NULL, enc[0].olm2_pickle, enc[0].pickle_key);
  enc2 = cm_enc_new (NULL, enc[1].olm2_pickle, enc[1].pickle_key);
  g_assert (CM_IS_ENC (enc1));
  g_assert (CM_IS_ENC (enc2));

  /* @message is in canonical form */
  message = "{\"timeout\":20000,\"type\":\"m.message\"}";
  sign = cm_enc_sign_string (enc1, message, -1);
  g_assert_nonnull (sign);

  root = cm_utils_string_to_json_object (message);
  g_assert_nonnull (root);

  json_object_set_object_member (root, "signatures", json_object_new ());
  object = json_object_get_object_member (root, "signatures");
  json_object_set_object_member (object, data.user_id, json_object_new ());
  object = json_object_get_object_member (object, data.user_id);
  g_assert (object);

  key_label = g_strconcat ("ed25519:", data.device_id, NULL);
  json_object_set_string_member (object, key_label, sign);
  json = cm_utils_json_object_to_string (root, FALSE);
  g_assert_nonnull (json);
  g_free (sign);
  g_free (key_label);
  json_object_unref (root);

  root = cm_utils_string_to_json_object (json);
  g_assert_nonnull (root);

  g_assert_true (cm_enc_verify (enc1, root, data.user_id,
                                data.device_id, data.ed_key));
  g_assert_true (cm_enc_verify (enc2, root, data.user_id,
                                data.device_id, data.ed_key));
  g_assert_false (cm_enc_verify (enc1, root, data.user_id,
                                 data.device_id, data.curve_key));
  g_assert_false (cm_enc_verify (enc1, root, enc[1].user_id,
                                 data.device_id, data.ed_key));
  json_object_unref (root);
  g_free (json);
}

static void
test_cm_enc_new (void)
{
  const char *value;
  char *pickle;
  EncData data;

  for (guint i = 0; i < G_N_ELEMENTS (enc); i++) {
    g_autoptr(CmEnc) cm_enc = NULL;

    data = enc[i];

#ifdef HAVE_OLM3
    cm_enc = cm_enc_new (NULL, data.olm3_pickle, data.pickle_key);
    g_assert (CM_IS_ENC (cm_enc));

    pickle = cm_enc_get_pickle (cm_enc);
# ifdef OLM_ACCOUNT_PICKLE_V4
    g_assert_cmpstr (pickle, ==, data.olm3_v4_pickle);
# else
    g_assert_cmpstr (pickle, ==, data.olm3_pickle);
# endif
    g_clear_pointer (&pickle, g_free);
    g_object_unref (cm_enc);
#endif

    cm_enc = cm_enc_new (NULL, data.olm2_pickle, data.pickle_key);
    g_assert (CM_IS_ENC (cm_enc));

    value = cm_enc_get_curve25519_key (cm_enc);
    g_assert_cmpstr (value, ==, data.curve_key);

    value = cm_enc_get_ed25519_key (cm_enc);
    g_assert_cmpstr (value, ==, data.ed_key);

    pickle = cm_enc_get_pickle (cm_enc);
#ifdef HAVE_OLM3
# ifdef OLM_ACCOUNT_PICKLE_V4
    g_assert_cmpstr (pickle, ==, data.olm3_v4_pickle);
# else
    g_assert_cmpstr (pickle, ==, data.olm3_pickle);
# endif
#else
    g_assert_cmpstr (pickle, ==, data.olm2_pickle);
#endif
    g_clear_pointer (&pickle, g_free);
  }
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/matrix/enc/new", test_cm_enc_new);
  g_test_add_func ("/matrix/enc/verify", test_cm_enc_verify);

  return g_test_run ();
}
