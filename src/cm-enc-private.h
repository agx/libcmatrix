/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-enc.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <json-glib/json-glib.h>
#include <glib-object.h>

#include "events/cm-event.h"
#include "cm-types.h"

G_BEGIN_DECLS

typedef struct _CmEncFileInfo CmEncFileInfo;
typedef void * cm_gcry_t;


struct _CmEncFileInfo {
  char *mxc_uri;

  char *aes_iv_base64;
  char *aes_key_base64;
  char *sha256_base64;

  char *algorithm;
  char *version;
  char *kty;

  gboolean extractable;
};

#define ALGORITHM_MEGOLM  "m.megolm.v1.aes-sha2"
#define ALGORITHM_OLM     "m.olm.v1.curve25519-aes-sha2"
#define CURVE25519_SIZE   43    /* when base64 encoded */
#define ED25519_SIZE      43    /* when base64 encoded */

#define CM_TYPE_ENC (cm_enc_get_type ())

G_DECLARE_FINAL_TYPE (CmEnc, cm_enc, CM, ENC, GObject)

CmEnc          *cm_enc_new                       (gpointer             matrix_db,
                                                  const char          *pickle,
                                                  const char          *key);
gpointer       cm_enc_get_sas_for_event          (CmEnc               *self,
                                                  CmVerificationEvent *event);
void           cm_enc_set_details                (CmEnc               *self,
                                                  GRefString          *user_id,
                                                  const char          *device_id);
char          *cm_enc_get_pickle                 (CmEnc               *self);
char          *cm_enc_get_pickle_key             (CmEnc               *self);
char          *cm_enc_sign_string                (CmEnc               *self,
                                                  const char          *str,
                                                  size_t               len);
gboolean       cm_enc_verify                     (CmEnc               *self,
                                                  JsonObject          *object,
                                                  const char          *matrix_id,
                                                  const char          *device_id,
                                                  const char          *ed_key);
size_t         cm_enc_max_one_time_keys          (CmEnc               *self);
size_t         cm_enc_create_one_time_keys       (CmEnc               *self,
                                                  size_t               count);
void           cm_enc_publish_one_time_keys      (CmEnc               *self);
JsonObject    *cm_enc_get_one_time_keys          (CmEnc               *self);
char          *cm_enc_get_one_time_keys_json     (CmEnc               *self);
char          *cm_enc_get_device_keys_json       (CmEnc               *self);
void           cm_enc_handle_room_encrypted      (CmEnc               *self,
                                                  JsonObject          *object);
char          *cm_enc_handle_join_room_encrypted (CmEnc               *self,
                                                  CmRoom              *room,
                                                  JsonObject          *object);
JsonObject    *cm_enc_encrypt_for_chat           (CmEnc               *self,
                                                  CmRoom              *room,
                                                  const char          *message);
JsonObject    *cm_enc_create_out_group_keys      (CmEnc               *self,
                                                  CmRoom              *room,
                                                  GPtrArray           *one_time_keys,
                                                  gpointer            *out_session);
gboolean       cm_enc_has_room_group_key         (CmEnc               *self,
                                                  CmRoom              *room);
void           cm_enc_set_room_group_key         (CmEnc               *self,
                                                  CmRoom              *room,
                                                  gpointer             out_session);
void           cm_enc_rm_room_group_key          (CmEnc               *self,
                                                  CmRoom              *room);
void           cm_enc_find_file_enc_async        (CmEnc               *self,
                                                  const char          *uri,
                                                  GAsyncReadyCallback  callback,
                                                  gpointer             user_data);
CmEncFileInfo *cm_enc_find_file_enc_finish       (CmEnc               *self,
                                                  GAsyncResult        *result,
                                                  GError             **error);
void           cm_enc_file_info_free             (gpointer             data);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CmEncFileInfo, cm_enc_file_info_free)
/* G_DEFINE_AUTOPTR_CLEANUP_FUNC (cm_gcry_t, gcry_free) */

/* For tests */
GRefString    *cm_enc_get_user_id            (CmEnc    *self);
const char    *cm_enc_get_device_id          (CmEnc    *self);
const char    *cm_enc_get_curve25519_key     (CmEnc    *self);
const char    *cm_enc_get_ed25519_key        (CmEnc    *self);

G_END_DECLS
