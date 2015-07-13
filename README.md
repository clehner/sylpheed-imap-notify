# sylph-imap-notify

This is a plugin for the [Sylpheed][] mail client which adds support for the
IMAP NOTIFY extension ([RFC 5465][]). It allows you to receive mail in real
time instead of by polling.

[Sylpheed]: http://sylpheed.sraoss.jp/en/
[RFC 5465]: https://tools.ietf.org/html/rfc5465

## Features

- When mail arrives, the folder view is updated. The summary view is updated if
  it is currently showing the folder containing the new mail.
- New mail notifications are shown if that preference is enabled.

## License

GPLv3+

## Current issues

- IMAP NOTIFY does not start for an IMAP account until the account is scanned
  for new mail. This is because it is hard to get a reference to the IMAP
  sessions at other times. It will show a popup notification saying "ready" to
  indicate that it has started.

- If you start Sylpheed with notifications disabled and then enable
  them, you will have to restart Sylpheed for that setting to take
  effect in this plugin.

# Todo

- show better indication of when IMAP NOTIFY is running on an account
- check account capabilities before trying to use NOTIFY
- add an account preference for using NOTIFY or not
