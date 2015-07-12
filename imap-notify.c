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
	"IMAP NOTIFY",
	"0.0.1",
	"Charles Lehner",
	"IMAP NOTIFY implementation for Sylpheed"
};

static const gchar *notify_str =
    "XX1 SELECT INBOX\n"
    "XX2 NOTIFY SET "
	"(selected (MessageExpunge MessageNew "
	    "(uid body.peek[header.fields (from subject)]))) "
	"(inboxes (MessageNew))";
    // "XX2 NOTIFY SET (inboxes (MessageNew (uid body.peek[header.fields (from to subject)])))";
    // "XX2 notify set (inboxes (MessageNew FlagChange MessageExpunge) "
	// "(uid body.peek[header.fields (from to subject)]))";

typedef struct _MsgSummary
{
	Session *session;
	gchar *subject;
	gchar *from;
} MsgSummary;

static void init_done_cb(GObject *obj, gpointer data);
static void app_exit_cb(GObject *obj, gpointer data);
static void inc_mail_finished_cb(GObject *obj, gint new_messages);
static gboolean display_summaries(gpointer item);

static gint imap_recv_msg(Session *session, const gchar *msg);
static void imap_recv_status(IMAPSession *session, const gchar *msg);
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
static MsgSummary *get_current_session_summary(Session *);
static void msg_summary_show_if_complete(MsgSummary *summary);
static void summaries_list_free(void);
static void check_new(FolderItem *item);
static void check_new_debounced(FolderItem *item);

static GSList *sessions_list = NULL;

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
	syl_plugin_signal_connect("inc-mail-finished",
			G_CALLBACK(inc_mail_finished_cb), NULL);

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
}

static void app_exit_cb(GObject *obj, gpointer data)
{
	GSList *cur;

	g_print("imap-notify: %p: app will exit\n", obj);

	for (cur = sessions_list; cur != NULL; cur = cur->next)
		session_destroy(cur->data);
	g_slist_free(sessions_list);

	summaries_list_free();
}

static void inc_mail_finished_cb(GObject *obj, gint new_messages)
{
	GList *cur;

	for (cur = folder_get_list(); cur != NULL; cur = cur->next) {
		Folder *folder = cur->data;
		PrefsAccount *account = folder->account;
		if (account && account->protocol == A_IMAP4 &&
				account->folder->session)
			get_imap_notify_session(account);
	}
}

static void summaries_list_free(void)
{
	GSList *cur;

	for (cur = summaries.list; cur != NULL; cur = cur->next) {
		MsgSummary *summary = cur->data;
		g_free(summary->from);
		g_free(summary->subject);
		g_free(summary);
	}

	g_slist_free(summaries.list);
	summaries.len = 0;
	summaries.list = 0;
	summaries.total_msgs = 0;
	summaries.timer = 0;
}

static MsgSummary *get_current_session_summary(Session *session)
{
	MsgSummary *summary = NULL;
	GSList *cur;

	for (cur = summaries.list; cur != NULL; cur = cur->next) {
		summary = cur->data;
		if (summary->session == session)
			break;
	}

	if (!summary) {
		summary = g_new0(MsgSummary, 1);
		summary->session = session;
		summaries.list = g_slist_prepend(summaries.list, summary);
	}

	return summary;
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
	/* Check for new mail */
	/*
	g_return_val_if_fail(item != NULL, session);
	g_return_val_if_fail(item->folder != NULL, session);

	(void)syl_plugin_folderview_check_new_item(item);
	if (item == syl_plugin_summary_get_current_folder()) {
		syl_plugin_summary_show_queued_msgs();
		// syl_plugin_summary_update_by_msgnum(messages);
	}
	*/
	syl_plugin_folderview_check_new_item(item);
	syl_plugin_folderview_update_item(item, TRUE);
	// g_timeout_add_full(G_PRIORITY_LOW, 5000, check_item_cb, item, NULL);
}

