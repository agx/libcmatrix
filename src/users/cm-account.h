/* cm-account.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "cm-user.h"

G_BEGIN_DECLS

#define CM_TYPE_ACCOUNT (cm_account_get_type ())

G_DECLARE_FINAL_TYPE (CmAccount, cm_account, CM, ACCOUNT, CmUser)

gboolean      cm_account_set_login_id                 (CmAccount           *self,
                                                       const char          *login_id);
const char   *cm_account_get_login_id                 (CmAccount           *self);
void          cm_account_set_display_name_async       (CmAccount           *self,
                                                       const char          *name,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_account_set_display_name_finish      (CmAccount           *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
void          cm_account_set_user_avatar_async        (CmAccount           *self,
                                                       GFile               *image_file,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_account_set_user_avatar_finish       (CmAccount           *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
void          cm_account_get_3pids_async              (CmAccount           *self,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_account_get_3pids_finish             (CmAccount           *self,
                                                       GPtrArray          **emails,
                                                       GPtrArray          **phones,
                                                       GAsyncResult        *result,
                                                       GError             **error);
void          cm_account_delete_3pid_async            (CmAccount           *self,
                                                       const char          *value,
                                                       const char          *type,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_account_delete_3pid_finish           (CmAccount           *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);

G_END_DECLS
