/*
 * Copyright 2024 The Phosh Developers
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

/**
 * CmPusherKind:
 * @CM_PUSHER_KIND_UNKNOWN: The type of the pusher is unknown
 * @CM_PUSHER_KIND_HTTP: A pusher that forwards messages via http
 * @CM_PUSHER_KIND_EMAIL: A pusher that forwards messages via email
 *
 * The type of a [class@Pusher]
 */
typedef enum _CmPusherKind {
  CM_PUSHER_KIND_UNKNOWN  = 0,
  CM_PUSHER_KIND_HTTP     = 1,
  CM_PUSHER_KIND_EMAIL    = 2,
} CmPusherKind;

#define CM_TYPE_PUSHER (cm_pusher_get_type ())
G_DECLARE_FINAL_TYPE (CmPusher, cm_pusher, CM, PUSHER, GObject)

CmPusher     *cm_pusher_new                           (void);
CmPusherKind  cm_pusher_get_kind                      (CmPusher *self);
const char   *cm_pusher_get_kind_as_string            (CmPusher *self);
void          cm_pusher_set_kind                      (CmPusher *self, CmPusherKind kind);
void          cm_pusher_set_kind_from_string          (CmPusher *self, const char *kind);
const char   *cm_pusher_get_app_display_name          (CmPusher *self);
void          cm_pusher_set_app_display_name          (CmPusher *self, const char *app_display_name);
const char   *cm_pusher_get_app_id                    (CmPusher *self);
void          cm_pusher_set_app_id                    (CmPusher *self, const char *app_id);
const char   *cm_pusher_get_device_display_name       (CmPusher *self);
void          cm_pusher_set_device_display_name       (CmPusher *self, const char *device_display_name);
const char   *cm_pusher_get_lang                      (CmPusher *self);
void          cm_pusher_set_lang                      (CmPusher *self, const char *lang);
const char   *cm_pusher_get_profile_tag               (CmPusher *self);
void          cm_pusher_set_profile_tag               (CmPusher *self, const char *profile_tag);
const char   *cm_pusher_get_pushkey                   (CmPusher *self);
void          cm_pusher_set_pushkey                   (CmPusher *self, const char *pushkey);

/* http pushers only: */
const char   *cm_pusher_get_url                       (CmPusher *self);
void          cm_pusher_set_url                       (CmPusher *self, const char *url);
void          cm_pusher_check_valid                   (CmPusher *self,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_pusher_check_valid_finish            (CmPusher *self,
                                                       GAsyncResult  *result,
                                                       GError       **error);

G_END_DECLS
