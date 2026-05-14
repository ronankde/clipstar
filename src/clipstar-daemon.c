/*
 * clipstar-daemon.c
 *
 * Daemon that watches the X11 CLIPBOARD selection (via Xlib / XWayland)
 * and persists every change into the ClipStar SQLite database.
 *
 * Build:
 *   cc -O2 -o clipstar-daemon clipstar-daemon.c $(pkg-config --libs --cflags x11 sqlite3) -lpthread
 *
 * Run:
 *   ./clipstar-daemon &
 *   # or via systemd user unit (see clipstar-daemon.service)
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>

/* ── tunables ────────────────────────────────────────────────── */

#define CS_MAX_HISTORY  5000
#define CS_MAX_TEXT_LEN (8 * 1024 * 1024)   /* 8 MB text limit        */
#define CS_MAX_IMG_LEN  (32 * 1024 * 1024)  /* 32 MB image limit      */
#define CS_POLL_MS      250                  /* event loop sleep (ms)  */
#define CS_LOCK_FILE    "/tmp/clipstar-daemon.lock"

/* ── item types (must match ClipStar schema) ─────────────────── */
#define CS_TYPE_TEXT    0
#define CS_TYPE_IMAGE   1
#define CS_TYPE_FILE    2
#define CS_TYPE_FOLDER  3

/* ── globals ─────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;
static sqlite3              *g_db      = NULL;
static Display              *g_dpy     = NULL;
static Window                g_win     = None;
static char                  g_db_path[1024] = {0};

/* X11 atoms we need */
static Atom CS_ATOM_CLIPBOARD;
static Atom CS_ATOM_TARGETS;
static Atom CS_ATOM_INCR;
static Atom CS_ATOM_UTF8_STRING;

static Atom CS_ATOM_TEXT;
static Atom CS_ATOM_TEXT_PLAIN;
static Atom CS_ATOM_TEXT_PLAIN_UTF8;
static Atom CS_ATOM_URI_LIST;
static Atom CS_ATOM_GNOME_FILES;
static Atom CS_ATOM_PNG;
static Atom CS_ATOM_JPEG;
static Atom CS_ATOM_BMP;
static Atom CS_ATOM_PROPERTY;    /* scratch property on our window */

/* ── signal handler ──────────────────────────────────────────── */

static void cs_sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── logging ─────────────────────────────────────────────────── */

#define LOG(fmt, ...) \
    do { \
        time_t _t = time(NULL); \
        struct tm _tm; \
        localtime_r(&_t, &_tm); \
        fprintf(stderr, "[%02d:%02d:%02d] clipstar-daemon: " fmt "\n", \
                _tm.tm_hour, _tm.tm_min, _tm.tm_sec, ##__VA_ARGS__); \
    } while(0)

/* ── single-instance lock ────────────────────────────────────── */

static int cs_acquire_lock(void) {
    int fd = open(CS_LOCK_FILE, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;

    struct flock fl = {
        .l_type   = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start  = 0,
        .l_len    = 0,
    };
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return -1;   /* already running */
    }

    /* write our PID */
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\n", (int)getpid());
    if (write(fd, pid_str, strlen(pid_str)) < 0) { /* ignore */ }
    return fd;  /* keep fd open to hold the lock */
}

/* ── path resolution ─────────────────────────────────────────── */
static int cs_mkdir_p(const char *path) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return -1;
    return 0;
}

static void cs_resolve_db_path(void) {
    const char *xdg_data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    /* 1. Try XDG_DATA_HOME */
    if (xdg_data && xdg_data[0] != '\0') {
        snprintf(g_db_path, sizeof(g_db_path), "%s/clipgnome/clipgnome.db", xdg_data);
        return;
    }

    /* 2. Try $HOME environment variable or getpwuid fallback */
    if (!home || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }

    if (home) {
        snprintf(g_db_path, sizeof(g_db_path), "%s/.local/share/clipgnome/clipgnome.db", home);
    } else {
        /* 3. Ultimate fallback if user has no home directory */
        snprintf(g_db_path, sizeof(g_db_path), "/tmp/clipgnome.db");
    }
}

/* ── database ────────────────────────────────────────────────── */

