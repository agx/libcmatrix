/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-room-member.c
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

#include "cm-client.h"
#include "cm-client-private.h"
#include "cm-utils-private.h"
#include "cm-device.h"
#include "cm-device-private.h"
#include "cm-room.h"
#include "cm-enc-private.h"
#include "cm-user-private.h"
#include "cm-room-member-private.h"
#include "cm-room-member.h"

struct _CmRoomMember
{
  CmUser      parent_instance;

  CmRoom     *room;
  CmClient   *client;
  GListStore *devices;
  GHashTable *devices_table;

  char       *user_id;
};

G_DEFINE_TYPE (CmRoomMember, cm_room_member, CM_TYPE_USER)

static void
cm_room_member_finalize (GObject *object)
{
  CmRoomMember *self = (CmRoomMember *)object;

  g_clear_object (&self->room);
  g_clear_object (&self->client);

  g_list_store_remove_all (self->devices);
  g_clear_object (&self->devices);
  g_clear_pointer (&self->devices_table, g_hash_table_unref);

  g_free (self->user_id);

  G_OBJECT_CLASS (cm_room_member_parent_class)->finalize (object);
}

static void
cm_room_member_class_init (CmRoomMemberClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_room_member_finalize;
}

static void
cm_room_member_init (CmRoomMember *self)
{
  self->devices = g_list_store_new (CM_TYPE_DEVICE);
  self->devices_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_object_unref);
}

CmRoomMember *
cm_room_member_new (gpointer    room,
                    gpointer    client,
                    const char *user_id)
{
  CmRoomMember *self;

  g_return_val_if_fail (CM_IS_ROOM (room), NULL);
  g_return_val_if_fail (CM_IS_CLIENT (client), NULL);
  g_return_val_if_fail (user_id && *user_id == '@', NULL);

  self = g_object_new (CM_TYPE_ROOM_MEMBER, NULL);
  self->user_id = g_strdup (user_id);
  self->room = g_object_ref (room);
  self->client = g_object_ref (client);

  return self;
}

/*
 * cm_room_member_set_device_changed:
 *
 * mark as the member's device list has changed.
 * It's useful to check if device has changed
 * to load keys of those changed device before
 * sending an encrypted message
 */
void
cm_room_member_set_device_changed (CmRoomMember *self,
                                   gboolean      changed)
{
  g_return_if_fail (CM_IS_ROOM_MEMBER (self));

  /* todo */
}

gboolean
cm_room_member_get_device_changed (CmRoomMember *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MEMBER (self), TRUE);

  return FALSE;
}

void
cm_room_member_set_json_data (CmRoomMember *self,
                              JsonObject   *object)
{
  const char *name, *avatar_url;

  g_return_if_fail (CM_IS_ROOM_MEMBER (self));
  g_return_if_fail (object);

  name = cm_utils_json_object_get_string (object, "display_name");
  if (!name)
      name = cm_utils_json_object_get_string (object, "displayname");

  avatar_url = cm_utils_json_object_get_string (object, "avatar_url");
  cm_user_set_details (CM_USER (self), name, avatar_url);
}

/*
 * cm_room_member_set_devices:
 *
 * Set devices for @self removing all
 * non existing devices in @root
 */
