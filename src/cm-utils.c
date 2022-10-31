/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-utils"
#define BUFFER_SIZE 256

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define __STDC_WANT_LIB_EXT1__ 1
#include <stdio.h>
#include <string.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "cm-client-private.h"
#include "cm-common.h"
#include "cm-enc-private.h"
#include "cm-enums.h"
#include "cm-utils-private.h"

static const char *error_codes[] = {
  "", /* Index 0 is reserved for no error */
  "M_FORBIDDEN",
  "M_UNKNOWN_TOKEN",
  "M_MISSING_TOKEN",
  "M_BAD_JSON",
  "M_NOT_JSON",
  "M_NOT_FOUND",
  "M_LIMIT_EXCEEDED",
  "M_UNKNOWN",
  "M_UNRECOGNIZED",
  "M_UNAUTHORIZED",
  "M_USER_DEACTIVATED",
  "M_USER_IN_USE",
  "M_INVALID_USERNAME",
  "M_ROOM_IN_USE",
  "M_INVALID_ROOM_STATE",
  "M_THREEPID_IN_USE",
  "M_THREEPID_NOT_FOUND",
  "M_THREEPID_AUTH_FAILED",
  "M_THREEPID_DENIED",
  "M_SERVER_NOT_TRUSTED",
  "M_UNSUPPORTED_ROOM_VERSION",
  "M_INCOMPATIBLE_ROOM_VERSION",
  "M_BAD_STATE",
  "M_GUEST_ACCESS_FORBIDDEN",
  "M_CAPTCHA_NEEDED",
  "M_CAPTCHA_INVALID",
  "M_MISSING_PARAM",
  "M_INVALID_PARAM",
  "M_TOO_LARGE",
  "M_EXCLUSIVE",
  "M_RESOURCE_LIMIT_EXCEEDED",
  "M_CANNOT_LEAVE_SERVER_NOTICE_ROOM",
};

const char *
cm_utils_log_bool_str (gboolean value,
                       gboolean use_success)
{
  if (!g_log_writer_supports_color (fileno (stdout)) ||
      g_log_writer_is_journald (fileno (stderr)))
    {
      if (value)
        return use_success ? "success" : "true";
      else
        return use_success ? "fail" : "false";
    }

  if (value)
    {
      if (use_success)
        return "\033[1;32m" "success" "\033[0m";
      else
        return "\033[1;32m" "true" "\033[0m";
    }
  else
    {
      if (use_success)
        return "\033[1;31m" "fail" "\033[0m";
      else
        return "\033[1;31m" "false" "\033[0m";
    }
}

const char *
cm_utils_anonymize (GString    *str,
                    const char *value)
{
  gunichar c, next_c, prev_c;

  g_assert (str);

  if (!value || !*value)
    return str->str;

  if (str->len && str->str[str->len - 1] != ' ')
    g_string_append_c (str, ' ');

  if (!g_utf8_validate (value, -1, NULL))
    {
      g_string_append (str, "******");
      return str->str;
    }

  if (*value == '!' || *value == '@' || *value == '+')
    {
      c = g_utf8_get_char (value);
      value = g_utf8_next_char (value);
      g_string_append_unichar (str, c);
    }

  if (!*value)
    return str->str;

  c = g_utf8_get_char (value);
  value = g_utf8_next_char (value);
  g_string_append_unichar (str, c);

  if (!*value)
    return str->str;

  c = g_utf8_get_char (value);
  value = g_utf8_next_char (value);
  g_string_append_unichar (str, c);

  while (*value)
    {
      prev_c = c;
      c = g_utf8_get_char (value);

      value = g_utf8_next_char (value);
      next_c = g_utf8_get_char (value);

      if (!g_unichar_isalnum (c))
        g_string_append_unichar (str, c);
      else if (!g_unichar_isalnum (prev_c) || !g_unichar_isalnum (next_c))
        g_string_append_unichar (str, c);
      else
        g_string_append_c (str, '#');
    }

  return str->str;
}

GError *
cm_utils_json_node_get_error (JsonNode *node)
{
  JsonObject *object = NULL;
  const char *error, *err_code;

  if (!node || (!JSON_NODE_HOLDS_OBJECT (node) && !JSON_NODE_HOLDS_ARRAY (node)))
    return g_error_new (CM_ERROR, CM_ERROR_NOT_JSON,
                        "Not JSON Object");

  /* Returned by /_matrix/client/r0/rooms/{roomId}/state */
  if (JSON_NODE_HOLDS_ARRAY (node))
    return NULL;

  object = json_node_get_object (node);
  err_code = cm_utils_json_object_get_string (object, "errcode");

  if (!err_code)
    return NULL;

  error = cm_utils_json_object_get_string (object, "error");

  if (!error)
    error = "Unknown Error";

  if (!g_str_has_prefix (err_code, "M_"))
    return g_error_new (CM_ERROR, CM_ERROR_UNKNOWN,
                        "Invalid Error code");

  for (guint i = 0; i < G_N_ELEMENTS (error_codes); i++)
    if (g_str_equal (error_codes[i], err_code))
      return g_error_new (CM_ERROR, i, "%s", error);

  return g_error_new (CM_ERROR, CM_ERROR_UNKNOWN, "Unknown Error");
}