static int cs_db_open(void) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", g_db_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (cs_mkdir_p(dir) < 0) {
            LOG("mkdir_p %s: %s", dir, strerror(errno));
            return 0;
        }
    }

    if (sqlite3_open(g_db_path, &g_db) != SQLITE_OK) {
        LOG("sqlite3_open: %s", sqlite3_errmsg(g_db));
        return 0;
    }

    /* performance */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",     NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;",   NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA temp_store=MEMORY;",    NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA mmap_size=30000000;",   NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;",      NULL, NULL, NULL);

    /* create tables if they don't exist yet */
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS clips ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  type      INTEGER NOT NULL DEFAULT 0,"
        "  text      TEXT,"
        "  blob      BLOB,"
        "  mime_type TEXT,"
        "  preview   TEXT,"
        "  timestamp INTEGER NOT NULL,"
        "  starred   INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_clips_timestamp ON clips(timestamp DESC);"
        "CREATE INDEX IF NOT EXISTS idx_clips_starred   ON clips(starred);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS clips_fts USING fts5("
        "  preview, text, content='clips', content_rowid='id',"
        "  tokenize='porter unicode61', detail=none"
        ");"
        "CREATE TRIGGER IF NOT EXISTS clips_ai AFTER INSERT ON clips BEGIN"
        "  INSERT INTO clips_fts(rowid, preview, text)"
        "    VALUES (new.id, new.preview, new.text);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS clips_ad AFTER DELETE ON clips BEGIN"
        "  INSERT INTO clips_fts(clips_fts, rowid, preview, text)"
        "    VALUES ('delete', old.id, old.preview, old.text);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS clips_au AFTER UPDATE ON clips BEGIN"
        "  INSERT INTO clips_fts(clips_fts, rowid, preview, text)"
        "    VALUES ('delete', old.id, old.preview, old.text);"
        "  INSERT INTO clips_fts(rowid, preview, text)"
        "    VALUES (new.id, new.preview, new.text);"
        "END;";

    char *err = NULL;
    if (sqlite3_exec(g_db, ddl, NULL, NULL, &err) != SQLITE_OK) {
        LOG("DDL error: %s", err);
        sqlite3_free(err);
        return 0;
    }

    LOG("database opened: %s", g_db_path);
    return 1;
}

/*
 * Percent-decode a URI path segment into `out` (size `out_size`).
 * Also strips a trailing \r if present (text/uri-list uses CRLF).
 */
static void cs_percent_decode(const char *src, char *out, size_t out_size) {
    char *dst = out;
    const char *end = out + out_size - 1;
    while (*src && dst < end) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    /* strip trailing \r left by CRLF line endings */
    if (dst > out && *(dst - 1) == '\r')
        *(dst - 1) = '\0';
}

