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

G_BEGIN_DECLS

#define CM_TYPE_OLM (cm_olm_get_type ())

G_DECLARE_FINAL_TYPE (CmOlm, cm_olm, CM, OLM, GObject)

gpointer    cm_olm_steal_session       (CmOlm          *self,
                                        CmSessionType   type);
CmOlm      *cm_olm_outbound_new        (gpointer        olm_account,
                                        const char     *curve_key,
                                        const char     *one_time_key,
                                        const char     *room_id);
CmOlm      *cm_olm_out_group_new       (void);
void        cm_olm_set_details         (CmOlm          *self,
                                        const char     *room_id,
                                        const char     *sender_id,
                                        const char     *device_id);
void        cm_olm_set_db              (CmOlm          *self,
                                        gpointer        cm_db);
void        cm_olm_set_key             (CmOlm          *self,
                                        const char     *key);
gboolean    cm_olm_save                (CmOlm          *self);

gpointer    cm_olm_match_olm_session     (const char  *body,
                                          gsize        body_len,
                                          size_t       message_type,
                                          const char  *pickle,
                                          const char  *pickle_key,
                                          char       **out_decrypted);

G_END_DECLS
