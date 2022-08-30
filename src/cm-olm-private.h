/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-olm-private.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include "cm-db-private.h"

G_BEGIN_DECLS

#define CM_TYPE_OLM (cm_olm_get_type ())

G_DECLARE_FINAL_TYPE (CmOlm, cm_olm, CM, OLM, GObject)

CmOlm      *cm_olm_new_from_pickle     (char           *pickle,
                                        const char     *pickle_key,
                                        const char     *sender_identity_key,
                                        CmSessionType   session_type);
CmOlm      *cm_olm_outbound_new        (gpointer        olm_account,
                                        const char     *curve_key,
                                        const char     *one_time_key,
                                        const char     *room_id);
CmOlm      *cm_olm_inbound_new         (gpointer        olm_account,
                                        const char     *sender_identity_key,
                                        const char     *one_time_key_message);
CmOlm      *cm_olm_in_group_new        (const char     *session_key,
                                        const char     *sender_identity_key,
                                        const char     *session_id);
CmOlm      *cm_olm_in_group_new_from_out (CmOlm          *out_group,
                                          const char     *sender_identity_key);
CmOlm      *cm_olm_out_group_new       (void);

CmSessionType cm_olm_get_session_type    (CmOlm          *self);
size_t      cm_olm_get_message_index   (CmOlm          *self);

void        cm_olm_set_sender_details  (CmOlm          *self,
                                        const char     *room_id,
                                        const char     *sender_id);
void        cm_olm_set_account_details (CmOlm          *self,
                                        const char     *account_user_id,
                                        const char     *account_device_id);
void        cm_olm_set_db              (CmOlm          *self,
                                        gpointer        cm_db);
void        cm_olm_set_key             (CmOlm          *self,
                                        const char     *key);
gboolean    cm_olm_save                (CmOlm          *self);
char       *cm_olm_encrypt             (CmOlm          *self,
                                        const char     *plain_text);
char       *cm_olm_decrypt             (CmOlm          *self,
                                        size_t          type,
                                        const char     *message);
size_t      cm_olm_get_message_type    (CmOlm          *self);

const char *cm_olm_get_session_id        (CmOlm        *self);
const char *cm_olm_get_session_key       (CmOlm        *self);

gpointer    cm_olm_match_olm_session     (const char  *body,
                                          gsize        body_len,
                                          size_t       message_type,
                                          const char  *pickle,
                                          const char  *pickle_key,
                                          char       **out_decrypted);

G_END_DECLS
