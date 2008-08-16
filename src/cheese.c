/*
 * Copyright (C) 2007,2008 daniel g. siegel <dgsiegel@gmail.com>
 * Copyright (C) 2007,2008 Jaap Haitsma <jaap@haitsma.org>
 * Copyright (C) 2008 Felix Kaser <f.kaser@gmx.net>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <cheese-config.h>
#endif

#include <stdio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <libgnomevfs/gnome-vfs.h>

#include "cheese-fileutil.h"
#include "cheese-window.h"
#include "cheese-dbus.h"

struct _CheeseOptions
{
  gboolean verbose;
  char *hal_device_id;
} CheeseOptions;

void cheese_print_handler (char *string)
{
  static FILE *fp = NULL;
  GDir *dir;
  char *filename, *path;
  
  CheeseFileUtil *fileutil = cheese_fileutil_new ();
  

  if (fp == NULL)
  {
    path = cheese_fileutil_get_log_path (fileutil);
    
    dir = g_dir_open (path, 0, NULL);
    if (!dir)
    {
      return;
    }
    
    filename = g_build_filename (path, "log", NULL);
    fp = fopen (filename, "w");

    g_object_unref (fileutil);
    g_free (filename);
  }

  if (fp)
    fputs (string, fp);

  if (CheeseOptions.verbose)
    fprintf (stdout, "%s", string);
    
}

int
main (int argc, char **argv)
{
  GOptionContext *context;
  CheeseDbus *dbus_server;
  
  GOptionEntry options[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &CheeseOptions.verbose, _("Be verbose"), NULL},
    { "hal-device", 'd', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &CheeseOptions.hal_device_id, NULL, NULL},
    { NULL }
  };
  CheeseOptions.hal_device_id = NULL;
  
  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gtk_rc_parse (APPNAME_DATA_DIR G_DIR_SEPARATOR_S "gtkrc");

  g_thread_init (NULL);
  gdk_threads_init ();

  g_set_application_name (_("Cheese"));

  context = g_option_context_new (N_("- Take photos and videos with your webcam, with fun graphical effects"));
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);
  g_option_context_add_group (context, gtk_get_option_group (TRUE));
  g_option_context_add_group (context, gst_init_get_option_group ());
  g_option_context_parse (context, &argc, &argv, NULL);
  g_option_context_free (context);

  dbus_server = cheese_dbus_new ();
  if (dbus_server == NULL)
  {
    return -1;
  }

  /* Needed for gnome_thumbnail functions */
  gnome_vfs_init ();

  g_set_print_handler ((GPrintFunc) cheese_print_handler);

  gtk_window_set_default_icon_name ("cheese");
  gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
                                     APPNAME_DATA_DIR G_DIR_SEPARATOR_S "icons");

  cheese_window_init (CheeseOptions.hal_device_id, dbus_server);
  
  gdk_threads_enter ();
  gtk_main ();
  gdk_threads_leave ();

  return 0;
}
