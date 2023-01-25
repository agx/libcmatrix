/*
 * Copyright (C) 2022 Purism SPC
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

#define CM_TYPE_DEVICE (cm_device_get_type ())

G_DECLARE_FINAL_TYPE (CmDevice, cm_device, CM, DEVICE, GObject)

const char   *cm_device_get_id                  (CmDevice *self);
const char   *cm_device_get_ed_key              (CmDevice *self);
const char   *cm_device_get_curve_key           (CmDevice *self);

G_END_DECLS
