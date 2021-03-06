/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "gproxymountoperation.h"

/* for protecting the id_to_op and id_count */
G_LOCK_DEFINE_STATIC(proxy_op);

/* map from id to GMountOperation */
static GHashTable *id_to_op = NULL;

static guint id_count = 1;

typedef struct
{
  gchar *id;
  GMountOperation *op;
  GProxyVolumeMonitor *monitor;
  gulong reply_handler_id;
} ProxyMountOpData;

static void
proxy_mount_op_data_free (ProxyMountOpData *data)
{
  g_free (data->id);
  if (data->reply_handler_id > 0)
    g_signal_handler_disconnect (data->op, data->reply_handler_id);
  g_object_unref (data->op);
  g_object_unref (data->monitor);
  g_free (data);
}

/* must be called with lock held */
static ProxyMountOpData *
proxy_mount_op_data_new (GMountOperation *op,
                         GProxyVolumeMonitor *monitor)
{
  ProxyMountOpData *data;

  data = g_new0 (ProxyMountOpData, 1);
  data->id = g_strdup_printf ("%d:%d", getpid (), id_count++);
  data->op = g_object_ref (op);
  data->monitor = g_object_ref (monitor);
  return data;
}

/* must be called with lock held */
static void
ensure_hash (void)
{
  if (id_to_op == NULL)
    id_to_op = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      NULL,
                                      (GDestroyNotify) proxy_mount_op_data_free);
}

const gchar *
g_proxy_mount_operation_wrap (GMountOperation *op,
                              GProxyVolumeMonitor *monitor)
{
  ProxyMountOpData *data;

  if (op == NULL)
    return "";

  G_LOCK (proxy_op);

  ensure_hash ();

  data = proxy_mount_op_data_new (op, monitor);
  g_hash_table_insert (id_to_op,
                       data->id,
                       data);

  G_UNLOCK (proxy_op);

  return data->id;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_op_reply_cb (GVfsRemoteVolumeMonitor *proxy,
                   GAsyncResult *res,
                   gpointer user_data)
{
  GError *error = NULL;
  
  if (!gvfs_remote_volume_monitor_call_mount_op_reply_finish (proxy,
                                                              res,
                                                              &error))
    {
      g_warning ("Error from MountOpReply(): %s", error->message);
      g_error_free (error);
    }
}

static void
mount_operation_reply (GMountOperation        *mount_operation,
                       GMountOperationResult  result,
                       gpointer               user_data)
{
  ProxyMountOpData *data = user_data;
  GVfsRemoteVolumeMonitor *proxy;
  const gchar *user_name;
  const gchar *domain;
  const gchar *password;
  gchar *encoded_password;
  gint password_save;
  gint choice;
  gboolean anonymous;
  
  user_name     = g_mount_operation_get_username (mount_operation);
  domain        = g_mount_operation_get_domain (mount_operation);
  password      = g_mount_operation_get_password (mount_operation);
  password_save = g_mount_operation_get_password_save (mount_operation);
  choice        = g_mount_operation_get_choice (mount_operation);
  anonymous     = g_mount_operation_get_anonymous (mount_operation);

  if (user_name == NULL)
    user_name = "";
  if (domain == NULL)
    domain = "";
  if (password == NULL)
    password = "";

  /* NOTE: this is not to add "security", it's merely to prevent accidental exposure
   *       of passwords when running dbus-monitor
   */
  encoded_password = g_base64_encode ((const guchar *) password, (gsize) (strlen (password) + 1));

  proxy = g_proxy_volume_monitor_get_dbus_proxy (data->monitor);
  gvfs_remote_volume_monitor_call_mount_op_reply (proxy,
                                                  data->id,
                                                  result,
                                                  user_name,
                                                  domain,
                                                  encoded_password,
                                                  password_save,
                                                  choice,
                                                  anonymous,
                                                  NULL,
                                                  (GAsyncReadyCallback) mount_op_reply_cb,
                                                  data);
  g_object_unref (proxy);
  g_free (encoded_password);
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_ask_password (const gchar  *wrapped_id,
                                             const gchar  *message,
                                             const gchar  *default_user,
                                             const gchar  *default_domain,
                                             guint         flags)
{
  ProxyMountOpData *data;

  g_return_if_fail (wrapped_id != NULL);

  if (id_to_op == NULL)
    goto out;
  
  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    goto out;
  
  if (data->reply_handler_id == 0)
    {
      data->reply_handler_id = g_signal_connect (data->op,
                                                 "reply",
                                                 G_CALLBACK (mount_operation_reply),
                                                 data);
    }

  g_signal_emit_by_name (data->op,
                         "ask-password",
                         message,
                         default_user,
                         default_domain,
                         flags);

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_ask_question (const gchar        *wrapped_id,
                                             const gchar        *message,
                                             const gchar *const *choices)
{
  ProxyMountOpData *data;

  g_return_if_fail (wrapped_id != NULL);

  if (id_to_op == NULL)
    goto out;
  
  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    goto out;

  if (data->reply_handler_id == 0)
    {
      data->reply_handler_id = g_signal_connect (data->op,
                                                 "reply",
                                                 G_CALLBACK (mount_operation_reply),
                                                 data);
    }

  g_signal_emit_by_name (data->op,
                         "ask-question",
                         message,
                         choices);

 out:
   ;
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_show_processes (const gchar        *wrapped_id,
                                               const gchar        *message,
                                               GVariant           *pids,
                                               const gchar *const *choices)
{
  ProxyMountOpData *data;
  GArray *processes;
  GVariantIter iter;
  GPid pid;

  g_return_if_fail (wrapped_id != NULL);

  processes = NULL;

  if (id_to_op == NULL)
    goto out;
  
  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    goto out;
  
  processes = g_array_new (FALSE, FALSE, sizeof (GPid));
  g_variant_iter_init (&iter, pids);
  while (g_variant_iter_loop (&iter, "i", &pid))
    g_array_append_val (processes, pid);
 
  if (data->reply_handler_id == 0)
    {
      data->reply_handler_id = g_signal_connect (data->op,
                                                 "reply",
                                                 G_CALLBACK (mount_operation_reply),
                                                 data);
    }

  g_signal_emit_by_name (data->op,
                         "show-processes",
                         message,
                         processes,
                         choices);

 out:
  if (processes)
    g_array_unref (processes);
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_show_unmount_progress (const gchar *wrapped_id,
                                                      const gchar *message,
                                                      guint64      time_left,
                                                      guint64      bytes_left)
{
  ProxyMountOpData *data;

  g_return_if_fail (wrapped_id != NULL);

  if (id_to_op == NULL)
    return;
  
  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    return;

  g_signal_emit_by_name (data->op,
                         "show-unmount-progress",
                         message,
                         time_left,
                         bytes_left);
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_aborted (const gchar *wrapped_id)
{
  ProxyMountOpData *data;

  g_return_if_fail (wrapped_id != NULL);

  if (id_to_op == NULL)
    goto out;

  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    goto out;

  g_signal_emit_by_name (data->op, "aborted");

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_destroy (const gchar *wrapped_id)
{
  g_return_if_fail (wrapped_id != NULL);

  if (strlen (wrapped_id) == 0)
    return;

  if (id_to_op == NULL)
    return;

  G_LOCK (proxy_op);
  g_hash_table_remove (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);
}

/* ---------------------------------------------------------------------------------------------------- */
