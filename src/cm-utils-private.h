/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cm-config.h"

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "cm-enums.h"
#include "cm-types.h"

/* Hack to check format specifier arguments match */
static inline void check_format (const char *fmt, ...) G_GNUC_PRINTF (1, 2);
static inline void check_format (const char *fmt, ...) {}

#define CM_TRACE(fmt, ...) do {                                         \
  check_format (fmt, ##__VA_ARGS__);                                    \
  g_log_structured (G_LOG_DOMAIN,                                       \
                    (1 << G_LOG_LEVEL_USER_SHIFT),                      \
                    "CODE_FILE", __FILE__,                              \
                    "CODE_LINE", G_STRINGIFY (__LINE__),                \
                    "CODE_FUNC", G_STRFUNC,                             \
                    "MESSAGE", fmt, ##__VA_ARGS__);                     \
} while (0)
#define CM_LOG_SUCCESS(_value) cm_utils_log_bool_str (_value, TRUE)
#define CM_LOG_BOOL(_value) cm_utils_log_bool_str (_value, FALSE)

const char   *cm_utils_log_bool_str             (gboolean             value,
                                                 gboolean             use_success);
const char   *cm_utils_anonymize                (GString             *str,
                                                 const char          *value);
GError       *cm_utils_json_node_get_error      (JsonNode            *node);
gboolean      cm_utils_get_item_position        (GListModel          *list,
                                                 gpointer             item,
                                                 guint               *position);
gboolean      cm_utils_remove_list_item         (GListStore          *store,
                                                 gpointer             item);
const char   *cm_utils_get_event_type_str       (CmEventType          type);
char         *cm_utils_json_object_to_string    (JsonObject          *object,
                                                 gboolean             prettify);
GString      *cm_utils_json_get_canonical       (JsonObject          *object,
                                                 GString             *out);
JsonObject   *cm_utils_string_to_json_object    (const char          *json_str);
gboolean      cm_utils_json_object_has_member   (JsonObject          *object,
                                                 const char          *member);
gint64        cm_utils_json_object_get_int      (JsonObject          *object,
                                                 const char          *member);
gboolean      cm_utils_json_object_get_bool     (JsonObject          *object,
                                                 const char          *member);
const char   *cm_utils_json_object_get_string   (JsonObject          *object,
                                                 const char          *member);
char         *cm_utils_json_object_dup_string   (JsonObject          *object,
                                                 const char          *member);
JsonObject   *cm_utils_json_object_get_object   (JsonObject          *object,
                                                 const char          *member);
JsonArray    *cm_utils_json_object_get_array    (JsonObject          *object,
                                                 const char          *member);
void          cm_utils_clear                    (char                *buffer,
                                                 size_t               length);
void          cm_utils_free_buffer              (char                *buffer);
const char   *cm_utils_get_url_from_user_id     (const char          *user_id);
gboolean      cm_utils_user_name_valid          (const char          *matrix_user_id);
gboolean      cm_utils_user_name_is_email       (const char          *user_id);
gboolean      cm_utils_mobile_is_valid          (const char          *mobile_num);

gboolean      cm_utils_home_server_valid        (const char          *homeserver);
void          cm_utils_read_uri_async           (const char          *uri,
                                                 guint                timeout,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
gpointer      cm_utils_read_uri_finish          (GAsyncResult        *result,
                                                 GError             **error);
void          cm_utils_verify_homeserver_async  (const char          *server,
                                                 guint                timeout,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
gboolean      cm_utils_verify_homeserver_finish (GAsyncResult        *result,
                                                 GError             **error);
void          cm_utils_save_url_to_path_async     (CmClient              *client,
                                                   const char            *uri,
                                                   char                  *file_path,
                                                   GCancellable          *cancellable,
                                                   GFileProgressCallback  progress_callback,
                                                   gpointer               progress_user_data,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
GFile        *cm_utils_save_url_to_path_finish    (GAsyncResult          *result,
                                                   GError               **error);
char         *cm_utils_get_path_for_m_type      (const char          *base_path,
                                                 CmEventType          type,
                                                 gboolean             thumbnail,
                                                 const char          *file_name);
