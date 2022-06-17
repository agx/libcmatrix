/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-input-stream-private.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>

#include "cm-enc-private.h"

G_BEGIN_DECLS

#define CM_TYPE_INPUT_STREAM (cm_input_stream_get_type ())

G_DECLARE_FINAL_TYPE (CmInputStream, cm_input_stream, CM, INPUT_STREAM, GFilterInputStream)

CmInputStream *cm_input_stream_new          (GInputStream  *base_stream);
void           cm_input_stream_set_file_enc (CmInputStream *self,
                                             CmEncFileInfo *file);
void           cm_input_stream_set_encrypt  (CmInputStream *self);

G_END_DECLS