gboolean
cm_utils_get_item_position (GListModel *list,
                            gpointer    item,
                            guint      *position)
{
  guint n_items;

  g_return_val_if_fail (G_IS_LIST_MODEL (list), FALSE);
  g_return_val_if_fail (item != NULL, FALSE);

  n_items = g_list_model_get_n_items (list);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(GObject) object = NULL;

      object = g_list_model_get_item (list, i);

      if (object == item)
        {
          if (position)
            *position = i;

          return TRUE;
        }
    }

  return FALSE;
}

gboolean
cm_utils_remove_list_item (GListStore *store,
                           gpointer    item)
{
  GListModel *model;
  guint n_items;

  g_return_val_if_fail (G_IS_LIST_STORE (store), FALSE);

  if (!item)
    return FALSE;

  model = G_LIST_MODEL (store);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(GObject) object = NULL;

      object = g_list_model_get_item (model, i);

      if (object == item)
        {
          g_list_store_remove (store, i);

          return TRUE;
        }
    }

  return FALSE;
}

const char *
cm_utils_get_event_type_str (CmEventType type)
{
  switch (type)
    {
    case CM_M_CALL_ANSWER:
      return "m.call.answer";

    case CM_M_CALL_ASSERTED_IDENTITY:
      return "m.call.asserted_identity";

    case CM_M_CALL_ASSERTED_IDENTITY_PREFIX:
      return "org.matrix.call.asserted_identity";

    case CM_M_CALL_CANDIDATES:
      return "m.call.candidates";

    case CM_M_CALL_HANGUP:
      return "m.call.hangup";

    case CM_M_CALL_INVITE:
      return "m.call.invite";

    case CM_M_CALL_NEGOTIATE:
      return "m.call.negotiate";

    case CM_M_CALL_REJECT:
      return "m.call.reject";

    case CM_M_CALL_REPLACES:
      return "m.call.replaces";

    case CM_M_CALL_SELECT_ANSWER:
      return "m.call.select_answer";

    case CM_M_DIRECT:
      return "m.direct";

    case CM_M_DUMMY:
      return "m.dummy";

    case CM_M_FORWARDED_ROOM_KEY:
      return "m.forwarded_room_key";

    case CM_M_FULLY_READ:
      return "m.fully_read";

    case CM_M_IGNORED_USER_LIST:
      return "m.ignored_user_list";

    case CM_M_KEY_VERIFICATION_ACCEPT:
      return "m.key.verification_accept";

    case CM_M_KEY_VERIFICATION_CANCEL:
      return "m.key.verification.cancel";

    case CM_M_KEY_VERIFICATION_DONE:
      return "m.key.verification.done";

    case CM_M_KEY_VERIFICATION_KEY:
      return "m.key.verification.key";

    case CM_M_KEY_VERIFICATION_MAC:
      return "m.key.verification.mac";

    case CM_M_KEY_VERIFICATION_READY:
      return "m.key.verification.ready";

    case CM_M_KEY_VERIFICATION_REQUEST:
      return "m.key.verification.request";

    case CM_M_KEY_VERIFICATION_START:
      return "m.key.verification.start";

    case CM_M_PRESENCE:
      return "m.presence";

    case CM_M_PUSH_RULES:
      return "m.push_rules";

    case CM_M_REACTION:
      return "m.reaction";

    case CM_M_RECEIPT:
      return "m.receipt";

    case CM_M_ROOM_ALIASES:
      return "m.room.aliases";

    case CM_M_ROOM_AVATAR:
      return "m.room.avatar";

    case CM_M_ROOM_BOT_OPTIONS:
      return "m.room.bot.options";

    case CM_M_ROOM_CANONICAL_ALIAS:
      return "m.room.canonical_alias";

    case CM_M_ROOM_CREATE:
      return "m.room.create";

    case CM_M_ROOM_ENCRYPTED:
      return "m.room.encrypted";

    case CM_M_ROOM_ENCRYPTION:
      return "m.room.encryption";

    case CM_M_ROOM_GUEST_ACCESS:
      return "m.room.guest_access";

    case CM_M_ROOM_HISTORY_VISIBILITY:
      return "m.room.history_visibility";

    case CM_M_ROOM_JOIN_RULES:
      return "m.room.join_rules";

    case CM_M_ROOM_KEY:
      return "m.room_key";

    case CM_M_ROOM_KEY_REQUEST:
      return "m.room_key.request";

    case CM_M_ROOM_MEMBER:
      return "m.room.member";

    case CM_M_ROOM_MESSAGE:
      return "m.room.message";

    case CM_M_ROOM_MESSAGE_FEEDBACK:
      return "m.room.message.feedback";

    case CM_M_ROOM_NAME:
      return "m.room.name";

    case CM_M_ROOM_PINNED_EVENTS:
      return "m.room.pinned_events";

    case CM_M_ROOM_PLUMBING:
      return "m.room.plumbing";

    case CM_M_ROOM_POWER_LEVELS:
      return "m.room.power_levels";

    case CM_M_ROOM_REDACTION:
      return "m.room.redaction";

    case CM_M_ROOM_RELATED_GROUPS:
      return "m.room.related_groups";

    case CM_M_ROOM_SERVER_ACL:
      return "m.room.server_acl";

    case CM_M_ROOM_THIRD_PARTY_INVITE:
      return "m.room.third_party_invite";

    case CM_M_ROOM_TOMBSTONE:
      return "m.room.tombstone";

    case CM_M_ROOM_TOPIC:
      return "m.room.topic";

    case CM_M_SECRET_REQUEST:
      return "m.secret.request";

    case CM_M_SECRET_SEND:
      return "m.secret.send";

    case CM_M_SECRET_STORAGE_DEFAULT_KEY:
      return "m.secret_storage.default_key";

    case CM_M_SPACE_CHILD:
      return "m.space.child";

    case CM_M_SPACE_PARENT:
      return "m.space.parent";

    case CM_M_STICKER:
      return "m.sticker";

    case CM_M_TAG:
      return "m.tag";

    case CM_M_TYPING:
      return "m.typing";

    case CM_M_UNKNOWN:
    case CM_M_USER_STATUS:
    case CM_M_ROOM_INVITE:
    case CM_M_ROOM_BAN:
    case CM_M_ROOM_KICK:
    default:
      g_return_val_if_reached (NULL);
    }

  return NULL;
}

