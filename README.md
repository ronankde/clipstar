# ClipStar 📋

**Clipboard manager for GNOME** — monitors your clipboard history, stores text, images and file copies, with fast fuzzy search. Built with GTK4, Wayland, C, Make and SQLite.
It's designed to be basic, fulfilling the functions we use most in a clipboard manager; no thumbnails, no syntax highlighting, which are basically useless.

---

## Features

- **Text history** — stores every text copy with full-text search (FTS5)
- **File copies** — records copied files with optional binary snapshot
- **Fast search** — debounced search with SQLite FTS5 prefix matching
- **ESC** to hide; click any item to restore it to the clipboard
- Keeps the last **500** entries (configurable in `application.h`)
- Lightweight: pure C, no Electron, no Python

---

## Dependencies

| Package            | Ubuntu/Debian                 | Fedora               |
|--------------------|-------------------------------|----------------------|
| GTK4 dev headers   | `libgtk-4-dev`                | `gtk4-devel`         |
| SQLite3 dev headers| `libsqlite3-dev`              | `sqlite-devel`       |
| pkg-config         | `pkg-config`                  | `pkgconf`            |
| GCC + Make         | `build-essential`             | `gcc make`           |

### Install on Ubuntu/Debian

```bash
sudo apt install libgtk-4-dev libsqlite3-dev pkg-config build-essential
```

### Install on Fedora

```bash
sudo dnf install gtk4-devel sqlite-devel pkgconf gcc make
```

---

## Build

```bash
# Debug build (default)
make

# Release build (optimised)
make release

# Run directly
make run
```

The binary is placed at `build/clipstar` && 'build/clipstart-daemon.

---

## Local install (recommended)

```bash
make install
# installs to $HOME/.local/bin/clipstar $HOME/.local/bin/clipstar-daemon
```

### Start daemon 

```bash
(no sudo)
systemctl daemon-reload --user
systemctl start --user clipstar-daemon
configure your gnome shorcuts to open clipstar app ex.(Shift+Super+V)
```
---

## Usage

| Action                         | How                               |
|-------------------------------|------------------------------------|
| Open / close window           | `Shift+Super+V` or click app icon  |
| Search history                | Type in the search bar             |
| Copy item back to clipboard   | Click row **or** copy button       |
| Browse through the items      | Up/Down, Enter & Delete            |
| Delete single item            | Click 🗑 button                    |
| Clear all history             | Click "🗑"                         |
| Close window without copying  | `Escape`                           |

---

## Project Structure

```
clipgnome/
├── src/
│   ├── main.c            # Entry point
│   ├── application.c/h   # GtkApplication subclass, wires everything
│   ├── clipboard.c/h     # Database watcher
│   ├── clipstar-daemon.c # Clipboard watcher daemon
│   ├── database.c/h      # SQLite persistence layer (CRUD + FTS5 search)
│   ├── item.c/h          # ClipItem data model
│   └── window.c/h        # GTK4 UI: search bar, list, actions
├── data/
│   ├── clipstart-daemon.service
│   ├── clipgnome.desktop
│   └── clipgnome.svg
├── Makefile
└── README.md
```

---

## Configuration

It has no settings; it's a simple app just for saving and accessing your clipboards.
```

---

## Database location

```
~/.local/share/clipstar/clipgnome.db
~/.local/share/clipstar/clipgnome.db-shm
~/.local/share/clipstar/clipgnome.db-wal

```

SQLite WAL mode — safe for concurrent access.

---

## Wayland notes

ClipStar is an app designed for Gnome with Wayland. It hasn't been tested on other desktop environments. It's also worth mentioning that the daemon depends on XWayland to function, since Gnome doesn't implement wlr-data-control, making it impossible to use wl-paste. However, only the background daemon depends on XWayland; the frontend is designed for Wayland and works very well. It simply manages the database created and populated by the clipstar-daemon.
Not to mention that it performs better than most clipboard managers.

---

## License

MIT — do whatever you want.
