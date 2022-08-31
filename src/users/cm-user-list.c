/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-user-list.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-user-private.h"
#include "cm-room-member-private.h"
#include "cm-client-private.h"
#include "cm-user-list-private.h"

/**
 * SECTION: cm-user-list
 * @title: CmUserList
 * @short_description: Track all users that belongs to the account
 * @include: "cm-user-list.h"
 *
 * #CmUserList tracks all users associated with the account, instead
 * of tracking them per room individually. Please note that only
 * relevant users (eg: Users that shares an encrypted room, or users
 * that have sent an event recently) are stored to avoid eating too
 * much memory
 */

struct _CmUserList
{
  GObject       parent_instance;

  CmClient     *client;
  GHashTable   *users_table;
};

G_DEFINE_TYPE (CmUserList, cm_user_list, G_TYPE_OBJECT)

static void
cm_user_list_finalize (GObject *object)
{
  CmUserList *self = (CmUserList *)object;

  g_clear_object (&self->client);
  g_hash_table_unref (self->users_table);

  G_OBJECT_CLASS (cm_user_list_parent_class)->finalize (object);
}

static void
cm_user_list_class_init (CmUserListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_user_list_finalize;
}

static void
cm_user_list_init (CmUserList *self)
{
  self->users_table = g_hash_table_new_full (g_direct_hash,
                                             g_direct_equal,
                                             (GDestroyNotify)g_ref_string_release,
                                             g_object_unref);
}

CmUserList *
cm_user_list_new (CmClient *client)
{
  CmUserList *self;

  g_return_val_if_fail (CM_IS_CLIENT (client), NULL);

  self = g_object_new (CM_TYPE_USER_LIST, NULL);
  self->client = g_object_ref (client);

  return self;
}

CmUser *
cm_user_list_find_user (CmUserList *self,
                        GRefString *user_id,
                        gboolean    create_if_missing)
{
  CmUser *user;

  g_return_val_if_fail (CM_IS_USER_LIST (self), NULL);
  g_return_val_if_fail (user_id && *user_id == '@', NULL);

  user = g_hash_table_lookup (self->users_table, user_id);

  if (user || !create_if_missing)
    return user;

  user = (CmUser *)cm_room_member_new (user_id);
  cm_user_set_client (user, self->client);
  g_hash_table_insert (self->users_table,
                       g_ref_string_acquire (user_id), user);

  return user;
}
