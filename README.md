# ClipGnome 📋

**Clipboard manager for GNOME** — monitors your clipboard history, stores text, images and file copies, with fast fuzzy search. Built with GTK4, Wayland, C, Make and SQLite.

---

## Features

- **Text history** — stores every text copy with full-text search (FTS5)
- **Image history** — saves PNG snapshots of image copies
- **File copies** — records copied files with optional binary snapshot
- **⭐ Star / pin** important items so they survive history pruning
- **Fast search** — debounced search with SQLite FTS5 prefix matching
- **Keyboard shortcut** — `Super+V` to toggle the window
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

The binary is placed at `build/clipgnome`.

---

## Install system-wide

```bash
sudo make install
# installs to /usr/local/bin/clipgnome
```

### Autostart with GNOME

```bash
mkdir -p ~/.config/autostart
cp data/io.github.clipgnome.desktop ~/.config/autostart/
```

Then add `X-GNOME-Autostart-enabled=true` to that file.

---

## Usage

| Action                         | How                          |
|-------------------------------|-------------------------------|
| Open / close window           | `Super+V` or click app icon  |
| Search history                | Type in the search bar       |
| Copy item back to clipboard   | Click row **or** copy button |
| Star / pin item               | Click ☆ button               |
| Show only starred             | Toggle ⭐ filter             |
| Delete single item            | Click 🗑 button              |
| Clear all history             | Click "🗑 Clear history"     |
| Close window without copying  | `Escape`                     |

---

## Project Structure

```
clipgnome/
├── src/
│   ├── main.c          # Entry point
│   ├── application.c/h # GtkApplication subclass, wires everything
│   ├── clipboard.c/h   # GdkClipboard watcher (GObject, emits "new-item")
│   ├── database.c/h    # SQLite persistence layer (CRUD + FTS5 search)
│   ├── item.c/h        # ClipItem data model
│   └── window.c/h      # GTK4 UI: search bar, list, actions
├── data/
│   ├── io.github.clipgnome.desktop
│   └── io.github.clipgnome.svg
├── Makefile
└── README.md
```

---

## Configuration

Edit `src/application.h`:

```c
#define CLIP_MAX_HISTORY   500          /* items to keep in DB        */
#define CLIP_MAX_FILE_SIZE (10*1024*1024) /* max file blob size        */
```

---

## Database location

```
~/.local/share/clipgnome/clipgnome.db
```

SQLite WAL mode — safe for concurrent access.

---

## Wayland notes

ClipGnome uses the GDK clipboard API which works on **both X11 and Wayland** transparently. On Wayland, clipboard access requires a focused window or an explicit paste operation — this is a security feature of the protocol. The monitor fires on every `changed` signal emitted by the display's clipboard.

---

## License

MIT — do whatever you want.
