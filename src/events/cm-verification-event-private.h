/*
 * Copyright (C) 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <json-glib/json-glib.h>

#include "cm-event-private.h"

G_BEGIN_DECLS

CmVerificationEvent *cm_verification_event_new                  (gpointer              client);
void                 cm_verification_event_set_json             (CmVerificationEvent  *self,
                                                                 JsonObject           *root);
const char          *cm_verification_event_get_transaction_id   (CmVerificationEvent  *self);
const char          *cm_verification_event_get_verification_key (CmVerificationEvent  *self);
void                 cm_verification_event_done_async           (CmVerificationEvent  *self,
                                                                 GCancellable         *cancellable,
                                                                 GAsyncReadyCallback   callback,
                                                                 gpointer              user_data);

G_END_DECLS

