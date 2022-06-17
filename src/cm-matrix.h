/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>
#include "cm-client.h"

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

G_BEGIN_DECLS

#define CM_TYPE_MATRIX (cm_matrix_get_type ())
G_DECLARE_FINAL_TYPE (CmMatrix, cm_matrix, CM, MATRIX, GObject)

void        cm_matrix_open_async         (CmMatrix            *self,
                                          const char          *db_path,
                                          const char          *db_name,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data);
gboolean    cm_matrix_open_finish        (CmMatrix            *self,
                                          GAsyncResult        *result,
                                          GError             **error);
gboolean    cm_matrix_is_ready           (CmMatrix            *self);
CmClient   *cm_matrix_client_new         (CmMatrix            *self);

G_END_DECLS
