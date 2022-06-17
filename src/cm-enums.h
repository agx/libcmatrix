/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-enums.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once


/**
 * CmError:
 *
 * The Error returned by the Matrix Server
 * See https://matrix.org/docs/spec/client_server/r0.6.1#api-standards
 * for details.
 */
typedef enum {
  M_FORBIDDEN = 1,
  M_UNKNOWN_TOKEN,
  M_MISSING_TOKEN,
  M_BAD_JSON,
  M_NOT_JSON,
  M_NOT_FOUND,
  M_LIMIT_EXCEEDED,
  M_UNKNOWN,
  M_UNRECOGNIZED,
  M_UNAUTHORIZED,
  M_USER_DEACTIVATED,
  M_USER_IN_USE,
  M_INVALID_USERNAME,
  M_ROOM_IN_USE,
  M_INVALID_ROOM_STATE,
  M_THREEPID_IN_USE,
  M_THREEPID_NOT_FOUND,
  M_THREEPID_AUTH_FAILED,
  M_THREEPID_DENIED,
  M_SERVER_NOT_TRUSTED,
  M_UNSUPPORTED_ROOM_VERSION,
  M_INCOMPATIBLE_ROOM_VERSION,
  M_BAD_STATE,
  M_GUEST_ACCESS_FORBIDDEN,
  M_CAPTCHA_NEEDED,
  M_CAPTCHA_INVALID,
  M_MISSING_PARAM,
  M_INVALID_PARAM,
  M_TOO_LARGE,
  M_EXCLUSIVE,
  M_RESOURCE_LIMIT_EXCEEDED,
  M_CANNOT_LEAVE_SERVER_NOTICE_ROOM,

  /* Local errors */
  M_BAD_PASSWORD,
  M_NO_HOME_SERVER,
  M_BAD_HOME_SERVER,
} CmError;


/*
 * CM_BLUE_PILL and CM_RED_PILL
 * are objects than actions
 */
typedef enum {
  /* When nothing real is happening */
  CM_BLUE_PILL,  /* For no/unknown command */
  CM_GET_HOMESERVER,
  CM_VERIFY_HOMESERVER,
  CM_PASSWORD_LOGIN,
  CM_ACCESS_TOKEN_LOGIN,
  CM_UPLOAD_KEY,
  CM_GET_JOINED_ROOMS,
  CM_SET_TYPING,
  CM_SEND_MESSAGE,
  CM_SEND_IMAGE,
  CM_SEND_VIDEO,
  CM_SEND_FILE,
  /* sync: plugged into the Matrix from the real world */
  /* TODO? */
  /* CM_RED_PILL = 255, */
  CM_RED_PILL,
} CmAction;

/* typedef enum { */
/*   CM_UNKNOWN, */
/*   CM_LOGGING_IN, */
/*   CM_LOGGED_IN, */
/*   CM_BAD_PASSWORD, */
/*   CM_BAD_HOMESERVER, */
/* } CmState; */

typedef enum {
  CM_ROOM_UNKNOWN,
  CM_ROOM_JOINED,
  CM_ROOM_LEFT,
  CM_ROOM_INVITED
} CmRoomType;

/* TODO */
/* typedef enum { */
/*   CM_MESSAGE_TYPE_AUDIO, */
/*   CM_MESSAGE_TYPE_EMOTE, */
/*   CM_MESSAGE_TYPE_FILE, */
/*   CM_MESSAGE_TYPE_IMAGE, */
/*   CM_MESSAGE_TYPE_LOCATION, */
/*   CM_MESSAGE_TYPE_TEXT */
/* } CmMessageType; */
