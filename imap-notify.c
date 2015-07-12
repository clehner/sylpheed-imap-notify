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
#include <ctype.h>

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
    "XX1 NOTIFY SET "
	"(selected (MessageNew MessageExpunge)) "
	"(inboxes (MessageNew))";
    // "XX2 NOTIFY SET (inboxes (MessageNew (uid body.peek[header.fields (from to subject)])))";
    // "XX2 notify set (inboxes (MessageNew FlagChange MessageExpunge) "
	// "(uid body.peek[header.fields (from to subject)]))";

static void init_done_cb(GObject *obj, gpointer data);
static void inc_mail_start_cb(GObject *obj, PrefsAccount *account);
// static gboolean check_item_cb(gpointer item);

static gint imap_recv_msg(Session *session, const gchar *msg);
static gint session_recv_noop(Session *session, const gchar *msg);
static IMAPSession *imap_recv_status(IMAPSession *session, const gchar *msg);
static gint parse_status_att_list	(gchar		*str,
					 gint		*messages,
					 gint		*recent,
					 guint32	*uid_next,
					 guint32	*uid_validity,
					 gint		*unseen);
static FolderItem *get_folder_item_for_mailbox(IMAPSession *session,
		const gchar *mailbox);
static IMAPSession *get_imap_notify_session(PrefsAccount *account);
static void imap_notify_session_init(Session *session);
static void imap_notify_session_deinit(Session *session);

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
	if (!str) return -1;
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

static IMAPSession *swap_session(IMAPSession *session, RemoteFolder *rfolder)
{
	Session *other_session;
	GSList *cur;

	g_return_val_if_fail(rfolder != NULL, NULL);

	other_session = rfolder->session;

	if (!other_session) {
		debug_print("IMAP NOTIFY no session to swap with.\n");
		return NULL;
	}

	if (other_session == SESSION(session)) {
		g_warning("imap-notify: same session\n");
		return NULL;
	}

	if (imap_is_session_active(IMAP_FOLDER(rfolder))) {
		g_warning("imap-notify: session busy\n");
		return NULL;
	}

	for (cur = sessions_list; cur != NULL; cur = cur->next) {
		if (cur->data == session) {
			cur->data = other_session;
			debug_print("IMAP NOTIFY swapping sessions.\n");
			break;
		}
	}

	imap_notify_session_deinit(SESSION(session));
	imap_notify_session_init(other_session);
	rfolder->session = SESSION(session);
	return IMAP_SESSION(other_session);
}

static IMAPSession *check_new(IMAPSession *session, FolderItem *item)
{
	IMAPSession *other_session;

	g_return_val_if_fail(item != NULL, session);
	g_return_val_if_fail(item->folder != NULL, session);

	/* Swap the async session for the main session, because the main
	 * session might now know about the new mail yet */
	other_session = swap_session(session, REMOTE_FOLDER(item->folder));
	if (other_session)
		session = other_session;

	/* Check for new mail */
	(void)syl_plugin_folderview_check_new_item(item);
	if (item == syl_plugin_summary_get_current_folder()) {
		syl_plugin_summary_show_queued_msgs();
		// syl_plugin_summary_update_by_msgnum(messages);
	}
	// g_timeout_add_full(G_PRIORITY_LOW, 100, check_item_cb, item, NULL);
	return session;
}

static IMAPSession *imap_recv_status(IMAPSession *session, const gchar *msg)
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
		if (str)
			*str++ = '\0';
	}

	if (parse_status_att_list(str, &messages, &recent, &uid_next,
				  &uid_validity, &unseen) < 0)
		return session;

	debug_print("IMAP NOTIFY status: \"%s\" %d %d %zu %zu %d\n",
			mailbox, messages, recent, uid_next, uid_validity, unseen);

	// syl_plugin_notification_window_open("IMAP NOTIFY: mailbox", mailbox, 10);
	log_print("IMAP NOTIFY: mailbox %s\n", mailbox);

	item = get_folder_item_for_mailbox(session, mailbox);

	if (!item) {
		debug_print("Got STATUS for unknown mailbox %s\n", mailbox);
		return session;
	}
	return check_new(session, item);
}

static IMAPSession *imap_recv_num(IMAPSession *session, const gchar *msg)
{
	gint num;

	for (num = 0; isdigit(*msg); msg++)
		num = num * 10 + *msg - '0';

	if (*msg++ != ' ')
		return 0;

	if (!strcmp(msg, "EXISTS")) {
		log_print("IMAP NOTIFY: EXISTS %d\n", num);
	} else if (!strcmp(msg, "RECENT")) {
		log_print("IMAP NOTIFY: RECENT %d\n", num);
	} else if (!strcmp(msg, "EXPUNGE")) {
		log_print("IMAP NOTIFY: EXPUNGE %d\n", num);
	} else {
		log_print("IMAP NOTIFY: unknown %s %d\n", msg, num);
	}

	return check_new(session, syl_plugin_summary_get_current_folder());
}

/*
static gboolean check_item_cb(gpointer item) {
	(void)syl_plugin_folderview_check_new_item(item);
	return G_SOURCE_REMOVE;
}
*/

static gint imap_recv_msg(Session *session, const gchar *msg)
{
	gint ret = 0;

	log_print("IMAP4<< %s\n", msg);
	session_set_access_time(SESSION(session));

	if (!strncmp(msg, "XX1 OK", 6)) {
		/* notify set */
		syl_plugin_notification_window_open("IMAP NOTIFY", "ready", 2);
	} else if (!strncmp(msg, "XX1 BAD", 7)) {
		/* notify error*/
		g_warning("imap-notify: error setting NOTIFY\n");
		return -1;
	} else if (!strncmp(msg, "XX2 OK", 6)) {
		/* session released back to core */
		session_set_recv_message_notify(session, NULL, NULL);
		return 0;
	} else if (*msg++ != '*' || *msg++ != ' ') {
		/* unknown */
	} else if (!strncmp(msg, "STATUS ", 7)) {
		IMAPSession *other_session =
			imap_recv_status(IMAP_SESSION(session), msg + 7);
		if (other_session)
			session = SESSION(other_session);
	} else if (isdigit(msg[0])) {
		IMAPSession *other_session =
			imap_recv_num(IMAP_SESSION(session), msg);
		if (other_session)
			session = SESSION(other_session);
	}

	debug_print("IMAP NOTIFY receiving on session %p\n", session);
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
	buf[sizeof buf-1] = '\0';

	debug_print("finding folder %s\n", buf);
	return folder_find_item_from_identifier(buf);
}

static void imap_notify_session_init(Session *session)
{
	session->recv_msg = imap_recv_msg;

	if (session_send_msg(session, SESSION_MSG_NORMAL, notify_str) < 0) {
		g_warning("imap-notify: error sending message\n");
	}
}

static void imap_notify_session_deinit(Session *session)
{
	session_send_msg(session, SESSION_MSG_NORMAL, "XX2 NOTIFY NONE");
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

	if (imap_is_session_active(IMAP_FOLDER(account->folder))) {
		/* TODO: retry after timeout */
		g_warning("imap-notify: session is busy\n");
		return NULL;
	}

	session = IMAP_SESSION(account->folder->session);
	if (session) {
		g_print("imap-notify: stealing IMAP session for NOTIFY\n");
		sessions_list = g_slist_prepend(sessions_list, session);
		account->folder->session = NULL;

		imap_notify_session_init(SESSION(session));
	}

        return session;
}

