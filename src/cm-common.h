/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-common.h
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

#define CM_ERROR (cm_error_quark ())
GQuark      cm_error_quark                          (void);
