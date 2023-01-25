/* cm-olm-sas-private.h
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

#include "cm-device.h"
#include "events/cm-event.h"

G_BEGIN_DECLS

#define CM_TYPE_OLM_SAS (cm_olm_sas_get_type ())

G_DECLARE_FINAL_TYPE (CmOlmSas, cm_olm_sas, CM, OLM_SAS, GObject)

CmOlmSas      *cm_olm_sas_new                   (void);
void           cm_olm_sas_set_client            (CmOlmSas     *self,
                                                 gpointer      cm_client);
void           cm_olm_sas_set_key_verification  (CmOlmSas            *self,
                                                 CmVerificationEvent *event);
gboolean       cm_olm_sas_matches_event         (CmOlmSas            *self,
                                                 CmVerificationEvent *event);
const char    *cm_olm_sas_get_cancel_code       (CmOlmSas     *self);
CmEvent       *cm_olm_sas_get_cancel_event      (CmOlmSas     *self,
                                                 const char   *cancel_code);
CmEvent       *cm_olm_sas_get_ready_event       (CmOlmSas     *self);
CmEvent       *cm_olm_sas_get_accept_event      (CmOlmSas     *self);
CmEvent       *cm_olm_sas_get_key_event         (CmOlmSas     *self);
GPtrArray     *cm_olm_sas_get_emojis            (CmOlmSas     *self);
CmEvent       *cm_olm_sas_get_mac_event         (CmOlmSas     *self);
CmEvent       *cm_olm_sas_get_done_event        (CmOlmSas     *self);
gboolean       cm_olm_sas_is_verified           (CmOlmSas     *self);
CmDevice      *cm_olm_sas_get_device            (CmOlmSas     *self);

G_END_DECLS
