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

/*
 * The order of the enum items SHOULD NEVER
 * be changed as they are used in database.
 * New items should be appended to the end.
 */
typedef enum {
  CM_M_UNKNOWN,
  CM_M_ANNOTATION,
  CM_M_CALL_ANSWER,
  CM_M_CALL_CANDIDATES,
  CM_M_CALL_HANGUP,
  CM_M_CALL_INVITE,
  CM_M_CALL_SELECT_ANSWER,
  CM_M_FULLY_READ,
  CM_M_NEW_DEVICE,
  CM_M_POLICY_RULE_ROOM,
  CM_M_POLICY_RULE_SERVER,
  CM_M_POLICY_RULE_USER,
  CM_M_REACTION,
  CM_M_READ,
  CM_M_RECEIPT,
  CM_M_ROOM_AVATAR,
  CM_M_ROOM_CANONICAL_ALIAS,
  CM_M_ROOM_CREATE,
  CM_M_ROOM_ENCRYPTED,
  CM_M_ROOM_ENCRYPTION,
  CM_M_ROOM_GUEST_ACCESS,
  CM_M_ROOM_HISTORY_VISIBILITY,
  CM_M_ROOM_JOIN_RULES,
  CM_M_ROOM_KEY,
  CM_M_ROOM_KEY_REQUEST,
  CM_M_ROOM_MEMBER,
  CM_M_ROOM_MESSAGE,
  CM_M_ROOM_MESSAGE_FEEDBACK,
  CM_M_ROOM_NAME,
  CM_M_ROOM_PINNED_MS,
  CM_M_ROOM_POWER_LEVELS,
  CM_M_ROOM_REDACTION,
  CM_M_ROOM_RELATED_GROUPS,
  CM_M_ROOM_SERVER_ACL,
  CM_M_ROOM_THIRD_PARTY_INVITE,
  CM_M_ROOM_TOMBSTONE,
  CM_M_ROOM_TOPIC,
  CM_M_SERVER_NOTICE,
  CM_M_TYPING,

  /* Custom */
  CM_M_USER_STATUS = 256,
} CmEventType;

typedef enum
{
  CM_EVENT_STATE_UNKNOWN,
  CM_EVENT_STATE_DRAFT,
  CM_EVENT_STATE_RECEIVED,
  /* When saving to db consider this as failed until sent? */
  CM_EVENT_STATE_SENDING,
  CM_EVENT_STATE_SENT,
  CM_EVENT_STATE_SENDING_FAILED,
} CmEventState;
