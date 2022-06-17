/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cm-db-private.h"
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

  CmDb *cm_db;

  gboolean is_open;
  gboolean is_opening_db;
};

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
}

static void
db_open_cb (GObject      *obj,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  GError *error = NULL;
  CmMatrix *self;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CM_IS_MATRIX (self));

  self->is_open = cm_db_open_finish (self->cm_db, result, &error);
  self->is_opening_db = FALSE;

  if (!self->is_open)
    {
      g_clear_object (&self->cm_db);
      g_warning ("Error opening Matrix client database: %s",
                 error ? error->message : "");
      g_task_return_error (task, error);
      return;
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_READY]);
  g_task_return_boolean (task, self->is_open);
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
