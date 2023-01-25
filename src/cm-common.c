/* cm-common.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cm-common.h"

/**
 * matrix_error_quark:
 *
 * Get the Matrix Error Quark.
 *
 * Returns: a #GQuark.
 **/
G_DEFINE_QUARK (cm-error-quark, cm_error)