void
cm_utils_clear (char   *buffer,
                size_t  length)
{
  if (!buffer || length == 0)
    return;

  /* Note: we are not comparing with -1 */
  if (length == (size_t)-1)
    length = strlen (buffer);

#ifdef __STDC_LIB_EXT1__
  memset_s (buffer, length, 0xAD, length);
#elif HAVE_EXPLICIT_BZERO
  explicit_bzero (buffer, length);
#else
  {
    volatile char *end = buffer + length;

    /* Set something nonzero, so it'll likely crash on reuse */
    while (buffer != end)
      *(buffer++) = 0xAD;
  }
#endif
}

void
cm_utils_free_buffer (char *buffer)
{
  cm_utils_clear (buffer, -1);
  g_free (buffer);
}

const char *
cm_utils_get_url_from_user_id (const char *user_id)
{
  if (!cm_utils_user_name_valid (user_id))
    return NULL;

  /* Return the string after ‘:’ */
  return strchr (user_id, ':') + 1;
}

/* https://spec.matrix.org/v1.2/appendices/#user-identifiers */
/* domain part is validated separately, so the regex is not complete */
#define MATRIX_USER_ID_RE "^@[A-Z0-9.=_-]+:[A-Z0-9.-]+$"

gboolean
cm_utils_user_name_valid (const char *matrix_user_id)
{
  const char *url_start;

  if (!matrix_user_id || !*matrix_user_id)
    return FALSE;

  /* GRegex is deprecated, but we can change this anytime:
   * https://gitlab.gnome.org/GNOME/glib/-/merge_requests/1451
   * https://gitlab.gnome.org/GNOME/glib/-/merge_requests/2529
   */
  if (!g_regex_match_simple (MATRIX_USER_ID_RE, matrix_user_id, G_REGEX_CASELESS, 0))
    return FALSE;

  url_start = strchr (matrix_user_id, ':') + 1;

  if (!cm_utils_home_server_valid (url_start))
    return FALSE;

  if (G_UNLIKELY (strlen (matrix_user_id) > 255))
    return FALSE;

  return TRUE;
}
#undef MATRIX_USER_ID_RE

#define EMAIL_RE "^[[:alnum:]._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}$"

gboolean
cm_utils_user_name_is_email (const char *user_id)
{
  if (!user_id || !*user_id)
    return FALSE;

  if (g_regex_match_simple (EMAIL_RE, user_id, G_REGEX_CASELESS, 0))
    return TRUE;

  return FALSE;
}
#undef EMAIL_RE

