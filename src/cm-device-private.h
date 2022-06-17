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

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "cm-device.h"

G_BEGIN_DECLS

CmDevice *cm_device_new                (gpointer    client,
                                        JsonObject *root);

void      cm_device_set_one_time_key   (CmDevice   *self,
                                        const char *key);
char     *cm_device_steal_one_time_key (CmDevice *self);

G_END_DECLS
