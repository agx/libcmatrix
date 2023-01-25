/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>
#include "cm-matrix.h"
#include "cm-db-private.h"

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

G_BEGIN_DECLS

const char  *cm_matrix_get_data_dir   (void);
const char  *cm_matrix_get_app_id     (void);

/* To be used only for tests */
CmDb *cm_matrix_get_db (CmMatrix *self);

G_END_DECLS