/* Rough estimate for an E.164 number */
#define MOBILE_RE "^\\+[0-9]{10,15}$"

gboolean
cm_utils_mobile_is_valid (const char *mobile_num)
{
  if (!mobile_num || !*mobile_num)
    return FALSE;

  if (g_regex_match_simple (MOBILE_RE, mobile_num, 0, 0))
    return TRUE;

  return FALSE;
}
#undef MOBILE_RE

gboolean
cm_utils_home_server_valid (const char *homeserver)
{
  gboolean valid = FALSE;

  if (homeserver && !*homeserver)
    homeserver = NULL;

  if (homeserver)
    {
      g_autofree char *server = NULL;
      g_autoptr(GUri) uri = NULL;
      const char *scheme = NULL;
      const char *path = NULL;
      const char *host = NULL;

      if (!strstr (homeserver, "//"))
        server = g_strconcat ("https://", homeserver, NULL);

      uri = g_uri_parse (server ?: homeserver, G_URI_FLAGS_NONE, NULL);

      if (uri)
        {
          scheme = g_uri_get_scheme (uri);
          path = g_uri_get_path (uri);
          host = g_uri_get_host (uri);
        }

      valid = scheme && *scheme;
      valid = valid && (g_str_equal (scheme, "http") || g_str_equal (scheme, "https"));
      valid = valid && host && *host;
      valid = valid && !g_str_has_suffix (host, ".");
      valid = valid && (!path || !*path || g_str_equal (path, "/"));
    }

  return valid;
}

char *
cm_utils_json_object_to_string (JsonObject *object,
                                gboolean    prettify)
{
  g_autoptr(JsonNode) node = NULL;

  g_return_val_if_fail (object, NULL);

  node = json_node_new (JSON_NODE_OBJECT);
  json_node_init_object (node, object);

  return json_to_string (node, !!prettify);
}

static void utils_json_canonical_array (JsonArray *array,
                                        GString   *out);
static void
utils_handle_node (JsonNode *node,
                   GString  *out)
{
  GType type;

  g_assert (node);
  g_assert (out);

  type = json_node_get_value_type (node);

  if (type == JSON_TYPE_OBJECT)
    cm_utils_json_get_canonical (json_node_get_object (node), out);
  else if (type == JSON_TYPE_ARRAY)
    utils_json_canonical_array (json_node_get_array (node), out);
  else if (type == G_TYPE_INVALID)
    g_string_append (out, "null");
  else if (type == G_TYPE_STRING)
    g_string_append_printf (out, "\"%s\"", json_node_get_string (node));
  else if (type == G_TYPE_INT64)
    g_string_append_printf (out, "%" G_GINT64_FORMAT, json_node_get_int (node));
  else if (type == G_TYPE_DOUBLE)
    g_string_append_printf (out, "%f", json_node_get_double (node));
  else if (type == G_TYPE_BOOLEAN)
    g_string_append (out, json_node_get_boolean (node) ? "true" : "false");
  else
    g_return_if_reached ();
}

static void
utils_json_canonical_array (JsonArray *array,
                            GString   *out)
{
  g_autoptr(GList) elements = NULL;

  g_assert (array);
  g_assert (out);

  g_string_append_c (out, '[');
  elements = json_array_get_elements (array);

  /* The order of array members shouldn’t be changed */
  for (GList *item = elements; item; item = item->next)
{
    utils_handle_node (item->data, out);

    if (item->next)
      g_string_append_c (out, ',');
  }

  g_string_append_c (out, ']');
}

GString *
cm_utils_json_get_canonical (JsonObject *object,
                             GString    *out)
{
  JsonNode *signatures, *non_signed;

  g_autoptr(GList) members = NULL;

  g_return_val_if_fail (object, NULL);

  if (!out)
    out = g_string_sized_new (BUFFER_SIZE);

  signatures = json_object_dup_member (object, "signatures");
  non_signed = json_object_dup_member (object, "unsigned");

  /* Remove the non signed members before verification */
  json_object_remove_member (object, "signatures");
  json_object_remove_member (object, "unsigned");

  g_string_append_c (out, '{');

  members = json_object_get_members (object);
  members = g_list_sort (members, (GCompareFunc)g_strcmp0);

  for (GList *item = members; item; item = item->next)
{
    JsonNode *node;

    g_string_append_printf (out, "\"%s\":", (char *)item->data);

    node = json_object_get_member (object, item->data);
    utils_handle_node (node, out);

    if (item->next)
      g_string_append_c (out, ',');
  }

  g_string_append_c (out, '}');

  /* Revert the changes we made to the JSON object */
  if (signatures)
    json_object_set_member (object, "signatures", signatures);
  if (non_signed)
    json_object_set_member (object, "unsigned", non_signed);

  return out;
}

