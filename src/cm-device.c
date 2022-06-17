/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "cm-client.h"
#include "cm-client-private.h"
#include "cm-utils-private.h"
#include "cm-enc-private.h"
#include "cm-device-private.h"
#include "cm-device.h"

struct _CmDevice
{
  GObject   parent_instance;

  CmClient *client;
  char     *user_id;
  char     *device_id;
  char     *device_name;
  char     *ed_key;
  char     *curve_key;
  char     *one_time_key;

  gboolean meagolm_v1;
  gboolean olm_v1;
  gboolean signature_failed;
};

G_DEFINE_TYPE (CmDevice, cm_device, G_TYPE_OBJECT)

static void
cm_device_finalize (GObject *object)
{
  CmDevice *self = (CmDevice *)object;

  g_clear_object (&self->client);
  g_free (self->device_id);
  g_free (self->user_id);
  g_free (self->device_name);
  g_free (self->ed_key);
  g_free (self->curve_key);

  G_OBJECT_CLASS (cm_device_parent_class)->finalize (object);
}

static void
cm_device_class_init (CmDeviceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_device_finalize;
}

static void
cm_device_init (CmDevice *self)
{
}

CmDevice *
cm_device_new (gpointer    client,
               JsonObject *root)
{
  JsonObject *object;
  JsonArray *array;
  CmDevice *self;
  const char *text;
  char *key_name;

  g_return_val_if_fail (CM_IS_CLIENT (client), NULL);
  g_return_val_if_fail (root, NULL);

  self = g_object_new (CM_TYPE_DEVICE, NULL);
  self->client = g_object_ref (client);

  text = cm_utils_json_object_get_string (root, "device_id");
  self->device_id = g_strdup (text);
  g_return_val_if_fail (text && *text, NULL);

  text = cm_utils_json_object_get_string (root, "user_id");
  self->user_id = g_strdup (text);

  object = cm_utils_json_object_get_object (root, "unsigned");
  text = cm_utils_json_object_get_string (object, "device_display_name");
  self->device_name = g_strdup (text);

  key_name = g_strconcat ("ed25519:", self->device_id, NULL);
  object = cm_utils_json_object_get_object (root, "keys");
  text = cm_utils_json_object_get_string (object, key_name);
  self->ed_key = g_strdup (text);
  g_free (key_name);

  if (!cm_enc_verify (cm_client_get_enc (self->client), root,
                      self->user_id, self->device_id, self->ed_key))
    {
      /* DEBUG */
      g_warning ("Signature failed");
      self->signature_failed = TRUE;
      return self;
    }

  key_name = g_strconcat ("curve25519:", self->device_id, NULL);
  object = cm_utils_json_object_get_object (root, "keys");
  text = cm_utils_json_object_get_string (object, key_name);
  self->curve_key = g_strdup (text);
  g_free (key_name);

  array = cm_utils_json_object_get_array (root, "algorithms");
  for (guint i = 0; array && i < json_array_get_length (array); i++) {
    const char *algorithm;

    algorithm = json_array_get_string_element (array, i);
    if (g_strcmp0 (algorithm, ALGORITHM_MEGOLM) == 0)
      self->meagolm_v1 = TRUE;
    else if (g_strcmp0 (algorithm, ALGORITHM_OLM) == 0)
      self->olm_v1 = TRUE;
  }

  return self;
}

void
cm_device_set_one_time_key (CmDevice   *self,
                            const char *key)
{
  g_return_if_fail (CM_IS_DEVICE (self));

  g_free (self->one_time_key);
  self->one_time_key = g_strdup (key);
}

char *
cm_device_steal_one_time_key (CmDevice *self)
{
  g_return_val_if_fail (CM_IS_DEVICE (self), NULL);

  return g_steal_pointer (&self->one_time_key);
}

gboolean
cm_device_has_one_time_key (CmDevice *self)
{
  g_return_val_if_fail (CM_IS_DEVICE (self), FALSE);

  return !!self->one_time_key;
}

const char *
cm_device_get_id (CmDevice *self)
{
  g_return_val_if_fail (CM_IS_DEVICE (self), NULL);

  return self->device_id;
}

const char *
cm_device_get_ed_key (CmDevice *self)
{
  g_return_val_if_fail (CM_IS_DEVICE (self), NULL);

  return self->ed_key;
}

const char *
cm_device_get_curve_key (CmDevice *self)
{
  g_return_val_if_fail (CM_IS_DEVICE (self), NULL);

  return self->curve_key;
}
