# sylph-imap-notify

This is a plugin for the [Sylpheed][] mail client which adds support for the
IMAP NOTIFY extension ([RFC 5465][]) and the IMAP IDLE command ([RFC 2177][]).
It allows you to receive mail in real time instead of by polling.

IMAP NOTIFY is a successor to IMAP IDLE  and allows for getting
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

## Usage

Compile Sylpheed:

```
svn checkout svn://sylpheed.sraoss.jp/sylpheed/trunk sylpheed
./autogen.sh
./configure
make
```

Compile and install the plugin:

```
cd plugin
git clone https://github.com/clehner/sylph-imap-notify
cd sylph-imap-notify
make SYLPHEED_DIR=../../
make install
```

## Current issues

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
- Add internationalization

# Why a plugin

The LibSylph IMAP4 client implementation is synchronous. When IMAP NOTIFY is
used, notifications could arrive at any time. This would interfere with
LibSylph's IMAP code and would require significant changes. This plugin uses
a workaround of having a second IMAP session for the asynchronous events, so
that the core IMAP code is left untouched.
