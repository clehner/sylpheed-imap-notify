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
#include "imap.h"
#include "session.h"

static SylPluginInfo info = {
        "IMAP NOTIFY",
        "0.0.1",
        "Charles Lehner",
        "IMAP NOTIFY implementation for Sylpheed"
};

static const gchar *notify_str =
    "XX1 NOTIFY SET (inboxes (MessageNew FlagChange MessageExpunge))";

static void init_done_cb(GObject *obj, gpointer data);
static void app_exit_cb(GObject *obj, gpointer data);
static void inc_mail_start_cb(GObject *obj, PrefsAccount *account);
static gint imap_recv_msg(Session *session, const gchar *msg);
static gint imap_recv_status(IMAPSession *session, const gchar *msg);
static gint imap_recv_capability(IMAPSession *session, const gchar *msg);
static gint parse_status_att_list	(gchar		*str,
					 gint		*messages,
					 gint		*recent,
					 guint32	*uid_next,
					 guint32	*uid_validity,
					 gint		*unseen);

static IMAPSession *get_imap_notify_session(PrefsAccount *account);

static GSList *sessions_list = NULL;

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

       g_signal_connect(syl_app_get(), "init-done", G_CALLBACK(init_done_cb),
                        NULL);
       g_signal_connect(syl_app_get(), "app-exit", G_CALLBACK(app_exit_cb),
                        NULL);
       syl_plugin_signal_connect("inc-mail-start",
                                 G_CALLBACK(inc_mail_start_cb), NULL);

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
       GtkWidget *btn = gtk_button_new_with_label("BTN");
       gtk_widget_show(btn);
       g_print("imap-notify: %p: app init done\n", obj);
       syl_plugin_folderview_add_sub_widget(btn);
       // syl_plugin_notification_window_open("HI", "yep\nyep", 8);
}

static void app_exit_cb(GObject *obj, gpointer data)
{
       GSList *cur;

       g_print("imap-notify: %p: app will exit\n", obj);

       for (cur = sessions_list; cur != NULL; cur = cur->next)
	       session_destroy(cur->data);
       g_slist_free(sessions_list);
}

static void inc_mail_start_cb(GObject *obj, PrefsAccount *account)
{
       IMAPSession *session = get_imap_notify_session(account);
       // syl_plugin_notification_window_open("IMAP NOTIFY", "starting", 6);
}

static gint parse_status_att_list(gchar *str,
				  gint *messages, gint *recent,
				  guint32 *uid_next, guint32 *uid_validity,
				  gint *unseen)
{
	str = strrchr_with_skip_quote(str, '"', '(');
	if (!str) return -1;
	str++;

	while (*str != '\0' && *str != ')') {
		while (*str == ' ') str++;

		if (!strncmp(str, "MESSAGES ", 9)) {
			str += 9;
			*messages = strtol(str, &str, 10);
		} else if (!strncmp(str, "RECENT ", 7)) {
			str += 7;
			*recent = strtol(str, &str, 10);
		} else if (!strncmp(str, "UIDNEXT ", 8)) {
			str += 8;
			*uid_next = strtoul(str, &str, 10);
		} else if (!strncmp(str, "UIDVALIDITY ", 12)) {
			str += 12;
			*uid_validity = strtoul(str, &str, 10);
		} else if (!strncmp(str, "UNSEEN ", 7)) {
			str += 7;
			*unseen = strtol(str, &str, 10);
		} else {
			g_warning("invalid STATUS response: %s\n", str);
			break;
		}
	}

	return 0;
}

static gint imap_recv_status(IMAPSession *session, const gchar *msg)
{
	gchar *str = (gchar *)msg;
	gchar *mailbox;
	gint messages = 0;
	gint recent = 0;
	guint32 uid_next = 0;
	guint32 uid_validity = 0;
	gint unseen = 0;

	if (*str == '"') {
		mailbox = str + 1;
		str = extract_quote_with_escape(str, '"') + 1;
	} else {
		mailbox = str;
		str = strchr(str, ' ');
		*str++ = '\0';
	}

	if (parse_status_att_list(str, &messages, &recent, &uid_next,
				  &uid_validity, &unseen) < 0)
		return 0;

	debug_print("IMAP NOTIFY status: \"%s\" %d %d %zu %zu %d\n",
			mailbox, messages, recent, uid_next, uid_validity, unseen);
}

static gint imap_recv_capability(IMAPSession *session, const gchar *msg)
{
	session->capability = g_strsplit(msg, " ", -1);
	return 0;
}

static gint imap_recv_msg(Session *session, const gchar *msg)
{
	gint ret;

	log_print("IMAP4<< %s\n", msg);
	syl_plugin_notification_window_open("IMAP NOTIFY: recv", msg, 9);

	if (!strncmp(msg, "XX1 OK", 6)) {
		/* notify set */
		ret = 0;
	} else if (*msg++ != '*' || *msg++ != ' ') {
		/* unknown */
		ret = 0;
	} else if (!strncmp(msg, "STATUS ", 7)) {
		log_message("IMAP NOTIFY STATUS\n");
		ret = imap_recv_status(IMAP_SESSION(session), msg + 7);
	} else if (!strncmp(msg, "CAPABILITY ", 11)) {
		log_message("IMAP CAPABILITY\n");
		ret = imap_recv_capability(IMAP_SESSION(session), msg + 11);
	}

	return session_recv_msg(session);
}

/*
	log_print("IMAP notify? %hhu\n", imap_has_capability(session, "NOTIFY"));
	if (imap_has_capability(session, "NOTIFY")) {
	    if (imap_cmd_notify(session, "(inboxes (MessageNew FlagChange MessageExpunge))")
	    != IMAP_SUCCESS) {
	    */

static IMAPSession *get_imap_notify_session(PrefsAccount *account)
{
	IMAPSession *session;
	GSList *cur;

	g_return_val_if_fail(account != NULL, NULL);

	for (cur = sessions_list; cur != NULL; cur = cur->next) {
		session = (IMAPSession *)cur->data;
		if (session->folder == (Folder *)account->folder)
                        return session;
        }

	/* No NOTIFY session yet. Try to steal one from the folder */
	session = IMAP_SESSION(account->folder->session);
	if (session) {
		g_print("imap-notify: stealing IMAP session for NOTIFY\n");
		sessions_list = g_slist_prepend(sessions_list, session);
		account->folder->session = NULL;

		/* Init the session */
		SESSION(session)->recv_msg = imap_recv_msg;

		if (session_send_msg(SESSION(session), SESSION_MSG_NORMAL,
					notify_str) < 0) {
			g_warning("imap-notify: error sending message\n");
		}
	}

        return session;
}