JsonObject *
cm_utils_string_to_json_object (const char *json_str)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *node;

  if (!json_str || !*json_str)
    return NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, json_str, -1, NULL))
    return NULL;

  node = json_parser_get_root (parser);

  if (!JSON_NODE_HOLDS_OBJECT (node))
    return NULL;

  return json_node_dup_object (node);
}

gboolean
cm_utils_json_object_has_member (JsonObject *object,
                                 const char *member)
{
  JsonNode *node;

  if (!object || !member || !*member)
    return 0;

  node = json_object_get_member (object, member);

  return !!node;
}

gint64
cm_utils_json_object_get_int (JsonObject *object,
                              const char *member)
{
  JsonNode *node;

  if (!object || !member || !*member)
    return 0;

  node = json_object_get_member (object, member);

  if (node && JSON_NODE_HOLDS_VALUE (node))
    return json_node_get_int (node);

  return 0;
}

gboolean
cm_utils_json_object_get_bool (JsonObject *object,
                               const char *member)
{
  JsonNode *node;

  if (!object || !member || !*member)
    return FALSE;

  node = json_object_get_member (object, member);

  if (node && JSON_NODE_HOLDS_VALUE (node))
    return json_node_get_boolean (node);

  return FALSE;
}

const char *
cm_utils_json_object_get_string (JsonObject *object,
                                 const char *member)
{
  JsonNode *node;

  if (!object || !member || !*member)
    return NULL;

  node = json_object_get_member (object, member);

  if (node && JSON_NODE_HOLDS_VALUE (node))
    return json_node_get_string (node);

  return NULL;
}

char *
cm_utils_json_object_dup_string (JsonObject *object,
                                 const char *member)
{
  const char *str;

  str = cm_utils_json_object_get_string (object, member);

  return g_strdup (str);
}

JsonObject *
cm_utils_json_object_get_object (JsonObject *object,
                                 const char *member)
{
  JsonNode *node;

  if (!object || !member || !*member)
    return NULL;

  node = json_object_get_member (object, member);

  if (node && JSON_NODE_HOLDS_OBJECT (node))
    return json_node_get_object (node);

  return NULL;
}

JsonArray *
cm_utils_json_object_get_array (JsonObject *object,
                                const char *member)
{
  JsonNode *node;

  if (!object || !member || !*member)
    return NULL;

  node = json_object_get_member (object, member);

  if (node && JSON_NODE_HOLDS_ARRAY (node))
    return json_node_get_array (node);

  return NULL;
}

static void
load_from_stream_cb (JsonParser   *parser,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  gboolean timeout;

  g_assert (JSON_IS_PARSER (parser));
  g_assert (G_IS_TASK (task));

  timeout = GPOINTER_TO_INT (g_task_get_task_data (task));

  /* Task return is handled somewhere else */
  if (timeout)
    return;

  if (json_parser_load_from_stream_finish (parser, result, &error))
    g_task_return_pointer (task, json_parser_steal_root (parser),
                           (GDestroyNotify)json_node_unref);
  else
    g_task_return_error (task, error);
}

static gboolean
cancel_read_uri (gpointer user_data)
{
  g_autoptr(GTask) task = user_data;

  g_assert (G_IS_TASK (task));

  g_object_set_data (G_OBJECT (task), "timeout-id", 0);

  /* XXX: Not thread safe? */
  if (g_task_get_completed (task) || g_task_had_error (task))
    return G_SOURCE_REMOVE;

  g_task_set_task_data (task, GINT_TO_POINTER (TRUE), NULL);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                           "Request timeout");
  g_cancellable_cancel (g_task_get_cancellable (task));

  return G_SOURCE_REMOVE;
}

static void
uri_file_read_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  SoupSession *session = (SoupSession *)object;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GInputStream) stream = NULL;
  g_autoptr(JsonParser) parser = NULL;
  GCancellable *cancellable;
  SoupMessage *message;
  GError *error = NULL;
  gboolean has_timeout;
  GTlsCertificateFlags err_flags;

  g_assert (G_IS_TASK (task));
  g_assert (SOUP_IS_SESSION (session));

  stream = soup_session_send_finish (session, result, &error);
  message = g_object_get_data (G_OBJECT (task), "message");
  has_timeout = GPOINTER_TO_INT (g_task_get_task_data (task));

  /* Task return is handled somewhere else */
  if (has_timeout)
    return;

  if (error)
{
    g_task_return_error (task, error);
    return;
  }

#if SOUP_MAJOR_VERSION == 2
  soup_message_get_https_status (message, NULL, &err_flags);
#else
  err_flags = soup_message_get_tls_peer_certificate_errors (message);
