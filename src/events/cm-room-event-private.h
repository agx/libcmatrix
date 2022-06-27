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

#include "cm-event-private.h"

G_BEGIN_DECLS

#define CM_TYPE_ROOM_EVENT (cm_room_event_get_type ())

G_DECLARE_DERIVABLE_TYPE (CmRoomEvent, cm_room_event, CM, ROOM_EVENT, CmEvent)

struct _CmRoomEventClass
{
  CmEventClass parent_class;
};

const char *cm_room_event_get_sender (CmRoomEvent *self);
void        cm_room_event_set_sender (CmRoomEvent *self,
                                      const char  *sender);
const char *cm_room_event_get_id     (CmRoomEvent *self);
void        cm_room_event_create_id  (CmRoomEvent *self,
                                      guint          id);

G_END_DECLS
