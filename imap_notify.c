/*
 * sylph-imap-notify -- IMAP NOTIFY plugin for Sylpheed
 * Copyright (C) 2015 Charles Lehner
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <ctype.h>

#include "sylmain.h"
#include "plugin.h"
#include "folder.h"
#include "imap.h"
#include "session.h"
#include "prefs_common.h"

static SylPluginInfo info = {
	"IMAP NOTIFY Plug-in",
	"0.1.0",
	"Charles Lehner",
	"IMAP NOTIFY implementation for Sylpheed"
};

static const gchar
    *notify_str =
	"XX1 NOTIFY SET "
	    "(selected (MessageExpunge MessageNew "
	    "(uid body.peek[header.fields (from subject)]))) "
	    "(inboxes (MessageNew))\n"
	"XX2 SELECT INBOX",
    *notify_str_no_summaries =
	"XX1 NOTIFY SET "
	    "(inboxes (MessageNew))\n"
	"XX2 CLOSE";

static const gint noop_interval = 60 * 29;

typedef struct _MsgSummary
{
	gchar *subject;
	gchar *from;
} MsgSummary;

typedef struct _IMAPNotifySession
{
	IMAPSession *imap_session;
	Folder *folder;
	MsgSummary *current_summary;
	gint noop_tag;
} IMAPNotifySession;

static void inc_mail_finished_cb(GObject *obj, gint new_messages);
static void imap_notify_session_noop_cb(IMAPSession *session);
static gboolean display_summaries(gpointer item);

static gint imap_recv_msg(Session *session, const gchar *msg);
static void imap_recv_status(IMAPNotifySession *session, const gchar *msg);
static gint parse_status_att_list	(gchar		*str,
					 gint		*messages,
					 gint		*recent,
					 guint32	*uid_next,
					 guint32	*uid_validity,
					 gint		*unseen);
static FolderItem *get_folder_item_for_mailbox(IMAPNotifySession *session,
		const gchar *mailbox);
static IMAPNotifySession *get_imap_notify_session(PrefsAccount *account);
static void steal_imap_notify_session(PrefsAccount *account);
static gboolean has_imap_notify_session(PrefsAccount *account);
static void imap_notify_session_init(IMAPSession *session);
static void msg_summary_show_if_complete(MsgSummary *summary);
static void summaries_list_free(void);
static void check_new(FolderItem *item);
static void check_new_debounced(FolderItem *item);

static void imap_notify_session_destroy(IMAPNotifySession *);
static void imap_notify_session_send(IMAPNotifySession *session,
		const gchar *msg);
static gboolean imap_notify_session_noop(gpointer item);

static GSList *sessions_list = NULL;

static GSList *(*original_get_msg_list)(Folder *folder, FolderItem *item,
		gboolean use_cache);
static GSList *wrapped_get_msg_list(Folder *folder, FolderItem *item,
		gboolean use_cache);

static struct {
	GSList *list;
	gint len;
	gint timer;
	gint total_msgs;
} summaries = {NULL, 0, 0, 0};

void plugin_load(void)
{
	GList *list, *cur;
	const gchar *ver;
	gpointer mainwin;
	FolderClass *imap_class;

	g_print("IMAP NOTIFY plug-in loaded!\n");

	syl_plugin_signal_connect("inc-mail-finished",
			G_CALLBACK(inc_mail_finished_cb), NULL);

	/* Wrap IMAP get_msg_list so we can snatch sessions */
	imap_class = imap_get_class();
	original_get_msg_list = imap_class->get_msg_list;
	imap_class->get_msg_list = wrapped_get_msg_list;
}

void plugin_unload(void)
{
	GSList *cur;

	g_print("IMAP NOTIFY plugin unloaded\n");

	g_slist_free_full(sessions_list,
			(GDestroyNotify)imap_notify_session_destroy);

	imap_get_class()->get_msg_list = original_get_msg_list;

	summaries_list_free();
}

SylPluginInfo *plugin_info(void)
{
	return &info;
}

gint plugin_interface_version(void)
{
	return SYL_PLUGIN_INTERFACE_VERSION;
}

