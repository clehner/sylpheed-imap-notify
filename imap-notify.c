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
    "XX1 CLOSE\nXX2 NOTIFY SET (inboxes (MessageNew FlagChange MessageExpunge))";

static void init_done_cb(GObject *obj, gpointer data);
static void app_exit_cb(GObject *obj, gpointer data);
static void inc_mail_start_cb(GObject *obj, PrefsAccount *account);
static gint imap_recv_msg(Session *session, const gchar *msg);
static gint imap_recv_status(IMAPSession *session, const gchar *msg);
static gint parse_status_att_list	(gchar		*str,
					 gint		*messages,
					 gint		*recent,
					 guint32	*uid_next,
					 guint32	*uid_validity,
					 gint		*unseen);
static FolderItem *get_folder_item_for_mailbox(IMAPSession *session,
		const gchar *mailbox);
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
       GSList *cur;

       g_print("IMAP NOTIFY plugin unloading\n");

       for (cur = sessions_list; cur != NULL; cur = cur->next)
	       session_destroy(cur->data);

       g_slist_free(sessions_list);
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
}

static void app_exit_cb(GObject *obj, gpointer data)
{
       g_print("imap-notify: %p: app will exit\n", obj);
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
	FolderItem *item;

	log_print("IMAP NOTIFY STATUS: %s\n", msg);

	if (*str == '"') {
		mailbox = str;
		extract_quote_with_escape(mailbox, '"');
		mailbox++;
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

	syl_plugin_notification_window_open("IMAP NOTIFY: mailbox", mailbox, 10);

	item = get_folder_item_for_mailbox(session, mailbox);
	if (!item) {
		debug_print("Got STATUS for unknown mailbox %s\n", mailbox);
		return 0;
	}

	(void)syl_plugin_folderview_check_new_item(item);

	return 0;
}

static gint imap_recv_msg(Session *session, const gchar *msg)
{
	gint ret = 0;

	log_print("IMAP4<< %s\n", msg);

	if (!strncmp(msg, "XX2 OK", 6)) {
		/* notify set */
		syl_plugin_notification_window_open("IMAP NOTIFY", "ready", 5);
	} else if (*msg++ != '*' || *msg++ != ' ') {
		/* unknown */
	} else if (!strncmp(msg, "STATUS ", 7)) {
		ret = imap_recv_status(IMAP_SESSION(session), msg + 7);
	}

	return session_recv_msg(session);
}

/*
	log_print("IMAP notify? %hhu\n", imap_has_capability(session, "NOTIFY"));
	if (imap_has_capability(session, "NOTIFY")) {
	    if (imap_cmd_notify(session, "(inboxes (MessageNew FlagChange MessageExpunge))")
	    != IMAP_SUCCESS) {
	    */

static FolderItem *get_folder_item_for_mailbox(IMAPSession *session,
		const gchar *mailbox)
{

	gchar buf[512];
	gchar *str;

	for (str = (gchar *)mailbox; *str; str++)
		if (*str == '.')
			*str = '/';

	snprintf(buf, sizeof buf, "#imap/%s/%s",
			session->folder->account->account_name, mailbox);

	log_message("IMAP NOTIFY mailbox: %s. name: %s\n", buf);
	// return folder_find_item_from_path(mailbox);
	return folder_find_item_from_identifier(buf);
	// #imap/cel/Subscriptions/11:11/foo"pie
	// "Subscriptions.11:11.foo\"pie"
	// #imap/cel/Subscriptions/11:11/a.d
	// Subscriptions.11:11.a.d
}

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
		syl_plugin_notification_window_open("IMAP NOTIFY", "stole session", 6);
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

