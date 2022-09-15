/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* client.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later OR CC0-1.0
 */

#include <stdio.h>

#ifndef CMATRIX_USE_EXPERIMENTAL_API
# define CMATRIX_USE_EXPERIMENTAL_API
#endif /* CMATRIX_USE_EXPERIMENTAL_API */
#include "cmatrix.h"

CmMatrix *matrix;
CmClient *client;

static void
simple_account_sync_cb (gpointer   object,
                        CmClient  *cm_client,
                        CmRoom    *room,
                        GPtrArray *events,
                        GError    *err)
{
  puts ("\n\n\n");

  if (room && events)
    {
      for (guint i = 0; i < events->len; i++)
        {
          gpointer event;

          event = events->pdata[i];
          g_warning ("here: %d", cm_event_get_m_type (event));

          if (CM_IS_ROOM_MESSAGE_EVENT (event) &&
              cm_room_message_event_get_msg_type (event))
            {
              g_warning ("text message: %s", cm_room_message_event_get_body (event));
            }
        }
    }

  if (err)
    g_warning ("client error: %s", err->message);

  puts ("\n\n\n");
}

static void
simple_matrix_save_cb (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;

  cm_matrix_save_client_finish (matrix, result, &error);

  if (error)
    g_warning ("Error saving client: %s", error->message);

  /* Now, enable the client, and the client will start to sync, executing the callback on events */
  cm_client_set_enabled (client, TRUE);
}

static void
simple_get_homeserver_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  const char *server;
  char homeserver[255];

  server = cm_client_get_homeserver_finish (client, result, &error);

  /* It's okay to not able to get homeserver from username */
  if (error)
    g_message ("Failed to guess/verify homeserver: %s", error->message);
  else if (server)
    g_warning ("autofetched homeserver: %s", server);

  /* Not having a homeserver set means we failed to guess it from provided login id */
  /* So ask user for one. */
  while (!cm_client_get_homeserver (client))
    {
      printf ("input your Matrix homeserver address: ");
      scanf ("%s", homeserver);
      if (!cm_client_set_homeserver (client, homeserver))
        g_warning ("'%s' is not a valid homeserver uri (did you forget to "
                   "prefix with 'https://')", homeserver);
    }

  /* The sync callback runs for every /sync response and other interesting events (like wrong password error) */
  cm_client_set_sync_callback (client,
                               simple_account_sync_cb,
                               NULL, NULL);
  cm_matrix_save_client_async (matrix, client,
                               simple_matrix_save_cb, NULL);

}

static void
simple_joined_rooms_changed_cb (GListModel *list,
                                guint       position,
                                guint       removed,
                                guint       added,
                                gpointer    user_data)
{
  puts ("\n\n\n");

  g_warning ("joined rooms changed");
  g_warning ("total number of items: %u", g_list_model_get_n_items (list));

  for (guint i = 0; i < g_list_model_get_n_items (list); i++)
    {
      g_autoptr(CmRoom) room = NULL;

      room = g_list_model_get_item (list, i);

      g_warning ("room name: %s, room id: %s",
                 cm_room_get_name (room),
                 cm_room_get_id (room));
    }

  puts ("\n\n\n");
}

static void
simple_matrix_open_cb (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  char username[255], password[255];
  GListModel *joined_rooms;
  CmAccount *account;

  if (!cm_matrix_open_finish (matrix, result, &error))
    g_error ("Error opening db: %s", error->message);

  printf ("input your Matrix username: ");
  scanf ("%s", username);
  printf ("input your Matrix password: ");
  scanf ("%s", password);
  puts ("");

  g_message ("username: %s, password: %s", username, password);

  client = cm_matrix_client_new (matrix);
  account = cm_client_get_account (client);
  joined_rooms = cm_client_get_joined_rooms (client);
  g_signal_connect_object (joined_rooms, "items-changed",
                           G_CALLBACK (simple_joined_rooms_changed_cb), client,
                           0);

  if (!cm_account_set_login_id (account, username))
    g_error ("'%s' isn't a valid username", username);
  cm_client_set_password (client, password);
  cm_client_set_device_name (client, "Example CMatrix");

  /* try if we can get a valid homeserver from username */
  cm_client_get_homeserver_async (client, NULL,
                                  simple_get_homeserver_cb, NULL);
}

int
main (void)
{
  GMainLoop *main_loop;
  g_autofree char *db_dir = NULL;
  g_autofree char *data_dir = NULL;
  g_autofree char *cache_dir = NULL;

  /* Initialize the library */
  cm_init (TRUE);

  /* Create a matrix object */
  data_dir = g_build_filename (g_get_user_data_dir (), "CMatrix", "simple-client", NULL);
  cache_dir = g_build_filename (g_get_user_cache_dir (), "CMatrix", "simple-client", NULL);
  matrix = cm_matrix_new (data_dir, cache_dir, "com.example.CMatrix", FALSE);

  /* Ask matrix to open/create the db which will be used to store keys, session data, etc. */
  db_dir = g_build_filename (g_get_user_data_dir (), "CMatrix", "simple-client", NULL);
  cm_matrix_open_async (matrix, db_dir, "matrix.db", NULL,
                        simple_matrix_open_cb, NULL);

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);
}
