/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <json-glib/json-glib.h>

#include "cm-types.h"
#include "cm-event.h"

G_BEGIN_DECLS

/* The order if the enum SHOULD NEVER be changed,
 * as it's used in db
 */
typedef enum {
  CM_RELATION_NONE,
  CM_RELATION_UNKNOWN,
  CM_RELATION_ANNOTATION,
  CM_RELATION_REPLACE,
  CM_RELATION_REFERENCE,
  CM_RELATION_THREAD
} CmRelationType;

CmEvent      *cm_event_new                (CmEventType   type);
CmEvent      *cm_event_new_from_json      (JsonObject   *root,
                                           JsonObject   *encrypted);
void          cm_event_set_state          (CmEvent      *self,
                                           CmEventState  state);
const char   *cm_event_get_transaction_id   (CmEvent      *self);
const char   *cm_event_get_verification_key (CmEvent        *self);
const char   *cm_event_get_txn_id         (CmEvent      *self);
void          cm_event_create_txn_id      (CmEvent      *self,
                                           guint         id);
const char   *cm_event_get_state_key      (CmEvent      *self);
void          cm_event_set_id             (CmEvent      *self,
                                           const char   *id);
const char   *cm_event_get_replaces_id    (CmEvent      *self);
const char   *cm_event_get_reply_to_id    (CmEvent      *self);
void          cm_event_set_m_type         (CmEvent      *self,
                                           CmEventType   type);
void          cm_event_set_json           (CmEvent      *self,
                                           JsonObject   *root,
                                           JsonObject   *encrypted);
GRefString   *cm_event_get_sender_id      (CmEvent      *self);
void          cm_event_set_sender         (CmEvent      *self,
                                           CmUser       *sender);
const char   *cm_event_get_sender_device_id (CmEvent    *self);
gboolean      cm_event_has_encrypted_content (CmEvent       *self);
gboolean      cm_event_is_decrypted          (CmEvent       *self);

char         *cm_event_get_json_str       (CmEvent      *self,
                                           gboolean      prettify);
JsonObject   *cm_event_get_json           (CmEvent      *self);
JsonObject   *cm_event_get_encrypted_json (CmEvent      *self);
JsonObject   *cm_event_generate_json      (CmEvent      *self,
                                           gpointer      room);
char         *cm_event_get_api_url         (CmEvent      *self,
                                           gpointer      room);
const char *cm_event_get_original_json (CmEvent    *self);
gboolean    cm_event_is_verified       (CmEvent    *self);

G_END_DECLS
