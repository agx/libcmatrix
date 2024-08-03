/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

void          cm_utils_get_homeserver_async     (const char          *username,
                                                 uint                 timeout,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
char         *cm_utils_get_homeserver_finish    (GAsyncResult        *result,
                                                 GError             **error);
char         *cm_utils_get_homeserver_sync      (const char          *username,
                                                 GError             **error);


G_END_DECLS
