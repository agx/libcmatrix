/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#if !GLIB_CHECK_VERSION(2, 66, 0)
# error "libcmatrix requires glib-2.0 >= 2.66.0"
#endif

#ifndef CMATRIX_USE_EXPERIMENTAL_API
# error "libcmatrix API is experimental, define CMATRIX_USE_EXPERIMENTAL_API to use "
#endif

#define _CMATRIX_TAKEN

#include "users/cm-user.h"
#include "users/cm-account.h"
#include "cm-enums.h"
#include "cm-common.h"
#include "cm-client.h"
#include "cm-matrix.h"
#include "cm-room.h"
#include "events/cm-room-message-event.h"
#include "events/cm-verification-event.h"
/* these are not yet public */
/* #include "cm-room-member.h" */
/* #include "cm-device.h" */

#undef _CMATRIX_TAKEN

G_END_DECLS
