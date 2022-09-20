/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-user-list-private.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <json-glib/json-glib.h>
#include <glib-object.h>

#include "cm-types.h"

G_BEGIN_DECLS

typedef struct _CmUserKey CmUserKey;

struct _CmUserKey {
  CmUser *user;
  GPtrArray *devices;
  GPtrArray *keys;
};

#define CM_TYPE_USER_LIST (cm_user_list_get_type ())

G_DECLARE_FINAL_TYPE (CmUserList, cm_user_list, CM, USER_LIST, GObject)

void          cm_user_key_free                     (gpointer             data);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CmUserKey, cm_user_key_free)

CmUserList   *cm_user_list_new                     (CmClient            *self);
void          cm_user_list_device_changed          (CmUserList          *self,
                                                    JsonObject          *root,
                                                    GPtrArray           *changed);
void          cm_user_list_set_account             (CmUserList          *self,
                                                    CmAccount           *account);
CmUser       *cm_user_list_find_user               (CmUserList          *self,
                                                    GRefString          *user_id,
                                                    gboolean             create_if_missing);
void          cm_user_list_load_devices_async      (CmUserList          *self,
                                                    GPtrArray           *users,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
GPtrArray    *cm_user_list_load_devices_finish     (CmUserList          *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void          cm_user_list_claim_keys_async        (CmUserList          *self,
                                                    CmRoom              *room,
                                                    GHashTable          *users,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
GPtrArray    *cm_user_list_claim_keys_finish       (CmUserList          *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void          cm_user_list_upload_keys_async       (CmUserList          *self,
                                                    CmRoom              *room,
                                                    GPtrArray           *one_time_keys,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean      cm_user_list_upload_keys_finish      (CmUserList          *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);

G_END_DECLS