#endif

  if (message && err_flags)
{
    guint timeout_id, timeout;

    timeout = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (task), "timeout"));
    timeout_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (task), "timeout-id"));
    g_clear_handle_id (&timeout_id, g_source_remove);
    g_object_unref (task);

    /* fixme: handle SSL errors */
    /* if (cm_utils_handle_ssl_error (message)) */
    /*   { */
    /*     g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, */
    /*                              "Cancelled"); */
    /*     return; */
    /*   } */

    timeout_id = g_timeout_add_seconds (timeout, cancel_read_uri, g_object_ref (task));
    g_object_set_data (G_OBJECT (task), "timeout-id", GUINT_TO_POINTER (timeout_id));
  }

  cancellable = g_task_get_cancellable (task);
  parser = json_parser_new ();
  json_parser_load_from_stream_async (parser, stream, cancellable,
                                      (GAsyncReadyCallback)load_from_stream_cb,
                                      g_steal_pointer (&task));
}

static void
message_network_event_cb (SoupMessage        *msg,
                          GSocketClientEvent  event,
                          GIOStream          *connection,
                          gpointer            user_data)
{
  GSocketAddress *address;

  /* We shall have a non %NULL @connection by %G_SOCKET_CLIENT_CONNECTING event */
  if (event != G_SOCKET_CLIENT_CONNECTING)
    return;

  /* @connection is a #GSocketConnection */
  address = g_socket_connection_get_remote_address (G_SOCKET_CONNECTION (connection), NULL);
  g_object_set_data_full (user_data, "address", address, g_object_unref);
}

#if SOUP_MAJOR_VERSION == 3
static gboolean
accept_certificate_callback (SoupMessage          *msg,
                             GTlsCertificate      *certificate,
                             GTlsCertificateFlags  tls_errors,
                             gpointer              user_data)
{
    /* Returning TRUE trusts it anyway. */
    return TRUE;
}
#endif

void
cm_utils_read_uri_async (const char          *uri,
                         guint                timeout,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  g_autoptr(SoupSession) session = NULL;
  g_autoptr(SoupMessage) message = NULL;
  g_autoptr(GCancellable) cancel = NULL;
  g_autoptr(GTask) task = NULL;
  guint timeout_id;

  g_return_if_fail (uri && *uri);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  if (cancellable)
    cancel = g_object_ref (cancellable);
  else
    cancel = g_cancellable_new ();

  task = g_task_new (NULL, cancel, callback, user_data);
  /* if this changes to TRUE, we consider it has been timeout */
  g_task_set_task_data (task, GINT_TO_POINTER (FALSE), NULL);
  g_task_set_source_tag (task, cm_utils_read_uri_async);

  timeout = CLAMP (timeout, 5, 60);
  timeout_id = g_timeout_add_seconds (timeout, cancel_read_uri, g_object_ref (task));
  g_object_set_data (G_OBJECT (task), "timeout", GUINT_TO_POINTER (timeout));
  g_object_set_data (G_OBJECT (task), "timeout-id", GUINT_TO_POINTER (timeout_id));

  message = soup_message_new (SOUP_METHOD_GET, uri);
  if (!message)
{
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                             "%s is not a valid uri", uri);
    return;
  }

  soup_message_set_flags (message, SOUP_MESSAGE_NO_REDIRECT);
  g_object_set_data_full (G_OBJECT (task), "message", g_object_ref (message), g_object_unref);

  g_signal_connect_object (message, "network-event",
                           G_CALLBACK (message_network_event_cb), task,
                           G_CONNECT_AFTER);
  session = soup_session_new ();
#if SOUP_MAJOR_VERSION == 2
  g_object_set (G_OBJECT (session), SOUP_SESSION_SSL_STRICT, FALSE, NULL);

  soup_session_send_async (session, message, cancel,
                           uri_file_read_cb,
                           g_steal_pointer (&task));
#else
  /* Accept invalid certificates */
  g_signal_connect (message, "accept-certificate", G_CALLBACK (accept_certificate_callback), NULL);

  soup_session_send_async (session, message, 0, cancel,
                           uri_file_read_cb,
                           g_steal_pointer (&task));
#endif
}

gpointer
cm_utils_read_uri_finish (GAsyncResult  *result,
                          GError       **error)
{
  GTask *task = (GTask *)result;

  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (g_task_get_source_tag (task) == cm_utils_read_uri_async, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
get_homeserver_cb (GObject      *obj,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonNode) root = NULL;
  JsonObject *object = NULL;
  const char *homeserver = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  root = cm_utils_read_uri_finish (result, &error);

  if (!root)
{
    g_task_return_error (task, error);
    return;
  }

  g_object_set_data_full (G_OBJECT (task), "address",
                          g_object_steal_data (G_OBJECT (result), "address"),
                          g_object_unref);

  if (JSON_NODE_HOLDS_OBJECT (root))
    object = json_node_get_object (root);

  if (object)
    object = cm_utils_json_object_get_object (object, "m.homeserver");

  if (object)
    homeserver = cm_utils_json_object_get_string (object, "base_url");

  if (!homeserver)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "Got invalid response from server");
  else
    g_task_return_pointer (task, g_strdup (homeserver), g_free);
}

