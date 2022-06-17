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

#include "cm-room-message.h"

G_BEGIN_DECLS

void  cm_room_message_set_transaction_id (CmRoomMessage *self,
                                          const char    *transaction_id);

G_END_DECLS
