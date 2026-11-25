# Changelog

What changed in each release, newest first, in the form of [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/).
`platform/release.sh` publishes a version's section as its GitHub release notes, and stops when the version has
none: add it with the change. Releases before 0.3.4 have their notes on GitHub.

## [0.3.5] - 2026-11-25

### Added

- While `Ctrl` + wheel magnifies, a corner box shows the window in small with the magnified part marked; it moves to
  the other corner when the pointer is over it.

### Fixed

- The magnified view no longer shakes with the pointer still.
- Magnified text is sharp all the way through the zoom, without a jump into focus once it stops, and no letter moves
  as it zooms.
- The first magnification no longer stalls, and magnifying no longer keeps memory once it ends.

## [0.3.4] - 2026-11-24

### Added

- A search bar over every page (over the mail list on Android), opened with `/` or `Ctrl+K` from anywhere. As you
  type it lists mail, folders, settings, calendar events, people and actions; the arrows pick one and `Enter` opens
  it. With none picked, `Enter` searches the mail into the list as before. The search words are marked in the
  list's senders and subjects.
- Animations and effects, on by default and kept per device (Settings > Appearance, `[view] effects`):
  - a sent message folds into a paper plane that circles while Undo waits, then flies off; Undo brings it back
  - Send later closes the message into an envelope with a clock that flies to Drafts
  - a message deleted to the Trash burns away like paper, and the rows below then close up
  - dialogs, the keys help and the phone's folder drawer blur the page behind them; dialogs rise into place
  - new mail slides in at the top and glows, unread counts roll, a flag pops with a spark
  - an empty Inbox shows drifting clouds and a gliding plane; the phone's pull to refresh is a slingshot
  - pages slide and fade in, and list rows lift under the pointer
- Links in plain-text mail, bare or in `<...>`, are links.
- Message zoom for the session: out, in and 100% in the reader's toolbar, and `Ctrl+-` / `Ctrl+=` / `Ctrl+0`.
- `Ctrl` + wheel magnifies around the pointer until `Ctrl` is let go.
- Not junk, in the reader's toolbar and the list's menu in a Junk folder: back to the Inbox, where the rules run on
  the message.
- Rules can be edited in place (Settings > Rules, the pencil).

### Changed

- `Ctrl`+click opens a link (a tap on Android); a plain click selects text, as on any text.
- Buttons are raised, with a soft shadow and a lit edge; they rise on hover and ripple on a press. Switches slide.
- Dialogs are centred in the window, with a shadow.
- Move to shows the folders as a tree with the sidebar's arrows, by their readable names, and scrolls.
- Folders are in tree order everywhere: a special folder's subfolders (those of Archives) stay under it.
- Calendar in the sidebar is a full-width button.
- On Android, keyboard shortcuts no longer show in hints, placeholders or tooltips, and Keys is not in the sidebar.

### Fixed

- A key pressed and released within one frame counts: Esc and other quick taps needed several presses on some
  keyboards.
- Esc closes the keys help at once, and Keys in the sidebar opens it.
- Archive works on servers that mark no folder as the archive, when one is named Archives.
- On Android, a tap outside a dialog cancels it, and prompts have OK and Cancel.
- A tap on an Undo toast no longer opens the message under it.
- The Move to list showed only its first folders.
- A list row under the pointer at its edge no longer flickers.