static gboolean account_cb(gpointer data)
{
	PrefsAccount *account = data;

	if (syl_plugin_summary_is_locked() || syl_plugin_inc_is_active())
		return G_SOURCE_CONTINUE;

	if (!has_imap_notify_session(account))
		steal_imap_notify_session(account);

	return G_SOURCE_REMOVE;
}

static GSList *wrapped_get_msg_list(Folder *folder, FolderItem *item,
		gboolean use_cache)
{
	GSList *list = original_get_msg_list(folder, item, use_cache);

	/* Ensure that we have a NOTIFY session for this account */
	if (!has_imap_notify_session(folder->account)) {
		/* Schedule getting the session.
		/* Use a timeout instead of idle so that we don't get stuck
		 * in GTK_EVENTS_FLUSH in summaryview_show */
		gdk_threads_add_timeout(10, account_cb, folder->account);
	}

	return list;
}

static void inc_mail_finished_cb(GObject *obj, gint new_messages)
{
	GList *cur;

	for (cur = folder_get_list(); cur != NULL; cur = cur->next) {
		Folder *folder = cur->data;
		PrefsAccount *account = folder->account;
		if (account && account->protocol == A_IMAP4 &&
				account->folder->session &&
				!has_imap_notify_session(account))
			steal_imap_notify_session(account);
	}
}

static void imap_notify_session_destroy(IMAPNotifySession *session)
{
	sessions_list = g_slist_remove(sessions_list, session);
	session_destroy(SESSION(session->imap_session));
	g_source_remove(session->noop_tag);
	g_free(session);
}

static void summary_free(MsgSummary *summary)
{
	g_free(summary->from);
	g_free(summary->subject);
	g_free(summary);
}

static void summaries_list_free(void)
{
	g_slist_free_full(summaries.list, (GDestroyNotify)summary_free);
	summaries.list = NULL;
	summaries.len = 0;
	summaries.total_msgs = 0;
	summaries.timer = 0;
}

static void msg_summary_show_if_complete(MsgSummary *summary) {
	if (!summary->from || !summary->subject) {
		debug_print("summary not ready. %s %s\n", summary->from,
				summary->subject);
	}

	/* debounce showing the notification */
	if (summaries.timer)
		g_source_remove(summaries.timer);
	summaries.timer = g_timeout_add_full(G_PRIORITY_LOW, 80,
			display_summaries, NULL, NULL);
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

static void check_new(FolderItem *item)
{
	/* Update folder view */
	syl_plugin_folderview_check_new_item(item);
	/* Update summary */
	syl_plugin_folderview_update_item(item, TRUE);
}

static gboolean check_new_cb(gpointer _item) {
	FolderItem *item = _item;
	item->cache_dirty = FALSE;
	check_new(item);
	return G_SOURCE_REMOVE;
}

static void check_new_debounced(FolderItem *item)
{
	g_return_val_if_fail(item != NULL, NULL);

	if (item->cache_dirty) return;
	item->cache_dirty = TRUE;
	g_timeout_add_full(G_PRIORITY_LOW, 50, check_new_cb, item, NULL);
}

static void imap_recv_status(IMAPNotifySession *session, const gchar *msg)
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
		return;

	debug_print("IMAP NOTIFY status: \"%s\" %d %d %zu %zu %d\n",
			mailbox, messages, recent, uid_next, uid_validity, unseen);

	item = get_folder_item_for_mailbox(session, mailbox);

	if (!item) {
		debug_print("Got STATUS for unknown mailbox %s\n", mailbox);
		return;
	}

	if (unseen || recent)
		check_new_debounced(item);
}