static gboolean check_new_cb(gpointer _item) {
	FolderItem *item = _item;
	item->cache_dirty = FALSE;
	check_new(item);
	return G_SOURCE_REMOVE;
}

static void check_new_debounced(FolderItem *item)
{
	if (item->cache_dirty) return;
	item->cache_dirty = TRUE;
	g_timeout_add_full(G_PRIORITY_LOW, 50, check_new_cb, item, NULL);
}

static void imap_recv_status(IMAPSession *session, const gchar *msg)
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

static void imap_recv_num(IMAPSession *session, gint num, const gchar *msg)
{
	if (!strcmp(msg, "EXISTS")) {
		log_print("IMAP NOTIFY: EXISTS %d\n", num);
	} else if (!strcmp(msg, "RECENT")) {
		FolderItem *item;
		log_print("IMAP NOTIFY: RECENT %d\n", num);
		if (num == 0) return;
		check_new_debounced(session->folder->inbox);
	} else if (!strcmp(msg, "EXPUNGE")) {
		log_print("IMAP NOTIFY: EXPUNGE %d\n", num);
	} else if (!strncmp(msg, "FETCH ", 6) && strchr(msg + 6, '{')) {
		MsgSummary *summary;
		/* receiving some headers of a new message */
		summaries.total_msgs++;
		if (summaries.len < 5)
			get_current_session_summary(SESSION(session));
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

	g_snprintf(title, sizeof title-2, _("Sylpheed: %d new messages"),
			summaries.total_msgs);
	strcat(title, "!");
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

	return G_SOURCE_REMOVE;
}

static gint imap_recv_msg(Session *session, const gchar *msg)
{
	log_print("IMAP4<< %s\n", msg);
	session_set_access_time(SESSION(session));

	if (!msg || msg[0] == ')') {
		/* fetch done */
	} else if (!strncmp(msg, "XX1 OK", 6)) {
		/* inbox selected */
	} else if (!strncmp(msg, "XX2 OK", 6)) {
		/* notify set */
		syl_plugin_notification_window_open("IMAP NOTIFY", "ready", 2);
	} else if (!strncmp(msg, "XX1 BAD", 7)) {
		/* notify error*/
		g_warning("imap-notify: error setting NOTIFY\n");
		return -1;
	} else if (!strncmp(msg, "From: ", 4)) {
		MsgSummary *summary = get_current_session_summary(session);
		summary->from = g_strdup(msg + 4);
		msg_summary_show_if_complete(summary);
	} else if (!strncmp(msg, "Subject: ", 9)) {
		MsgSummary *summary = get_current_session_summary(session);
		summary->subject = g_strdup(msg + 9);
		msg_summary_show_if_complete(summary);
	} else if (*msg++ != '*' || *msg++ != ' ') {
		/* unknown */
	} else if (!strncmp(msg, "STATUS ", 7)) {
		imap_recv_status(IMAP_SESSION(session), msg + 7);
	} else if (isdigit(msg[0])) {
		gint num = atoi(msg);
		msg = strchr(msg, ' ');
		if (msg++)
			imap_recv_num(IMAP_SESSION(session), num, msg);
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

	return folder_find_item_from_identifier(buf);
}

static void imap_notify_session_init(Session *session)
{
	session->recv_msg = imap_recv_msg;

	if (session_send_msg(session, SESSION_MSG_NORMAL, notify_str) < 0) {
		g_warning("imap-notify: error sending message\n");
	}
}

static IMAPSession *get_imap_notify_session(PrefsAccount *account)
{
	IMAPSession *session;
	GSList *cur;

	g_return_val_if_fail(account != NULL, NULL);
	g_return_val_if_fail(account->folder != NULL, NULL);

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
	if (!session) {
		g_warning("imap-notify: no session\n");
		return NULL;
	}

	log_print("IMAP NOTIFY: obtaining IMAP session for account %s\n",
			account->account_name);
	sessions_list = g_slist_prepend(sessions_list, session);
	account->folder->session = NULL;

	imap_notify_session_init(SESSION(session));

	return session;
}

