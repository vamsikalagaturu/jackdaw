# jackdaw

Keyboard-first mail client for Linux and Android. C11 + raylib, IMAP/SMTP, offline store in SQLite.

## Build (Linux)

```
platform/linux/build.sh [release|debug]
```

Output: `~/.cache/jackdaw/linux/build{,-debug}/jackdaw`. Third-party sources are fetched into `thirdparty/`.
Needs autoconf/automake/libtool (libetpan), OpenSSL, zlib, libsystemd; libsecret-1 for the keystore.

## Keys

| Where | Keys |
|---|---|
| Anywhere | `Ctrl+Tab` / `Ctrl+1..9` tabs, `Ctrl+W` close tab, `Ctrl+R` sync now, `Ctrl+,` settings, `Ctrl+B` sidebar full/icons, `?` keys (type to find one) |
| Modes | the footer shows NORMAL, VISUAL or INSERT; `v` in the list starts visual selection, `Esc` in compose goes to normal (`j` `k` fields), `i` back to insert |
| Mail | `Tab` / `h` / `l` panes, `j` `k` `gg` `G` move, `gi` `gs` `gd` `ga` `gt` `gj` Inbox, Sent, Drafts, Archive, Trash, Junk, `Enter` open, `Shift+Enter` or middle click open in a tab, `p` reading pane right/bottom/off, right click for a menu |
| Messages | `r` reply, `R` reply all, `F` forward, `Ctrl+N` new, `u` read/unread, `s` flag, `e` archive, `dd` delete (`d` on a visual selection, or `Delete`), `m` move, `c` copy, `z` undo |
| List | `x` / `Shift+j` `Shift+k` / `Ctrl+A` select, `t` conversations/messages, `fu` `ff` `fa` `fs` `fc` filters (unread, flagged, attachments, sender, clear) |
| Search | `/` or `Ctrl+K` search; operators become pills once followed by a space (`in:<folder>` is there from the start: remove it to search every folder), `Backspace` at the start takes the last pill back, the quick filter chips are pills too, `Esc` leave, `Shift+S` also search the server, `Shift+M` make a rule from the search |
| Folders | `Space` collapse, `a` new, `Shift+R` rename, `Shift+D` delete |
| Reader | `j` `k` `Space` scroll, `o` expand all, `Shift+P` plain text / HTML, `v` a caret to move (`v` again starts selecting, `y` copies, `Esc` steps back), drag to select and `Ctrl+C`, `Shift+A` save attachments (several as one .zip), `Ctrl+S` save as .eml, `Ctrl+P` print; a click on an attachment opens it, its download icon saves it |
| Compose | `Ctrl+Enter` send, `Ctrl+S` save draft, `Ctrl+Shift+A` attach (or drop files), `Ctrl+Shift+C` Cc/Bcc |
| Rules | `Ctrl+Shift+L` list rules, `Ctrl+Shift+F` server filters (Sieve) |

Search operators: `from:` `to:` `subject:` `in:` `is:unread` `is:read` `is:flagged` `has:attachment`
`after:DD-MM-YYYY` `before:DD-MM-YYYY`, other words go to the full-text index.

## Settings

Everything is in the Settings tab (`Ctrl+,`) and saved to `~/.config/jackdaw/settings.toml`: `[account]`,
`[view]` (theme, scale, density, snippets, grouping, reading pane right/bottom/off, sidebar full/rail, tabs
top/sidebar, `list_text` and `reader_text` sizes, pane sizes), `[fonts]` (`ui`, `list`, `reader`, `mono` as
fontconfig families, `mono_plain`), `[reading]` (format, mark_read, `remote_images` = ask/always, `images_from` =
trusted senders), `[notifications]` (enabled, when_focused), `[compose]` (`markdown`: the body also goes out as HTML
rendered from Markdown; `attachment_list`: a `[1] name` list of attachments in the body), `[sync]`, `[signature]` (`new`, `reply`) and `[[rule]]` entries (`match` in search syntax, `action` = `read`, `flag`,
`delete`, `move:<folder>`, `copy:<folder>`). The password lives in the system keystore. Mail is stored in
`~/.local/share/jackdaw/<address>/`.

## Test server

`tests/imap/start.sh` runs Dovecot + Postfix in Docker with the mail `tests/imap/make_mail.py` generates
(threads, a mailing list, HTML newsletters with inline images, receipts, attachments, nine folders): user `jackdaw`, password
`jackdaw-test`, IMAPS 9993, STARTTLS 9143, SMTP 9465/9587, ManageSieve 14190, remote test images on http 9080. Start jackdaw with
`SSL_CERT_FILE=~/.cache/jackdaw/imap-test/ca.crt` to trust its certificate.

## Licence

MIT. Inter font: SIL Open Font License (`assets/Inter-LICENSE.txt`). Lucide icons: ISC (`assets/icons/LICENSE`).
