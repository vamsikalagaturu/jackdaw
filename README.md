# jackdaw

A keyboard-first mail client for Linux and Android, written in C11 with raylib. It talks plain IMAP and SMTP with
an app password and keeps your mail offline in SQLite.

- Conversations, a folder tree, multi-select, undo, and right-click menus.
- Search as you type: full text over the local store, then the server. Operators like `from:` and `has:attachment`
  turn into pills.
- HTML mail laid out with litehtml. Remote images are blocked until you allow them, and text can be selected and
  copied.
- Compose in Markdown, sent as HTML. The body editor works like vim, and drafts save themselves.
- Rules on your machine, and Sieve filters on the server.
- Dark and light Nord themes, adjustable scale and fonts. Every action has a key.

## Install

- **Linux (x86_64):** `platform/linux/install.sh` installs the latest release as `~/.local/bin/jackdaw` with a desktop
  entry. Or download `jackdaw-linux-x86_64` from [Releases](https://github.com/vamsikalagaturu/jackdaw/releases).
- **Android 11+:** install `jackdaw.apk` from [Releases](https://github.com/vamsikalagaturu/jackdaw/releases).

## More

- [Keys](docs/keys.md)
- [Settings](docs/settings.md)
- [Development](docs/development.md): build, the test server, releases
- [Android](platform/android/README.md)

## Licence

MIT. Inter font: SIL Open Font License (`assets/Inter-LICENSE.txt`). Lucide icons: ISC (`assets/icons/LICENSE`).
