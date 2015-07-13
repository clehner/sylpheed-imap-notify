# sylph-imap-notify

This is a plugin for the [Sylpheed][] mail client which adds support for the
IMAP NOTIFY extension ([RFC 5465][]). It allows you to receive mail in real
time instead of by polling.

IMAP NOTIFY is a successor to IMAP IDLE ([RFC 2177][]) and allows for getting
notifications about more than one mailbox at a time on a single connection, and
allows for listening for arbitrary mailbox events. However, it is not widely
implemented. (It's author, Arnt Gulbrandsen, [said][Arnt] "it should have been good but is a disaster"). However, it is supported by the [Dovecot][] and [Archiveopteryx][] IMAP servers.

[Sylpheed]: http://sylpheed.sraoss.jp/en/
[RFC 5465]: https://tools.ietf.org/html/rfc5465
[RFC 2177]: https://tools.ietf.org/html/rfc2177
[Archiveopteryx]: http://www.archiveopteryx.org/
[Dovecot]: http://dovecot.org/
[Arnt]: http://rant.gulbrandsen.priv.no/good-bad-rfc

## Features

- When mail arrives, the folder view is updated. The summary view is updated if
  it is currently showing the folder containing the new mail.
- When mail arrives in an Inbox, a new mail notification is shown if that
  preference is enabled.

## License

GPLv3+

## Current issues

- THe IMAP NOTIFY listener does not start for an IMAP account until the account
  is scanned once for new mail. It will show a popup notification saying
  "ready" to indicate that it has started. If the session gets disconnected, it
  won't reconnect until the account is scanned for new mail again. Therefore it
  is a good idea to keep the Auto-check new mail preference enabled, and
  perhaps the check new mail on startup preference as well.

- If you start Sylpheed with notifications disabled and then enable
  them, you will have to restart the application for that setting to take
  effect in this plugin.

- The plugin only handles new mail notificaions. It does not
  handle message expunge events, flag changes, mailbox changes, or other
  events.

# Todo

- Show better indication of when IMAP NOTIFY is running on an account
- Check account capabilities before trying to use NOTIFY
- Add an account preference for using NOTIFY or not
- Show fine-grained updates without having to reload the entire message summary
  view.

# Why a plugin

The LibSylph IMAP4 client implementation is synchronous. When IMAP NOTIFY is
used, notifications could arrive at any time. This would interfere with
LibSylph's IMAP code and would require significant changes. This plugin uses
a workaround of having a second IMAP session for the asynchronous events, so
that the core IMAP code is left untouched.
