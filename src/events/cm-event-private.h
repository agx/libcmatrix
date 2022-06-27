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

G_BEGIN_DECLS

#define CM_TYPE_EVENT (cm_event_get_type ())

G_DECLARE_DERIVABLE_TYPE (CmEvent, cm_event, CM, EVENT, GObject)

struct _CmEventClass
{
  GObjectClass parent_class;
};

const char *cm_event_get_id            (CmEvent    *self);
void        cm_event_set_id            (CmEvent    *self,
                                        const char *id);
const char *cm_event_get_json          (CmEvent    *self);
const char *cm_event_get_original_json (CmEvent    *self);
gboolean    cm_event_is_encrypted      (CmEvent    *self);
gboolean    cm_event_is_verified       (CmEvent    *self);

G_END_DECLS