static void imap_recv_num(IMAPNotifySession *session, gint num,
		const gchar *msg)
{
	if (!strcmp(msg, "EXISTS")) {
		log_print("IMAP NOTIFY: EXISTS %d\n", num);
	} else if (!strcmp(msg, "RECENT")) {
		log_print("IMAP NOTIFY: RECENT %d\n", num);
		if (num == 0) return;
		check_new_debounced(session->folder->inbox);
	} else if (!strcmp(msg, "EXPUNGE")) {
		log_print("IMAP NOTIFY: EXPUNGE %d\n", num);
	} else if (!strncmp(msg, "FETCH ", 6) && strchr(msg + 6, '{')) {
		/* receiving some headers of a new message */
		summaries.total_msgs++;
		if (prefs_common.enable_newmsg_notify_window &&
				summaries.len < 5) {
			MsgSummary *summary = g_new0(MsgSummary, 1);
			session->current_summary = summary;
			summaries.list = g_slist_prepend(summaries.list,
					summary);
		}
		check_new_debounced(session->folder->inbox);
	} else {
		log_print("IMAP NOTIFY: unknown %s %d\n", msg, num);
	}
}

static gboolean display_summaries(gpointer item) {
	GSList *cur;
	gchar title[1024];
	GString *str;

	summaries.timer = 0;

	g_snprintf(title, sizeof title, _("Sylpheed: %d new messages"),
			summaries.total_msgs);
	str = g_string_new("");

	log_message(title);
	for (cur = summaries.list; cur != NULL; cur = cur->next) {
		MsgSummary *summary = cur->data;
		gchar *markup;

		if (str->len > 0)
			g_string_append_c(str, '\n');
		markup = g_markup_printf_escaped("<b>%s</b>  %s",
				    summary->subject, summary->from);
		g_string_append(str, markup);
		g_free(markup);
	}

	syl_plugin_notification_window_open(title, str->str,
			prefs_common.notify_window_period);

	g_string_free(str, TRUE);
	summaries_list_free();

	/* play sound */
	if (prefs_common.enable_newmsg_notify_sound &&
	    prefs_common.newmsg_notify_sound) {
		play_sound(prefs_common.newmsg_notify_sound, TRUE);
	}

	/* execute command */
	if (prefs_common.enable_newmsg_notify &&
	    prefs_common.newmsg_notify_cmd) {
		gchar buf[1024];

		if (str_find_format_times
			(prefs_common.newmsg_notify_cmd, 'd') == 1)
			g_snprintf(buf, sizeof(buf),
				   prefs_common.newmsg_notify_cmd,
				   summaries.total_msgs);
		else
			strncpy2(buf, prefs_common.newmsg_notify_cmd,
				 sizeof(buf));
		execute_command_line(buf, TRUE);
	}

	return G_SOURCE_REMOVE;
}

static gboolean imap_notify_session_noop(gpointer item)
{
	imap_notify_session_send(item, "XX3 NOOP");
	return G_SOURCE_CONTINUE;
}

static gint imap_recv_msg(Session *_session, const gchar *msg)
{
	IMAPNotifySession *session = _session->data;
	GSList *cur;

	log_print("IMAP4<< %s\n", msg);

	if (!msg || msg[0] == ')') {
		/* fetch done */
	} else if (!strncmp(msg, "XX1 OK", 6)) {
		/* notify set */
		session->noop_tag = g_timeout_add_seconds_full(
				G_PRIORITY_LOW, noop_interval,
				imap_notify_session_noop, session, NULL);
		syl_plugin_notification_window_open("IMAP NOTIFY", "ready", 2);
	} else if (!strncmp(msg, "XX2 OK", 6)) {
		/* inbox selected */
	} else if (!strncmp(msg, "XX3 OK", 6)) {
		/* noop ok */
	} else if (!strncmp(msg, "XX1 BAD", 7)) {
		g_warning("IMAP NOTIFY not supported by %s\n",
			session->folder->account->recv_server);
		/* keep the session existing so we know not to reconnect */
		return 0;
	} else if (!strncmp(msg, "XX2 BAD", 7)) {
		debug_print("IMAP NOTIFY: error selecting/closing mailbox\n");
	} else if (!strncmp(msg, "From: ", 4)) {
		MsgSummary *summary = session->current_summary;
		if (summary && prefs_common.enable_newmsg_notify_window) {
			summary->from = g_strdup(msg + 4);
			msg_summary_show_if_complete(summary);
		}
	} else if (!strncmp(msg, "Subject: ", 9)) {
		MsgSummary *summary = session->current_summary;
		if (summary && prefs_common.enable_newmsg_notify_window) {
			summary->subject = g_strdup(msg + 9);
			msg_summary_show_if_complete(summary);
		}
	} else if (*msg++ != '*' || *msg++ != ' ') {
		/* unknown */
	} else if (!strncmp(msg, "STATUS ", 7)) {
		imap_recv_status(session, msg + 7);
	} else if (!strncmp(msg, "BYE ", 7)) {
		imap_notify_session_destroy(session);
		return 0;
	} else if (isdigit(msg[0])) {
		gint num = atoi(msg);
		msg = strchr(msg, ' ');
		if (msg++)
			imap_recv_num(session, num, msg);
	}

	return session_recv_msg(&session->imap_session->session);
}