/* build a short preview string, caller must free */
static char *cs_make_preview(int type, const char *text,
                             const void *blob, size_t blob_len) {
    (void)blob;
    char buf[256];

    switch (type) {
        case CS_TYPE_TEXT: {
            if (!text || !text[0]) return strdup("(empty)");
            /* copy up to 120 chars, collapse whitespace */
            char tmp[128] = {0};
            size_t j = 0;
            for (size_t i = 0; text[i] && j < 120; i++) {
                unsigned char c = (unsigned char)text[i];
                tmp[j++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (char)c;
            }
            int truncated = text[120] != '\0';
            snprintf(buf, sizeof(buf), "%s%s", tmp, truncated ? "…" : "");
            return strdup(buf);
        }
        case CS_TYPE_IMAGE: {
            if (blob_len < 1024)
                snprintf(buf, sizeof(buf), "🖼  %zu B", blob_len);
            else if (blob_len < 1024 * 1024)
                snprintf(buf, sizeof(buf), "🖼  %.1f KB", blob_len / 1024.0);
            else
                snprintf(buf, sizeof(buf), "🖼  %.1f MB",
                         blob_len / (1024.0 * 1024.0));
            return strdup(buf);
        }
        case CS_TYPE_FILE:
        case CS_TYPE_FOLDER: {
            const char *icon = (type == CS_TYPE_FOLDER) ? "📁" : "📄";
            if (!text) return strdup(icon);

            /*
             * x-special/gnome-copied-files starts with an action line
             * ("copy" or "cut") that is not a URI — skip it.
             */
            const char *scan = text;
            if (strncmp(scan, "copy\n", 5) == 0 || strncmp(scan, "cut\n", 4) == 0) {
                scan = strchr(scan, '\n');
                if (scan) scan++;
                else      scan = text;
            }
            if (!scan || !*scan) return strdup(icon);

            /* Count non-empty lines (= URI entries) */
            int count = 0;
            const char *p = scan;
            while (p && *p) {
                const char *nl = strchr(p, '\n');
                size_t ll = nl ? (size_t)(nl - p) : strlen(p);
                if (ll > 0) count++;
                p = nl ? nl + 1 : NULL;
            }

            /* First URI → extract basename */
            const char *nl1 = strchr(scan, '\n');
            size_t first_len = nl1 ? (size_t)(nl1 - scan) : strlen(scan);
            char first[512];
            snprintf(first, sizeof(first), "%.*s", (int)first_len, scan);

            /* Percent-decode the first URI, then extract basename */
            char decoded_first[512];
            const char *uri_src = (strncmp(first, "file://", 7) == 0) ? first + 7 : first;
            cs_percent_decode(uri_src, decoded_first, sizeof(decoded_first));
            const char *bptr = strrchr(decoded_first, '/');
            const char *base = (bptr && *(bptr + 1)) ? bptr + 1 : decoded_first;

            char basename[64];
            strncpy(basename, base, sizeof(basename) - 1);
            basename[sizeof(basename) - 1] = '\0';

            if (count > 1)
                snprintf(buf, sizeof(buf), "%s %s (+%d)", icon, basename, count - 1);
            else
                snprintf(buf, sizeof(buf), "%s %s", icon, basename);
            return strdup(buf);
        }
    }
    return strdup("(unknown)");
}

/* check if most recent clip text matches, to skip duplicates (for any type) */
static int cs_is_duplicate_text(const char *text) {
    if (!text || text[0] == '\0') return 0;

    sqlite3_stmt *stmt;
    /* Removido o 'WHERE type=0'. Agora verifica a última coisa copiada, seja o que for */
    if (sqlite3_prepare_v2(g_db,
            "SELECT text FROM clips ORDER BY timestamp DESC LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    int dup = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *last = (const char *)sqlite3_column_text(stmt, 0);
        if (last && strcmp(last, text) == 0)
            dup = 1;
    }
    sqlite3_finalize(stmt);
    return dup;
}

static void cs_db_insert(int type,
                         const char *text,
                         const void *blob, size_t blob_len,
                         const char *mime_type) {
    /* Ignora texto vazio */
    if (type == CS_TYPE_TEXT && (!text || !text[0])) return;
    
    /* Bloqueia qualquer duplicata (texto, arquivo ou pasta) que tenha o mesmo conteúdo */
    if (text && text[0] != '\0' && cs_is_duplicate_text(text)) return;

    int64_t now = (int64_t)time(NULL);
    char *preview = cs_make_preview(type, text, blob, blob_len);

    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO clips(type,text,blob,mime_type,preview,timestamp,starred)"
        " VALUES(?,?,?,?,?,?,0);";

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG("prepare insert: %s", sqlite3_errmsg(g_db));
        free(preview);
        return;
    }

    sqlite3_bind_int(stmt, 1, type);
    if (text) sqlite3_bind_text(stmt, 2, text, -1, SQLITE_STATIC);
    else      sqlite3_bind_null(stmt, 2);

    if (blob && blob_len > 0) sqlite3_bind_blob(stmt, 3, blob, (int)blob_len, SQLITE_STATIC);
    else                      sqlite3_bind_null(stmt, 3);

    if (mime_type) sqlite3_bind_text(stmt, 4, mime_type, -1, SQLITE_STATIC);
    else           sqlite3_bind_null(stmt, 4);

    sqlite3_bind_text(stmt, 5, preview, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, now);

    if (sqlite3_step(stmt) != SQLITE_DONE) LOG("insert error: %s", sqlite3_errmsg(g_db));
    else                                   LOG("saved: %s", preview);

    sqlite3_finalize(stmt);
    free(preview);
}

static void cs_db_prune(void) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(g_db,
        "DELETE FROM clips WHERE starred=0 AND id NOT IN ("
        "  SELECT id FROM clips WHERE starred=0"
        "  ORDER BY timestamp DESC LIMIT ?"
        ");",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, CS_MAX_HISTORY);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* ── X11 helpers ─────────────────────────────────────────────── */

static void cs_intern_atoms(void) {
    CS_ATOM_CLIPBOARD       = XInternAtom(g_dpy, "CLIPBOARD",         False);
    CS_ATOM_TARGETS         = XInternAtom(g_dpy, "TARGETS",           False);
    CS_ATOM_INCR            = XInternAtom(g_dpy, "INCR",              False);
    CS_ATOM_UTF8_STRING     = XInternAtom(g_dpy, "UTF8_STRING",       False);
    CS_ATOM_TEXT            = XInternAtom(g_dpy, "TEXT",              False);
    CS_ATOM_TEXT_PLAIN      = XInternAtom(g_dpy, "text/plain",        False);
    CS_ATOM_TEXT_PLAIN_UTF8 = XInternAtom(g_dpy, "text/plain;charset=utf-8", False);
    CS_ATOM_URI_LIST        = XInternAtom(g_dpy, "text/uri-list",     False);
    CS_ATOM_GNOME_FILES     = XInternAtom(g_dpy, "x-special/gnome-copied-files", False);
    CS_ATOM_PNG             = XInternAtom(g_dpy, "image/png",         False);
    CS_ATOM_JPEG            = XInternAtom(g_dpy, "image/jpeg",        False);
    CS_ATOM_BMP             = XInternAtom(g_dpy, "image/bmp",         False);
    CS_ATOM_PROPERTY        = XInternAtom(g_dpy, "CLIPSTAR_PROP",     False);
}

/*
 * Request a selection conversion and wait for SelectionNotify.
 * Returns malloc'd buffer + sets *out_len.  NULL on failure.
 * Handles INCR protocol for large payloads.
 */
static unsigned char *cs_request_target(Atom target, size_t *out_len) {
    *out_len = 0;

    /* delete old property first */
    XDeleteProperty(g_dpy, g_win, CS_ATOM_PROPERTY);
    XFlush(g_dpy);

    XConvertSelection(g_dpy, CS_ATOM_CLIPBOARD, target, CS_ATOM_PROPERTY,
                      g_win, CurrentTime);
    XFlush(g_dpy);

    /* wait up to 2 s for SelectionNotify */
    XEvent ev;
    int waited = 0;
    while (waited < 2000) {
        if (XCheckTypedWindowEvent(g_dpy, g_win, SelectionNotify, &ev))
            break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
        nanosleep(&ts, NULL);
        waited += 10;
    }
    if (waited >= 2000) return NULL;

    XSelectionEvent *se = &ev.xselection;
    if (se->property == None) return NULL;   /* conversion refused */

    /* read the property */
    Atom actual_type;
    int  actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    XGetWindowProperty(g_dpy, g_win, CS_ATOM_PROPERTY,
                       0, 0, False, AnyPropertyType,
                       &actual_type, &actual_format,
                       &nitems, &bytes_after, &prop);
    if (prop) { XFree(prop); prop = NULL; }

    /* INCR protocol: large transfer via incremental chunks */
    if (actual_type == CS_ATOM_INCR) {
        XDeleteProperty(g_dpy, g_win, CS_ATOM_PROPERTY);
        XFlush(g_dpy);

        unsigned char *buf = NULL;
        size_t total = 0;

        while (1) {
            /* wait for PropertyNotify (new chunk available) */
            int found = 0;
            int w2 = 0;
            while (w2 < 5000) {
                if (XCheckTypedWindowEvent(g_dpy, g_win, PropertyNotify, &ev)) {
                    if (ev.xproperty.atom == CS_ATOM_PROPERTY &&
                        ev.xproperty.state == PropertyNewValue) {
                        found = 1;
                        break;
                    }
                }
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
                nanosleep(&ts, NULL);
                w2 += 10;
            }
            if (!found) break;

            /* read chunk */
            XGetWindowProperty(g_dpy, g_win, CS_ATOM_PROPERTY,
                               0, 0x7FFFFFFF, True, AnyPropertyType,
                               &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop);

            if (!nitems || !prop) {
                if (prop) XFree(prop);
                break;  /* empty property = end of INCR */
            }

            size_t chunk_bytes = (size_t)nitems * (size_t)(actual_format / 8);

            /* size guard */
            if (total + chunk_bytes > CS_MAX_IMG_LEN) {
                XFree(prop);
                free(buf);
                return NULL;
            }

            buf = realloc(buf, total + chunk_bytes);
            memcpy(buf + total, prop, chunk_bytes);
            total += chunk_bytes;
            XFree(prop);
        }

        *out_len = total;
        return buf;
    }

    /* normal (non-INCR) */
    XGetWindowProperty(g_dpy, g_win, CS_ATOM_PROPERTY,
                       0, 0x7FFFFFFF, True, AnyPropertyType,
                       &actual_type, &actual_format,
                       &nitems, &bytes_after, &prop);

    if (!prop || !nitems) {
        if (prop) XFree(prop);
        return NULL;
    }

    size_t nbytes = (size_t)nitems * (size_t)(actual_format / 8);
    unsigned char *out = malloc(nbytes + 1);
    memcpy(out, prop, nbytes);
    out[nbytes] = '\0';
    XFree(prop);

    *out_len = nbytes;
    return out;
}

/*
 * Get the list of supported TARGETS for the current clipboard owner.
 * Returns malloc'd array of Atom, sets *n_targets.
 */
static Atom *cs_get_targets(int *n_targets) {
    *n_targets = 0;

    size_t len = 0;
    unsigned char *data = cs_request_target(CS_ATOM_TARGETS, &len);
    if (!data || len == 0) {
        free(data);
        return NULL;
    }

    int n = (int)(len / sizeof(Atom));
    Atom *targets = malloc(len);
    memcpy(targets, data, len);
    free(data);
    *n_targets = n;
    return targets;
}

static int cs_targets_contain(Atom *targets, int n, Atom a) {
    for (int i = 0; i < n; i++)
        if (targets[i] == a) return 1;
    return 0;
}

/* ── main clipboard processing ───────────────────────────────── */

static void cs_process_clipboard(void) {
    /* Who owns the clipboard? */
    Window owner = XGetSelectionOwner(g_dpy, CS_ATOM_CLIPBOARD);
    if (owner == None || owner == g_win) return;

    int n_targets = 0;
    Atom *targets = cs_get_targets(&n_targets);
    if (!targets || n_targets == 0) {
        free(targets);
        return;
    }

    /* ── Case 1: URI list (files/folders) ──────────────────────── */
    Atom file_target = None;
    if (cs_targets_contain(targets, n_targets, CS_ATOM_GNOME_FILES)) {
        file_target = CS_ATOM_GNOME_FILES;
    } else if (cs_targets_contain(targets, n_targets, CS_ATOM_URI_LIST)) {
        file_target = CS_ATOM_URI_LIST;
    }

    if (file_target != None) {
        size_t len = 0;
        unsigned char *data = cs_request_target(file_target, &len);
        if (data && len > 0) {
            const char *mime = (file_target == CS_ATOM_GNOME_FILES)
                               ? "x-special/gnome-copied-files"
                               : "text/uri-list";

            /*
             * Determine whether we are dealing with folders, files, or a mix.
             *
             * For x-special/gnome-copied-files the payload looks like:
             *   copy\n
             *   file:///path/to/item\n
             *   file:///path/to/other\n
             *
             * For text/uri-list it is just the URIs (one per line).
             *
             * We parse every "file://" URI, percent-decode it, stat() it and
             * check whether it is a directory.  If ALL entries are directories
             * we save as CS_TYPE_FOLDER; if ANY entry is a regular file we save
             * as CS_TYPE_FILE.  Unknown / non-file URIs default to CS_TYPE_FILE.
             */
            int has_dir  = 0;
            int has_file = 0;

            /* Work on a NUL-terminated copy so we can tokenise safely */
            char *payload = strndup((char *)data, len);
            if (payload) {
                char *line = payload;
                while (line && *line) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';

                    /* Skip the "copy" / "cut" action line and empty lines */
                    if (strncmp(line, "file://", 7) == 0) {
                        /* Percent-decode the path (also strips trailing \r) */
                        char decoded[4096];
                        cs_percent_decode(line + 7, decoded, sizeof(decoded));

                        struct stat st;
                        if (stat(decoded, &st) == 0) {
                            if (S_ISDIR(st.st_mode))
                                has_dir = 1;
                            else
                                has_file = 1;
                        } else {
                            /* stat failed — treat conservatively as file */
                            has_file = 1;
                        }
                    }

                    line = nl ? nl + 1 : NULL;
                }
                free(payload);
            }

            /*
             * Choose the item type:
             *   - pure directories → CS_TYPE_FOLDER
             *   - anything else (files, mixed, or unresolvable) → CS_TYPE_FILE
             */
            int item_type = (has_dir && !has_file) ? CS_TYPE_FOLDER : CS_TYPE_FILE;

            /*
             * Build a clean newline-separated list of local paths to store.
             * The UI expects plain paths like "/home/user/file.txt", not
             * "copy\nfile:///home/user/file.txt\n".  We decode every file://
             * URI into a local path here so both the daemon and the GTK
             * clipboard.c save data in the same format.
             */
            char *store_buf = malloc(len + 1);  /* decoded paths can only be shorter */
            if (!store_buf) { free(data); free(targets); return; }
            size_t store_len = 0;
            store_buf[0] = '\0';

            char *payload2 = strndup((char *)data, len);
            if (payload2) {
                char *line = payload2;
                while (line && *line) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';

                    if (strncmp(line, "file://", 7) == 0) {
                        char decoded[4096];
                        cs_percent_decode(line + 7, decoded, sizeof(decoded));
                        size_t dlen = strlen(decoded);
                        if (store_len + dlen + 2 <= len) {
                            if (store_len > 0)
                                store_buf[store_len++] = '\n';
                            memcpy(store_buf + store_len, decoded, dlen);
                            store_len += dlen;
                            store_buf[store_len] = '\0';
                        }
                    }

                    line = nl ? nl + 1 : NULL;
                }
                free(payload2);
            }

            if (store_len > 0) {
                cs_db_insert(item_type, store_buf, NULL, 0, mime);
                cs_db_prune();
            }
            free(store_buf);
        }
        if (data) free(data);
        free(targets);
        return;
    }
    
    /* ── Case 3: Text ──────────────────────────────────────────── */
    Atom text_target = None;
    const char *text_mime = NULL;

    if (cs_targets_contain(targets, n_targets, CS_ATOM_UTF8_STRING)) {
        text_target = CS_ATOM_UTF8_STRING;     text_mime = "text/plain;charset=utf-8";
    } else if (cs_targets_contain(targets, n_targets, CS_ATOM_TEXT_PLAIN_UTF8)) {
        text_target = CS_ATOM_TEXT_PLAIN_UTF8; text_mime = "text/plain;charset=utf-8";
    } else if (cs_targets_contain(targets, n_targets, CS_ATOM_TEXT_PLAIN)) {
        text_target = CS_ATOM_TEXT_PLAIN;      text_mime = "text/plain";
    } else if (cs_targets_contain(targets, n_targets, CS_ATOM_TEXT)) {
        text_target = CS_ATOM_TEXT;            text_mime = "text/plain";
    } else if (cs_targets_contain(targets, n_targets, XA_STRING)) {
        text_target = XA_STRING;          text_mime = "text/plain";    }

    if (text_target != None) {
        size_t len = 0;
        unsigned char *data = cs_request_target(text_target, &len);
        if (data && len > 0 && len <= (size_t)CS_MAX_TEXT_LEN) {
            /* Standard text insertion, fallback is no longer needed */
            cs_db_insert(CS_TYPE_TEXT, (char *)data, NULL, 0, text_mime);
            cs_db_prune();
        }
        if (data) free(data);
    }
    
    free(targets);
}

