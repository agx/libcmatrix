/* cm-room-event.h
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

#include "cm-event.h"
#include "cm-types.h"

G_BEGIN_DECLS

#define CM_TYPE_ROOM_EVENT (cm_room_event_get_type ())

G_DECLARE_DERIVABLE_TYPE (CmRoomEvent, cm_room_event, CM, ROOM_EVENT, CmEvent)

struct _CmRoomEventClass
{
  CmEventClass parent_class;

  /*< private >*/
  gpointer reserved[8];
};

CmRoom *cm_room_event_get_room        (CmRoomEvent *self);
CmUser *cm_room_event_get_room_member (CmRoomEvent *self);

G_END_DECLS