void
cm_room_member_set_devices (CmRoomMember *self,
                            JsonObject   *root)
{
  g_autoptr(GList) members = NULL;
  g_autoptr(GHashTable) devices_table = NULL;
  GHashTable *old_devices;
  JsonObject *child;

  g_return_if_fail (CM_IS_ROOM_MEMBER (self));
  g_return_if_fail (root);

  /* Create a table of devices and add the items here */
  devices_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, g_object_unref);
  members = json_object_get_members (root);

  for (GList *member = members; member; member = member->next)
    {
      g_autoptr(CmDevice) device = NULL;
      g_autofree char *device_name = NULL;
      const char *device_id, *user;

      child = cm_utils_json_object_get_object (root, member->data);
      device_id = cm_utils_json_object_get_string (child, "device_id");
      user = cm_utils_json_object_get_string (child, "user_id");

      if (!device_id || !*device_id)
        continue;

      if (self->devices_table &&
          (device = g_hash_table_lookup (self->devices_table, device_id)))
        {
          /* If the device is already in the old table, remove it there
           * so that it's present only in the new devices_table.
           */
          /* device variable is autofree, so the ref is unref later automatically */
          g_object_ref (device);
          g_hash_table_remove (self->devices_table, device_id);
          g_hash_table_insert (devices_table, g_strdup (device_id), g_object_ref (device));
          continue;
        }

      if (g_strcmp0 (user, self->user_id) != 0)
        {
          g_warning ("‘%s’ and ‘%s’ are not the same users", user, self->user_id);
          continue;
        }

      /* /\* TODO: Is this required? *\/ */
      /* if (self->is_self && */
      /*     g_strcmp0 (device_id, cm_client_get_device_id (self->cm_client)) == 0) */
      /*   continue; */

      if (g_strcmp0 (member->data, device_id) != 0)
        {
          g_warning ("‘%s’ and ‘%s’ are not the same device", (char *)member->data, device_id);
          continue;
        }

      device = cm_device_new (self->client, child);
      g_hash_table_insert (devices_table, g_strdup (device_id), g_object_ref (device));
      g_list_store_append (self->devices, device);
    }

  old_devices = self->devices_table;
  self->devices_table = g_steal_pointer (&devices_table);
  /* Assign so as to autofree */
  devices_table = old_devices;

  if (old_devices)
    {
      g_autoptr(GList) devices = NULL;

      /* The old table now contains the devices that are not used by the user anymore */
      devices = g_hash_table_get_values (old_devices);

      for (GList *device = devices; device && device->data; device = device->next)
        cm_utils_remove_list_item (self->devices, device->data);
    }
}

GListModel *
cm_room_member_get_devices (CmRoomMember *self)
{
  g_return_val_if_fail (CM_IS_ROOM_MEMBER (self), NULL);

  return G_LIST_MODEL (self->devices);
}

JsonObject *
cm_room_member_get_device_key_json (CmRoomMember *self)
{
  JsonObject *object;
  guint n_items;

  g_return_val_if_fail (CM_IS_ROOM_MEMBER (self), NULL);

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->devices));
  if (!n_items)
    return NULL;

  object = json_object_new ();

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmDevice) device = NULL;
      const char *device_id;

      device = g_list_model_get_item (G_LIST_MODEL (self->devices), i);
      device_id = cm_device_get_id (device);

      if (!cm_device_has_one_time_key (device))
        json_object_set_string_member (object, device_id, "signed_curve25519");
    }

  return object;
}

void
cm_room_member_add_one_time_keys (CmRoomMember *self,
                                  JsonObject   *root)
{
  JsonObject *object, *child;
  guint n_items;

  g_return_if_fail (CM_IS_ROOM_MEMBER (self));
  g_return_if_fail (root);

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->devices));

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(CmDevice) device = NULL;
      g_autoptr(GList) members = NULL;
      const char *device_id;

      device = g_list_model_get_item (G_LIST_MODEL (self->devices), i);
      device_id = cm_device_get_id (device);
      child = cm_utils_json_object_get_object (root, device_id);

      if (!child)
        {
          g_warning ("device '%s' not found", device_id);
          continue;
        }

      members = json_object_get_members (child);

      for (GList *node = members; node; node = node->next)
        {
          object = cm_utils_json_object_get_object (child, node->data);

          if (cm_enc_verify (cm_client_get_enc (self->client),
                             object, self->user_id, device_id,
                             cm_device_get_ed_key (device)))
            {
              const char *key;

              key = cm_utils_json_object_get_string (object, "key");
              cm_device_set_one_time_key (device, key);
            }
        }
    }
}
