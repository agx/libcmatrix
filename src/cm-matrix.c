/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define GCRYPT_NO_DEPRECATED
#include <gcrypt.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cm-db-private.h"
#include "cm-utils-private.h"
#include "cm-client.h"
#include "cm-client-private.h"
#include "cm-matrix.h"
#include "cm-matrix-private.h"

/**
 * SECTION: cm-matrix
 * @title: CmMatrix
 * @short_description:
 * @include: "cm-matrix.h"
 */

struct _CmMatrix
{
  GObject parent_instance;

  char *db_path;
  char *db_name;

  char *data_dir;
  char *cache_dir;

  CmDb *cm_db;

  gboolean is_open;
  gboolean is_opening_db;
};

char *cmatrix_data_dir, *cmatrix_app_id;

G_DEFINE_TYPE (CmMatrix, cm_matrix, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_READY,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
cm_matrix_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  CmMatrix *self = (CmMatrix *)object;

  switch (prop_id)
    {
    case PROP_READY:
      g_value_set_boolean (value, cm_matrix_is_ready (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
cm_matrix_finalize (GObject *object)
{
  CmMatrix *self = (CmMatrix *)object;

  g_free (self->db_path);
  g_free (self->db_name);

  g_free (self->data_dir);
  g_free (self->cache_dir);

  G_OBJECT_CLASS (cm_matrix_parent_class)->finalize (object);
}

static void
cm_matrix_class_init (CmMatrixClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cm_matrix_finalize;
  object_class->get_property = cm_matrix_get_property;

  /**
   * CmMatrix:ready:
   *
   * Whether matrix is enabled and usable
   */
  properties[PROP_READY] =
    g_param_spec_boolean ("ready",
                          "matrix is ready",
                          "matrix is ready",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
cm_matrix_init (CmMatrix *self)
{
  if (!gcry_control (GCRYCTL_INITIALIZATION_FINISHED_P))
    g_error ("libgcrypt has not been initialized, did you run cm_init()?");
}

/**
 * cm_matrix_new:
 * @data_dir: The data directory
 * @cache_dir: The cache directory
 * @app_id: The app id string (unused)
 *
 * Create a new #CmMatrix with the provided details
 *
 * @data_dir is used to store downloaded files,
 * avatars, and thumbnails.  The content shall not
 * be encrypted even if that was the case when
 * received over the wire.
 *
 * @app_id should be a valid string when validated
 * with g_application_id_is_valid()
 *
 * The same values should be provided every time
 * #CmMatrix is created as these info are used
 * to store data.
 *
 * Returns: (transfer full): A #CmMatrix
 */
/*
 * @cache_dir may be used to store files temporarily
 * when needed (eg: when resizing images)
 */
CmMatrix *
cm_matrix_new (const char *data_dir,
               const char *cache_dir,
               const char *app_id)
{
  CmMatrix *self;
  char *dir;

  g_return_val_if_fail (data_dir && *data_dir, NULL);
  g_return_val_if_fail (cache_dir && *cache_dir, NULL);
  g_return_val_if_fail (g_application_id_is_valid (app_id), NULL);

  self = g_object_new (CM_TYPE_MATRIX, NULL);
  self->data_dir = g_build_filename (data_dir, "cmatrix", NULL);
  cmatrix_data_dir = g_strdup (self->data_dir);
  cmatrix_app_id = g_strdup (app_id);
  self->cache_dir = g_build_filename (cache_dir, "cmatrix", NULL);

  dir = cm_utils_get_path_for_m_type (self->data_dir, CM_M_ROOM_MESSAGE, TRUE, NULL);
  g_mkdir_with_parents (dir, S_IRWXU);
  g_free (dir);

  dir = cm_utils_get_path_for_m_type (self->data_dir, CM_M_ROOM_MEMBER, TRUE, NULL);
  g_mkdir_with_parents (dir, S_IRWXU);
  g_free (dir);

  dir = cm_utils_get_path_for_m_type (self->data_dir, CM_M_ROOM_AVATAR, TRUE, NULL);
  g_mkdir_with_parents (dir, S_IRWXU);
  g_free (dir);

  return self;
}

/**
 * cm_init:
 * @init_gcrypt: Whether to initialize gcrypt
 *
 * This function should be called to initialize the library.
 * You may call this in main()
 *
 * If you don't initialize gcrypt, you should do it yourself
 */
void
cm_init (gboolean init_gcrypt)
{
  if (init_gcrypt)
    {
      /* Version check should be the very first call because it
         makes sure that important subsystems are initialized. */
      if (!gcry_check_version (GCRYPT_VERSION))
        {
          g_critical ("libgcrypt version mismatch");
          exit (2);
        }
      gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
      gcry_control (GCRYCTL_INIT_SECMEM, 512 * 1024, 0);
      gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
      gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
    }
}

/**
 * cm_matrix_open_async:
 * @db_path: The path where db is (to be) stored
 * @db_name: The name of database
 * @cancellable: (nullable): A #GCancellable
 * @callback: The callback to run when ready
 * @user_data: user data for @callback
 *
 * Open the matrix E2EE db which shall be used by clients
 * when required.
 *
 * Run cm_matrix_open_finish() to get the result.
 */
void
cm_matrix_open_async (CmMatrix            *self,
                      const char          *db_path,
                      const char          *db_name,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (CM_IS_MATRIX (self));
  g_return_if_fail (db_path && *db_path);
  g_return_if_fail (db_name && *db_name);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (!self->is_open);
  g_return_if_fail (!self->cm_db);

  task = g_task_new (self, cancellable, callback, user_data);

  if (self->is_opening_db)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Opening db in progress");
      return;
    }

  if (self->is_open)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  self->is_opening_db = TRUE;
  self->cm_db = cm_db_new ();
  cm_db_open_async (self->cm_db, g_strdup (db_path), db_name,
                    db_open_cb,
                    g_steal_pointer (&task));
}

gboolean
cm_matrix_open_finish (CmMatrix      *self,
                       GAsyncResult  *result,
                       GError       **error)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
cm_matrix_is_ready (CmMatrix *self)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), FALSE);

  return self->is_open;
}

/**
 * cm_matrix_client_new:
 * @self: A #CmMatrix
 *
 * Create a new #CmClient.  It's an error
 * to create a new client before opening
 the db with cm_matrix_open_async()
 *
 * Returns: (transfer full): A #CmClient
 */
CmClient *
cm_matrix_client_new (CmMatrix *self)
{
  CmClient *client;

  g_return_val_if_fail (CM_IS_MATRIX (self), NULL);

  if (!cm_matrix_is_ready (self))
    g_error ("Database not open, See cm_matrix_open_async()");

  client = g_object_new (CM_TYPE_CLIENT, NULL);
  cm_client_set_db (client, self->cm_db);

  return client;
}

CmDb *
cm_matrix_get_db (CmMatrix *self)
{
  g_return_val_if_fail (CM_IS_MATRIX (self), NULL);

  return self->cm_db;
}