/**
 * cm_utils_get_homeserver_async:
 * @username: A complete matrix username
 * @timeout: timeout in seconds
 * @cancellable: (nullable): A #GCancellable
 * @callback: The callback to run
 * @user_data: (nullable): The data passed to @callback
 *
 * Get homeserver from the given @username.  @userename
 * should be in complete form (eg: @user:example.org)
 *
 * @timeout is clamped between 5 and 60 seconds.
 *
 * This is a network operation and shall connect to the
 * network to fetch homeserver details.
 *
 * See https://matrix.org/docs/spec/client_server/r0.6.1#server-discovery
 */
void
cm_utils_get_homeserver_async (const char          *username,
                               guint                timeout,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree char *uri = NULL;
  const char *url;

  g_return_if_fail (username);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback);

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, cm_utils_get_homeserver_async);

  if (!cm_utils_user_name_valid (username))
{
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                             "Username '%s' is not a complete matrix id", username);
    return;
  }

  url = cm_utils_get_url_from_user_id (username);
  uri = g_strconcat ("https://", url, "/.well-known/matrix/client", NULL);

  cm_utils_read_uri_async (uri, timeout, cancellable,
                           get_homeserver_cb, g_steal_pointer (&task));
}

/**
 * cm_utils_get_homeserver_finish:
 * @result: A #GAsyncResult
 * @error: (optional): A #GError
 *
 * Finish call to cm_utils_get_homeserver_async().
 *
 * Returns: (nullable) : The homeserver string or %NULL
 * on error.  Free with g_free().
 */
char *
cm_utils_get_homeserver_finish (GAsyncResult  *result,
                                GError       **error)
{
  GTask *task = (GTask *)result;

  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (g_task_get_source_tag (task) == cm_utils_get_homeserver_async, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
api_get_version_cb (GObject      *obj,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(JsonNode) root = NULL;
  JsonObject *object = NULL;
  JsonArray *array = NULL;
  GError *error = NULL;
  const char *server;
  gboolean valid;

  g_assert (G_IS_TASK (task));

  server = g_task_get_task_data (task);
  root = cm_utils_read_uri_finish (result, &error);

  if (!error && root)
    error = cm_utils_json_node_get_error (root);

  if (!root ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
{
    if (error)
      g_task_return_error (task, error);
    else
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to get version for server '%s'", server);
    return;
  }

  g_object_set_data_full (G_OBJECT (task), "address",
                          g_object_steal_data (G_OBJECT (result), "address"),
                          g_object_unref);

  object = json_node_get_object (root);
  array = cm_utils_json_object_get_array (object, "versions");
  valid = FALSE;

  if (array)
{
    g_autoptr(GString) versions = NULL;
    guint length;

    versions = g_string_new ("");
    length = json_array_get_length (array);

    for (guint i = 0; i < length; i++)
{
      const char *version;

      version = json_array_get_string_element (array, i);
      g_string_append_printf (versions, " %s", version);

      /* We have tested only with r0.6.x and r0.5.0 */
      if (g_str_has_prefix (version, "r0.5.") ||
          g_str_has_prefix (version, "r0.6.") ||
          g_str_has_prefix (version, "v1."))
        valid = TRUE;
    }

    g_debug ("'%s' has versions:%s, valid: %d",
             server, versions->str, valid);
  }

  g_task_return_boolean (task, valid);
}

void
cm_utils_verify_homeserver_async (const char          *server,
                                  guint                timeout,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  g_autofree char *uri = NULL;

  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback);

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_task_data (task, g_strdup (server), g_free);
  g_task_set_source_tag (task, cm_utils_verify_homeserver_async);

  if (!server || !*server ||
      !g_str_has_prefix (server, "http"))
{
    g_task_return_new_error (task, G_IO_ERROR,
                             G_IO_ERROR_INVALID_DATA,
                             "URI '%s' is invalid", server);
    return;
  }

  uri = g_strconcat (server, "/_matrix/client/versions", NULL);
  cm_utils_read_uri_async (uri, timeout, cancellable,
                           api_get_version_cb,
                           g_steal_pointer (&task));
}

gboolean
cm_utils_verify_homeserver_finish (GAsyncResult  *result,
                                   GError       **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
utils_file_stream_cb (GObject      *obj,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &error);

  if (error) {
    g_task_return_error (task, error);
  } else {
    GFile *out_file;

    out_file = g_object_get_data (user_data, "file");
    if (out_file)
      g_object_ref (out_file);
    g_task_return_pointer (task, out_file, g_object_unref);
  }
}

static void
get_file_cb (GObject      *object,
             GAsyncResult *result,
             gpointer      user_data)
{
  CmClient *client;
  g_autoptr(GTask) task = user_data;
  GInputStream *istream = NULL;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  client = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (client));

  istream = cm_net_get_file_finish (CM_NET (object), result, &error);

  if (error)
    g_task_return_error (task, error);
  else
    {
      GOutputStream *out_stream = NULL;
      GFile *out_file = NULL;
      const char *file_path;

      file_path = g_object_get_data (user_data, "path");
      out_file = g_file_new_for_path (file_path);
      out_stream = (GOutputStream *)g_file_create (out_file, G_FILE_CREATE_NONE, NULL, &error);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          GFileIOStream *io_stream;

          g_clear_error (&error);
          io_stream = g_file_open_readwrite (out_file, NULL, &error);
          if (io_stream)
            {
              g_object_set_data_full (G_OBJECT (task), "io-stream", io_stream, g_object_unref);
              out_stream = g_io_stream_get_output_stream (G_IO_STREAM (io_stream));
            }
        }

      if (out_stream)
        {
          g_object_set_data_full (G_OBJECT (task), "file", out_file, g_object_unref);
          g_output_stream_splice_async (G_OUTPUT_STREAM (out_stream), istream,
                                        G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                        0, NULL,
                                        utils_file_stream_cb,
                                        g_steal_pointer (&task));
        }
      else
        {
          if (error)
            g_task_return_error (task, error);
          else
            g_task_return_boolean (task, FALSE);
        }
    }
}

