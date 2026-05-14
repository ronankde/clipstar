# ────────────────────────────────────────────────────────────────
#  ClipGnome & ClipStar Daemon - Unified Makefile
# ────────────────────────────────────────────────────────────────

APP_UI     := clipgnome
APP_DAEMON := clipstar-daemon
APP_ID     := io.github.clipgnome
VERSION    := 1.0.0

# Base Directories
SRCDIR     := src
BUILDDIR   := build
DATADIR    := data

# ── Installation Directories (Local User) ────────────────────────
PREFIX     := $(HOME)/.local
BINDIR     := $(PREFIX)/bin
DESKTOPDIR := $(PREFIX)/share/applications
ICONDIR    := $(PREFIX)/share/icons/hicolor/scalable/apps
UNITDIR    := $(HOME)/.config/systemd/user

# ── Global Compilation Settings ──────────────────────────────────
CC         := gcc
# Use gnu11 to ensure compatibility with _POSIX_C_SOURCE in the daemon
COMMON_CFLAGS := -Wall -Wextra -Wpedantic -std=gnu11 -DAPP_VERSION=\"$(VERSION)\"

ifdef RELEASE
  COMMON_CFLAGS += -O2 -DNDEBUG
else
  COMMON_CFLAGS += -g -O0 -DDEBUG
endif

# ── Frontend Dependencies & Flags (GTK4 UI) ──────────────────────
PKG_UI     := gtk4 sqlite3
CFLAGS_UI  := $(COMMON_CFLAGS) $(shell pkg-config --cflags $(PKG_UI))
LDFLAGS_UI := $(shell pkg-config --libs $(PKG_UI))

# ── Backend Dependencies & Flags (X11 Daemon) ────────────────────
PKG_DAEMON := x11 sqlite3
CFLAGS_DAEMON  := $(COMMON_CFLAGS) $(shell pkg-config --cflags $(PKG_DAEMON))
LDFLAGS_DAEMON := $(shell pkg-config --libs $(PKG_DAEMON))

# Check for XFixes support (event-driven clipboard detection)
HAVE_XFIXES := $(shell pkg-config --exists xfixes && echo yes || echo no)
ifeq ($(HAVE_XFIXES),yes)
  CFLAGS_DAEMON  += $(shell pkg-config --cflags xfixes) -DHAVE_XFIXES
  LDFLAGS_DAEMON += $(shell pkg-config --libs   xfixes)
  $(info [INFO] XFixes detected — using event-driven clipboard watching)
else
  $(info [INFO] XFixes not found — using polling fallback (250ms))
endif

# ── Source File Separation ───────────────────────────────────────
# Isolate the daemon source to avoid conflicts with the UI code
DAEMON_SRC := $(SRCDIR)/clipstar-daemon.c
# Get all .c files in src, EXCEPT the daemon
UI_SRCS    := $(filter-out $(DAEMON_SRC), $(wildcard $(SRCDIR)/*.c))
# Generate the corresponding object file list for the UI
UI_OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(UI_SRCS))

# ── Build Rules ──────────────────────────────────────────────────

.PHONY: all clean install uninstall check-deps run release

all: check-deps $(BUILDDIR)/$(APP_UI) $(BUILDDIR)/$(APP_DAEMON)

# Build the Frontend (ClipGnome)
$(BUILDDIR)/$(APP_UI): $(UI_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS_UI)
	@echo "✓ Build successful: $@"

# Compile Frontend object files (.c -> .o)
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS_UI) -c -o $@ $<

# Build the Backend (Daemon) in a single step
$(BUILDDIR)/$(APP_DAEMON): $(DAEMON_SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS_DAEMON) -o $@ $< $(LDFLAGS_DAEMON)
	@echo "✓ Build successful: $@"

# Create build directory
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Utility Targets ──────────────────────────────────────────────

run: all
	./$(BUILDDIR)/$(APP_UI)

release:
	$(MAKE) RELEASE=1

clean:
	rm -rf $(BUILDDIR)

# ── Installation & Uninstallation ────────────────────────────────

install: all
	@echo "Creating local directories..."
	mkdir -p $(BINDIR) $(DESKTOPDIR) $(ICONDIR) $(UNITDIR)
	
	@echo "Installing binaries..."
	install -Dm755 $(BUILDDIR)/$(APP_UI) $(BINDIR)/$(APP_UI)
	install -Dm755 $(BUILDDIR)/$(APP_DAEMON) $(BINDIR)/$(APP_DAEMON)
	
	@echo "Installing Desktop files (Shortcut & Icon)..."
	install -Dm644 $(DATADIR)/$(APP_ID).desktop $(DESKTOPDIR)/$(APP_ID).desktop
	install -Dm644 $(DATADIR)/$(APP_ID).svg $(ICONDIR)/$(APP_ID).svg
	
	@echo "Installing and enabling Systemd User Service..."
	install -Dm644 $(DATADIR)/clipstar-daemon.service $(UNITDIR)/clipstar-daemon.service
	systemctl --user daemon-reload
	systemctl --user enable --now clipstar-daemon
	
	@echo "✓ Installation completed successfully in ~/.local"

uninstall:
	@echo "Stopping and removing Systemd service..."
	systemctl --user disable --now clipstar-daemon 2>/dev/null || true
	rm -f $(UNITDIR)/clipstar-daemon.service
	systemctl --user daemon-reload
	
	@echo "Removing binaries and desktop files..."
	rm -f $(BINDIR)/$(APP_UI)
	rm -f $(BINDIR)/$(APP_DAEMON)
	rm -f $(DESKTOPDIR)/$(APP_ID).desktop
	rm -f $(ICONDIR)/$(APP_ID).svg
	
	@echo "✓ Uninstallation completed."

# ── Dependency Check ─────────────────────────────────────────────

check-deps:
	@pkg-config --exists $(PKG_UI) $(PKG_DAEMON) || \
	  (echo "ERROR: Missing dependencies. Install them using:"; \
	   echo "  sudo apt install libgtk-4-dev libsqlite3-dev libx11-dev libxfixes-dev pkg-config"; \
	   exit 1)
