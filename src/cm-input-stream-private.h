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

CmInputStream  *cm_input_stream_new                   (GInputStream        *base_stream);
CmInputStream  *cm_input_stream_new_from_file         (GFile               *file,
                                                       gboolean             encrypt,
                                                       GCancellable        *cancellable,
                                                       GError             **error);
void            cm_input_stream_set_file_enc          (CmInputStream       *self,
                                                       CmEncFileInfo       *file);
void            cm_input_stream_set_encrypt           (CmInputStream       *self);
char           *cm_input_stream_get_sha256            (CmInputStream       *self);
const char     *cm_input_stream_get_content_type      (CmInputStream       *self);
goffset         cm_input_stream_get_size              (CmInputStream       *self);
JsonObject     *cm_input_stream_get_file_json         (CmInputStream       *self);

G_END_DECLS