static void
find_file_enc_cb (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  CmClient *client;
  g_autoptr(GTask) task = user_data;
  CmEncFileInfo *file_info;
  GCancellable *cancellable;
  char *uri;

  g_assert (G_IS_TASK (task));

  client = g_task_get_source_object (task);
  g_assert (CM_IS_CLIENT (client));

  file_info = cm_enc_find_file_enc_finish (CM_ENC (object), result, NULL);

  cancellable = g_task_get_cancellable (task);
  uri = g_object_get_data (user_data, "uri");

  cm_net_get_file_async (cm_client_get_net (client),
                         uri, file_info, cancellable,
                         get_file_cb,
                         g_steal_pointer (&task));
}

void
cm_utils_save_url_to_path_async (CmClient              *client,
                                 const char            *uri,
                                 char                  *file_path,
                                 GCancellable          *cancellable,
                                 GFileProgressCallback  progress_callback,
                                 gpointer               progress_user_data,
                                 GAsyncReadyCallback    callback,
                                 gpointer               user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (uri && *uri);
  g_return_if_fail (file_path && *file_path == '/');
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (client, cancellable, callback, user_data);
  g_object_set_data_full (G_OBJECT (task), "uri", g_strdup (uri), g_free);
  g_object_set_data_full (G_OBJECT (task), "path", file_path, g_free);
  g_object_set_data (G_OBJECT (task), "progress-cb", progress_callback);
  g_object_set_data (G_OBJECT (task), "progress-cb-data", progress_user_data);

  cm_enc_find_file_enc_async (cm_client_get_enc (client), uri,
                              find_file_enc_cb, task);
}

GFile *
cm_utils_save_url_to_path_finish (GAsyncResult  *result,
                                  GError       **error)
{
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/*
 * The @base_path should contain the base path up to 'cmatrix'
 * directory
 */
char *
cm_utils_get_path_for_m_type (const char  *base_path,
                              CmEventType  type,
                              gboolean     thumbnail,
                              const char  *file_name)
{
  const char *thumbnail_path = NULL;
  g_autofree char *path = NULL;

  g_return_val_if_fail (base_path && *base_path, NULL);

  if (thumbnail)
    thumbnail_path = "thumbnails";

  if (type == CM_M_ROOM_MESSAGE)
    path = g_build_filename (base_path, "files", thumbnail_path, NULL);

  if (type == CM_M_ROOM_AVATAR)
    path = g_build_filename (base_path, "avatars", "rooms", thumbnail_path, NULL);

  if (type == CM_M_ROOM_MEMBER)
    path = g_build_filename (base_path, "avatars", "users", thumbnail_path, NULL);

  if (path)
    {
      if (file_name && *file_name)
        return g_build_filename (path, file_name, NULL);
      else
        return g_steal_pointer (&path);
    }

  g_return_val_if_reached (NULL);
}
