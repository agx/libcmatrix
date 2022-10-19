/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "cm-event.h"

G_BEGIN_DECLS

#define CM_TYPE_VERIFICATION_EVENT (cm_verification_event_get_type ())

G_DECLARE_FINAL_TYPE (CmVerificationEvent, cm_verification_event, CM, VERIFICATION_EVENT, CmEvent)

void          cm_verification_event_cancel_async     (CmVerificationEvent *self,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
gboolean      cm_verification_event_cancel_finish    (CmVerificationEvent *self,
                                                      GAsyncResult        *result,
                                                      GError             **error);
void          cm_verification_event_continue_async   (CmVerificationEvent *self,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
gboolean      cm_verification_event_continue_finish  (CmVerificationEvent *self,
                                                      GAsyncResult        *result,
                                                      GError             **error);
void          cm_verification_event_match_async      (CmVerificationEvent *self,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
gboolean      cm_verification_event_match_finish     (CmVerificationEvent *self,
                                                      GAsyncResult        *result,
                                                      GError             **error);

G_END_DECLS
