/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-event.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "cm-types.h"
#include "cm-enums.h"

G_BEGIN_DECLS

#define CM_TYPE_EVENT (cm_event_get_type ())

G_DECLARE_DERIVABLE_TYPE (CmEvent, cm_event, CM, EVENT, GObject)

struct _CmEventClass
{
  GObjectClass   parent_class;

  /*< private >*/
  gpointer     (*generate_json) (CmEvent     *self,
                                 gpointer     room);
  char        *(*get_api_url)   (CmEvent     *self,
                                 gpointer     room);
  gpointer       reserved[8];
};

const char   *cm_event_get_id            (CmEvent      *self);
CmUser       *cm_event_get_sender        (CmEvent      *self);
CmEventType   cm_event_get_m_type        (CmEvent      *self);
CmEventState  cm_event_get_state         (CmEvent      *self);
gint64        cm_event_get_time_stamp    (CmEvent      *self);
gboolean      cm_event_is_encrypted      (CmEvent      *self);

G_END_DECLS
