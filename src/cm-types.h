/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-types.h
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

#include <glib.h>

G_BEGIN_DECLS

typedef struct _CmUser              CmUser;
typedef struct _CmAccount           CmAccount;
typedef struct _CmRoomMember        CmRoomMember;
typedef struct _CmClient            CmClient;
typedef struct _CmRoom              CmRoom;
typedef struct _CmEvent             CmEvent;
typedef struct _CmRoomEvent         CmRoomEvent;
typedef struct _CmRoomMessageEvent  CmRoomMessageEvent;

/* Private types */
#ifdef CMATRIX_COMPILATION
typedef struct _CmDb                CmDb;
typedef struct _CmOlm               CmOlm;
typedef struct _CmUserList          CmUserList;
#endif /* CMATRIX_COMPILATION */

G_END_DECLS
