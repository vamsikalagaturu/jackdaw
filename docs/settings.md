# Settings

Everything is in the Settings tab (`Ctrl+,`) and saved to `~/.config/jackdaw/settings.toml`. Passwords live in
the system keystore (Secret Service on Linux, Android Keystore on Android), never in that file. Each account's mail is
stored in `~/.local/share/jackdaw/<address>/`, the calendar in `~/.local/share/jackdaw/calendar.db`. Development
builds use `jackdaw-dev` in place of `jackdaw` in these paths.

**Several mail accounts**: the account at the top of the sidebar lists them all with their unread mail, and adds one
(Add an account); `Ctrl+Shift+1`…`9` open one directly. One is open at a time; the others keep syncing in the
background (rules, scheduled sends and snoozed mail included, on Android while jackdaw is closed too), and their new
mail notifies: the notification opens its account. Switching closes the open account's tabs (drafts are saved).

**Log out** (Settings → Account) forgets the open account: its password and its `[[account]]`, and moves its local mail
folder to the trash (deletes it on Android); the next account opens. The calendar, the other settings and the rules
stay.

**Calendar** (Settings → Calendar): accounts are added one at a time (Add account), each a CalDAV server or a Google
account; each shows as a card with its calendars under it. CalDAV takes the server's address, a username and a
password. Google needs a
sign-in in the browser: jackdaw asks Google for access to your calendars (OAuth 2.0 with PKCE, answered on a port of
127.0.0.1), and keeps the sign-in in the keystore. The account's calendars are then listed there, none synced until
ticked; unticking one removes its events from this computer. CalDAV / Google only switches the form; Remove
account, on an account's card, forgets its sign-in and the calendars kept here. Each calendar's brush picks its colour
(presets, a colour square with a hue bar, or a hex value); it stays in jackdaw over the server's, and Reset goes back
to the server's. The same list is behind Calendars in the calendar's header, where calendars are also shown or hidden.

**Vacation reply** (Settings → Server filters): on the server (Sieve vacation, RFC 5230), so it answers while
jackdaw is closed: optional from and until dates (the date extension's `currentdate`, RFC 5260), a subject, the reply,
and once per sender every so many days. It is a block between `# jackdaw vacation` lines in the active script, after
its `require` lines; the rest of the script stays as it is, and turning the reply off takes the block out.

**Shared between devices** (Settings → Sync, `[sync] settings`): the theme, reading, notifications (not push),
compose, signatures, rules, the calendar's default view, the calendar accounts and each calendar's synced / shown /
colour choice travel to the account's other jackdaws, as one message in a folder `jackdaw` on the mail server (under
the personal namespace; jackdaw hides it). Layout, sizes, fonts, push and the sync timers stay per device; passwords
and sign-ins never leave it, so a calendar account shared from another device asks to be added again here to sign
in. Each of those groups carries the time it last changed; when two devices changed the same group before seeing each
other's, the later change wins, and changes to different groups are all kept. A device sharing for the first time,
when another already did, asks whether to use those settings or share its own. What this device last agreed with,
its changes not yet on the server and its device id are in `shared-settings.toml` in the mail folder. Settings → Sync
lists the devices sharing, each by its host name ("Android" on a phone) with when it last wrote the copy; a device
writes itself in when it shares and at least once a day. Forget takes one off the list; it comes back if it shares
again.

| Section | Keys |
|---|---|
| `[[account]]` | one per mail account, the first first (its folder carries the shared settings): `name`, `address`, `username`, `imap_host` / `imap_port` / `imap_security`, `smtp_host` / `smtp_port` / `smtp_security`, `sieve_port` (4190). A single `[account]`, as earlier versions wrote it, is read as the only one |
| `[theme]` | `scheme` = `dark` / `light` / `system` (as light or dark as the desktop theme); colour roles as in the desktop theme's `~/.local/state/omarchy/current/theme/colors.toml`, which jackdaw follows when it is there: `accent`, `background`, `dark_background`, `lighter_background`, `foreground`, `bright_foreground`, `red`, `yellow`, `green`, `blue`, `magenta`, `cyan`, `orange` (`"#rrggbb"`); otherwise Nord. The same as in ferret and cuttle |
| `[view]` | scale, density, snippets, `effects` (true: sent mail folds into a paper plane that circles while Undo waits, deleted mail burns away, dialogs rise over a frosted page; per device), grouping, reading pane right/bottom/off, sidebar full/rail, tabs top/sidebar, `list_text` and `reader_text` sizes, pane sizes, `calendar` = `day` / `week` / `month` / `agenda`: the view the calendar opens in (absent: week, agenda on a phone) |
| `[fonts]` | `ui`, `list`, `reader`, `mono` as fontconfig families, `mono_plain` |
| `[reading]` | format, mark_read, `remote_images` = ask/always, `images_from` = trusted senders |
| `[notifications]` | enabled, when_focused |
| `[compose]` | `markdown`: the body also goes out as HTML rendered from Markdown; `attachment_list`: a `[1] name` list of attachments in the body; `undo_seconds` (10; 0, 5, 20 or 30): a sent message waits this long with Undo before it goes, out of the tabs (Undo brings it back; quitting sends it at once) |
| `[updates]` | `check`: look for a new release once a day and offer it (default true); Settings → About checks by hand |
| `[sync]` | `prefetch_days` (30: days of bodies fetched for offline reading and search; 0 none, -1 all), `prefetch_folders` (`"inbox"`, or `"all"` for every folder, fetched after each sync round), `check_minutes` (5), `settings` (true: shared with the account's other jackdaws, below) |
| `[[calendar]]` | one per calendar account: `provider` = `caldav` / `google`, `url` (CalDAV: the server, a principal or one calendar's address; discovery follows RFC 6764 and RFC 4791), `username` (the CalDAV login, or the Google address), `check_minutes` (15), `reminders` (true: a notification at each event's reminder; on Android also while jackdaw is closed; the same in every entry). Each account's password, or Google sign-in, is in the keystore. A single `[calendar]` section, as earlier versions wrote it, is read as the first account |
| `[signature]` | `new`, `reply` |
| `[[rule]]` | `match` in search syntax, `action` = `read`, `flag`, `delete`, `move:<folder>`, `copy:<folder>` |
