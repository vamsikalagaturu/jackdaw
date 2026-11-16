# Settings

Everything is in the Settings tab (`Ctrl+,`) and saved to `~/.config/jackdaw/settings.toml`. The password lives in
the system keystore (Secret Service on Linux, Android Keystore on Android), never in that file. Mail is stored in
`~/.local/share/jackdaw/<address>/`. Development builds use `jackdaw-dev` in place of `jackdaw` in both paths.

**Log out** (Settings → Account) forgets the password and the `[account]` section and moves the local mail folder to
the trash (deletes it on Android); the other settings and the rules stay.

| Section | Keys |
|---|---|
| `[account]` | `name`, `address`, `username`, `imap_host` / `imap_port` / `imap_security`, `smtp_host` / `smtp_port` / `smtp_security`, `sieve_port` (4190) |
| `[theme]` | `scheme` = `dark` / `light` / `system` (as light or dark as the desktop theme); colour roles as in the desktop theme's `~/.local/state/omarchy/current/theme/colors.toml`, which jackdaw follows when it is there: `accent`, `background`, `dark_background`, `lighter_background`, `foreground`, `bright_foreground`, `red`, `yellow`, `green`, `blue`, `magenta`, `cyan`, `orange` (`"#rrggbb"`); otherwise Nord. The same as in ferret and cuttle |
| `[view]` | scale, density, snippets, grouping, reading pane right/bottom/off, sidebar full/rail, tabs top/sidebar, `list_text` and `reader_text` sizes, pane sizes |
| `[fonts]` | `ui`, `list`, `reader`, `mono` as fontconfig families, `mono_plain` |
| `[reading]` | format, mark_read, `remote_images` = ask/always, `images_from` = trusted senders |
| `[notifications]` | enabled, when_focused |
| `[compose]` | `markdown`: the body also goes out as HTML rendered from Markdown; `attachment_list`: a `[1] name` list of attachments in the body |
| `[updates]` | `check`: look for a new release once a day and offer it (default true); Settings → About checks by hand |
| `[sync]` | `prefetch_days` (days of Inbox bodies fetched for offline reading), `check_minutes` (5) |
| `[signature]` | `new`, `reply` |
| `[[rule]]` | `match` in search syntax, `action` = `read`, `flag`, `delete`, `move:<folder>`, `copy:<folder>` |