/* ── event loop ──────────────────────────────────────────────── */

static void cs_event_loop(void) {
    Window prev_owner = None;
    int    prev_timestamp = 0;

    LOG("watching CLIPBOARD on display %s", XDisplayName(NULL));

    while (g_running) {
        /* Drain X events */
        XEvent ev;
        while (XPending(g_dpy)) {
            XNextEvent(g_dpy, &ev);
            /* We are also interested in PropertyNotify for INCR,
               which is handled inside cs_request_target().
               Other events are silently discarded here. */
        }

        /* Poll clipboard owner */
        Window owner = XGetSelectionOwner(g_dpy, CS_ATOM_CLIPBOARD);
        if (owner != None && owner != g_win && owner != prev_owner) {
            prev_owner    = owner;
            prev_timestamp = 1;
            cs_process_clipboard();
        } else if (owner != None && owner != g_win && prev_timestamp) {
            /*
             * Same owner, but we want to catch re-copies from same app.
             * We can't reliably detect that with polling — we'd need
             * XFixesSelectSelectionInput which is an extension.
             * For now: re-query on every tick if owner is the same
             * (small overhead, ~1 extra TARGETS request per 250 ms).
             * Comment out the else-if body and keep only the outer
             * if-block if you want owner-change-only detection.
             */
        }

        /* Sleep CS_POLL_MS */
        struct timespec ts = {
            .tv_sec  = CS_POLL_MS / 1000,
            .tv_nsec = (CS_POLL_MS % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
}

/* ── XFixes for owner-change events (optional, more reliable) ── */

#ifdef HAVE_XFIXES
#include <X11/extensions/Xfixes.h>

static void cs_event_loop_xfixes(void) {
    int event_base, error_base;
    if (!XFixesQueryExtension(g_dpy, &event_base, &error_base)) {
        LOG("XFixes not available, falling back to polling");
        cs_event_loop();
        return;
    }

    XFixesSelectSelectionInput(g_dpy, g_win, CS_ATOM_CLIPBOARD,
                               XFixesSetSelectionOwnerNotifyMask |
                               XFixesSelectionClientCloseNotifyMask);

    LOG("watching CLIPBOARD via XFixes on display %s", XDisplayName(NULL));

    while (g_running) {
        XEvent ev;
        if (XPending(g_dpy)) {
            XNextEvent(g_dpy, &ev);
            if (ev.type == event_base + XFixesSelectionNotify) {
                XFixesSelectionNotifyEvent *sne = (XFixesSelectionNotifyEvent *)&ev;
                if (sne->selection == CS_ATOM_CLIPBOARD &&
                    sne->subtype == XFixesSetSelectionOwnerNotify &&
                    sne->owner != g_win) {
                    cs_process_clipboard();
                }
            }
        } else {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
}
#endif /* HAVE_XFIXES */

/* ── entry point ─────────────────────────────────────────────── */

int main(void) {
    /* Resolve the dynamic database path right at startup */
    cs_resolve_db_path();

    /* single-instance guard */
    int lock_fd = cs_acquire_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "clipstar-daemon: already running (lock: %s)\n", CS_LOCK_FILE);
        return 1;
    }

    /* signals */
    signal(SIGINT,  cs_sig_handler);
    signal(SIGTERM, cs_sig_handler);
    signal(SIGHUP,  cs_sig_handler);

    /* open X display */
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        LOG("cannot open display (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }

    /* create invisible window to receive SelectionNotify events */
    int screen = DefaultScreen(g_dpy);
    g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen),
                                0, 0, 1, 1, 0,
                                BlackPixel(g_dpy, screen),
                                BlackPixel(g_dpy, screen));

    /* We need PropertyNotify so INCR can be received */
    XSelectInput(g_dpy, g_win, PropertyChangeMask);
    XFlush(g_dpy);

    cs_intern_atoms();

    /* Resolve the dynamic database path right at startup */
    cs_resolve_db_path();

    /* open database */
    if (!cs_db_open()) {
        XDestroyWindow(g_dpy, g_win);
        XCloseDisplay(g_dpy);
        return 1;
    }

    LOG("started (pid %d)", (int)getpid());

#ifdef HAVE_XFIXES
    cs_event_loop_xfixes();
#else
    cs_event_loop();
#endif

    LOG("shutting down");

    sqlite3_close(g_db);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    unlink(CS_LOCK_FILE);
    close(lock_fd);
    return 0;
}
