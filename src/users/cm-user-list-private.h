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

#include <glib-object.h>

#include "cm-types.h"

G_BEGIN_DECLS

#define CM_TYPE_USER_LIST (cm_user_list_get_type ())

G_DECLARE_FINAL_TYPE (CmUserList, cm_user_list, CM, USER_LIST, GObject)

CmUserList   *cm_user_list_new                     (CmClient            *self);
CmUser       *cm_user_list_find_user               (CmUserList          *self,
                                                    GRefString          *user_id,
                                                    gboolean             create_if_missing);

G_END_DECLS
