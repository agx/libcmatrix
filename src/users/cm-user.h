/* cm-user.h
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

G_BEGIN_DECLS

#define CM_TYPE_USER (cm_user_get_type ())

G_DECLARE_DERIVABLE_TYPE (CmUser, cm_user, CM, USER, GObject)

struct _CmUserClass
{
  GObjectClass parent_class;

  /*< private >*/
  gpointer reserved[8];
};

GRefString   *cm_user_get_id                  (CmUser              *self);
const char   *cm_user_get_display_name        (CmUser              *self);
const char   *cm_user_get_avatar_url          (CmUser              *self);
void          cm_user_get_avatar_async        (CmUser              *self,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
GInputStream *cm_user_get_avatar_finish       (CmUser              *self,
                                               GAsyncResult        *result,
                                               GError             **error);
void          cm_user_load_info_async         (CmUser              *self,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
gboolean      cm_user_load_info_finish        (CmUser              *self,
                                               GAsyncResult        *result,
                                               GError             **error);

G_END_DECLS