static void imap_notify_session_send(IMAPNotifySession *session,
		const gchar *msg)
{
	log_print("IMAP4>> %s\n", msg);

	if (session_send_msg(SESSION(session->imap_session),
				SESSION_MSG_NORMAL, msg) < 0) {
		g_warning("imap-notify: error sending message\n");
	}
}

static FolderItem *get_folder_item_for_mailbox(IMAPNotifySession *session,
		const gchar *mailbox)
{

	gchar buf[512];
	gchar *str;

	for (str = (gchar *)mailbox; *str; str++)
		if (*str == '.')
			*str = '/';

	g_snprintf(buf, sizeof buf, "#imap/%s/%s",
			session->folder->account->account_name, mailbox);

	return folder_find_item_from_identifier(buf);
}

static void imap_notify_session_init(IMAPSession *session)
{
	GList *cur;
	Folder *folder = NULL;
	IMAPNotifySession *notify_session;

	g_return_val_if_fail(session != NULL, NULL);

	for (cur = folder_get_list(); cur != NULL; cur = cur->next) {
		folder = cur->data;
		if (folder->account && folder->account->protocol == A_IMAP4 &&
		    REMOTE_FOLDER(folder)->session == SESSION(session))
			break;
	}
	if (folder == NULL) {
		g_warning("imap-notify: can't find folder\n");
		return;
	}

	notify_session = g_new0(IMAPNotifySession, 1);
	notify_session->imap_session = session;
	notify_session->folder = folder;
	SESSION(session)->data = notify_session;
	SESSION(session)->recv_msg = imap_recv_msg;

	sessions_list = g_slist_prepend(sessions_list, notify_session);

	imap_notify_session_send(notify_session,
			prefs_common.enable_newmsg_notify_window ?
			notify_str : notify_str_no_summaries);
}

static IMAPNotifySession *get_imap_notify_session(PrefsAccount *account)
{
	GSList *cur;
	IMAPNotifySession *notify_session = NULL;

	for (cur = sessions_list; cur != NULL; cur = cur->next) {
		notify_session = cur->data;
		if (notify_session->folder->account == account)
			return notify_session;
	}

	return NULL;
}

static gboolean has_imap_notify_session(PrefsAccount *account)
{
	IMAPNotifySession *notify_session = get_imap_notify_session(account);

	if (notify_session) {
		if (notify_session->imap_session->session.state ==
				SESSION_EOF) {
			debug_print("IMAP NOTIFY session ended\n");
			imap_notify_session_destroy(notify_session);
			return FALSE;
		}
		return TRUE;
	}

	return FALSE;
}

static void steal_imap_notify_session(PrefsAccount *account)
{
	IMAPSession *imap_session;
	IMAPNotifySession *imap_notify_session;

	g_return_val_if_fail(account != NULL, NULL);
	g_return_val_if_fail(account->folder != NULL, NULL);

	/* No NOTIFY session yet. Try to steal one from the folder */

	if (imap_is_session_active(IMAP_FOLDER(account->folder))) {
		/* TODO: retry after timeout */
		g_warning("imap-notify: session is busy\n");
		return;
	}

	imap_session = IMAP_SESSION(account->folder->session);
	if (!imap_session) {
		g_warning("imap-notify: no session\n");
		return;
	}

	log_print("IMAP NOTIFY: obtaining IMAP session for account %s\n",
			account->account_name);
	imap_notify_session_init(imap_session);
	account->folder->session = NULL;
}
