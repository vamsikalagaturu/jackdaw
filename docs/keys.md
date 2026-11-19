# Keys

`?` in the app shows these keys; start typing to find one. On Android a tap works like a click and a long press
opens the menu; the vim-style modes are left out. In a message a long press on text selects the word: drag its
handles to change the selection, and the bar above it copies, selects all, shares or (⋮) hands it to apps such as
Translate.

| Where | Keys |
|---|---|
| Anywhere | `Ctrl+Tab` / `Ctrl+1..9` tabs, `Ctrl+W` close tab, `Ctrl+R` sync now, `Ctrl+,` settings, `Ctrl+B` sidebar full/icons, `?` keys (type to find one) |
| Modes | the footer shows NORMAL, VISUAL or INSERT; `v` in the list starts visual selection, `Esc` in compose goes to normal (`j` `k` fields), `i` back to insert |
| Mail | `Tab` / `h` / `l` panes, `j` `k` `gg` `G` move, `gi` `gs` `gd` `ga` `gt` `gj` Inbox, Sent, Drafts, Archive, Trash, Junk, `gc` calendar, `Enter` open, `Shift+Enter` or middle click open in a tab, `p` reading pane right/bottom/off, right click for a menu |
| Messages | `r` reply, `R` reply all, `F` forward, `Ctrl+N` new, `u` read/unread, `s` flag, `e` archive, `dd` delete (`d` on a visual selection, or `Delete`), `m` move, `c` copy, `z` undo |
| List | `x` / `Shift+j` `Shift+k` / `Ctrl+A` select, `t` conversations/messages, `fu` `ff` `fa` `fs` `fc` filters (unread, flagged, attachments, sender, clear) |
| Search | `/` or `Ctrl+K` search; operators become pills once followed by a space (`in:<folder>` is there from the start: remove it to search every folder), `Backspace` at the start takes the last pill back, the quick filter chips are pills too, `Esc` leave, `Shift+S` also search the server, `Shift+M` make a rule from the search |
| Folders | `Space` collapse, `a` new, `Shift+R` rename, `Shift+D` delete |
| Reader | `j` `k` `Space` scroll, `o` expand all, `Shift+P` plain text / HTML, `Shift+L` light / dark mail page (follows the theme until flipped), `v` a caret to move (`v` again starts selecting, `y` copies, `Esc` steps back), drag to select and `Ctrl+C`, `Shift+A` save attachments (several as one .zip), `Ctrl+S` save as .eml, `Ctrl+P` print; a click on an attachment opens it, its download icon saves it; each open message has its own reply, reply all, forward, read, flag and delete icons and a ⋮ menu (archive, move, copy, open in a tab, save, print) |
| Compose | `Ctrl+Enter` send, `Ctrl+S` save draft, `Ctrl+Shift+A` attach (or drop files), `Ctrl+Shift+C` Cc/Bcc, `Ctrl+Shift+Q` quote an HTML original as `> ` text (the default, to answer line by line) or as its own HTML (with its images); Discard throws the message and its saved draft away |
| Rules | `Ctrl+Shift+L` list rules, `Ctrl+Shift+F` server filters (Sieve) |
| Calendar | `gc` (or Calendar in the sidebar) opens it; `1` `2` `3` `4` day, week, month, agenda (`w` `m` `a` too); Calendars in its header shows or hides each synced calendar; `h` `l` the day before or after (agenda: a week), `Shift+h` `Shift+l` the period before or after, `t` today; `j` `k` next or previous event, `Enter` its details (again: edit); `n` new event, `e` edit, `d` delete, `r` sync now; a double click on an empty hour (week) or day (month) starts an event there, on an event edits it. In an event: `Tab` next field, `Ctrl+S` save, `Esc` cancel |
| Invitations | a message carrying an invitation shows it above the text with Accept, Maybe and Decline; an answer goes to the organizer and the event into the calendar |

## Search operators

`from:` `to:` `subject:` `in:` `is:unread` `is:read` `is:flagged` `has:attachment` `after:DD-MM-YYYY`
`before:DD-MM-YYYY`; other words go to the full-text index. Terms are all required; `OR` (in capitals) between two
terms takes either, and binds before the rest: `in:INBOX subject:invoice OR subject:rechnung` is the Inbox's
messages with either subject. Rules use the same syntax.
