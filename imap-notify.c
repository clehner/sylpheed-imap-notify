/*
 * sylph-imap-notify -- IMAP NOTIFY plugin for Sylpheed
 * Copyright (C) 2015 Charles Lehner
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <glib.h>
#include <gtk/gtk.h>

#include "sylmain.h"
#include "plugin.h"
#include "folder.h"

static SylPluginInfo info = {
        "IMAP NOTIFY",
        "0.0.1",
        "Charles Lehner",
        "IMAP NOTIFY implementation for Sylpheed"
};

static void init_done_cb(GObject *obj, gpointer data);
static void app_exit_cb(GObject *obj, gpointer data);

static void show_notif(void);

void plugin_load(void)
{
       GList *list, *cur;
       const gchar *ver;
       gpointer mainwin;

       g_print("IMAP NOTIFY plug-in loaded!\n");

       list = folder_get_list();
       g_print("folder list = %p\n", list);
       for (cur = list; cur != NULL; cur = cur->next) {
               Folder *folder = FOLDER(cur->data);
               gchar *id = folder_get_identifier(folder);
               g_print("folder id = %s\n", id);
       }

       ver = syl_plugin_get_prog_version();
       g_print("program ver: %s\n", ver);

       mainwin = syl_plugin_main_window_get();
       g_print("mainwin: %p\n", mainwin);
       syl_plugin_main_window_popup(mainwin);

       syl_plugin_add_menuitem("/Tools", NULL, NULL, NULL);
       syl_plugin_add_menuitem("/Tools", "Notify", show_notif, NULL);

       g_signal_connect(syl_app_get(), "init-done", G_CALLBACK(init_done_cb),
                        NULL);
       g_signal_connect(syl_app_get(), "app-exit", G_CALLBACK(app_exit_cb),
                        NULL);

       g_print("imap-notify plug-in loading done\n");
}

void plugin_unload(void)
{
       g_print("IMAP NOTIFY plugin unloaded\n");
}

SylPluginInfo *plugin_info(void)
{
       return &info;
}

gint plugin_interface_version(void)
{
       return SYL_PLUGIN_INTERFACE_VERSION;
}

static void init_done_cb(GObject *obj, gpointer data)
{
       g_print("imap-notify: %p: app init done\n", obj);
       syl_plugin_notification_window_open("HI", "yep\nyep", 8);
}

static void app_exit_cb(GObject *obj, gpointer data)
{
       g_print("imap-notify: %p: app will exit\n", obj);
}

static void show_notif(void)
{
       syl_plugin_notification_window_open("HI", "yep\nyep", 8);
}
