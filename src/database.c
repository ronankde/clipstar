#include "database.h"
#include "application.h"
#include <sqlite3.h>
#include <glib.h>
#include <string.h>

struct _ClipDatabase {
    sqlite3 *db;
    gchar   *path;
};

/* ── helpers ─────────────────────────────────────────────────── */

static gchar *get_db_path(void) {
    const gchar *data_dir = g_get_user_data_dir();
    gchar *dir = g_build_filename(data_dir, "clipgnome", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, CLIP_GNOME_DB_NAME, NULL);
    g_free(dir);
    return path;
}

static void db_exec(ClipDatabase *self, const gchar *sql) {
    char *err = NULL;
    if (sqlite3_exec(self->db, sql, NULL, NULL, &err) != SQLITE_OK) {
        g_warning("SQLite exec error: %s\n  SQL: %s", err, sql);
        sqlite3_free(err);
    }
}

/* ── lifecycle ───────────────────────────────────────────────── */

ClipDatabase *clip_database_new(void) {
    ClipDatabase *self = g_new0(ClipDatabase, 1);
    self->path = get_db_path();
    return self;
}

void clip_database_free(ClipDatabase *db) {
    if (!db) return;
    clip_database_close(db);
    g_free(db->path);
    g_free(db);
}

gboolean clip_database_open(ClipDatabase *self) {
    if (sqlite3_open(self->path, &self->db) != SQLITE_OK) {
        g_critical("Cannot open DB at %s: %s", self->path,
                   sqlite3_errmsg(self->db));
        return FALSE;
    }
    /* performance pragmas */
    db_exec(self, "PRAGMA journal_mode=WAL;");
    db_exec(self, "PRAGMA synchronous=NORMAL;");
    db_exec(self, "PRAGMA foreign_keys=ON;");
    return TRUE;
}

void clip_database_migrate(ClipDatabase *self) {
    const gchar *ddl =
        "CREATE TABLE IF NOT EXISTS clips ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  type       INTEGER NOT NULL DEFAULT 0,"
        "  text       TEXT,"
        "  blob       BLOB,"
        "  mime_type  TEXT,"
        "  preview    TEXT,"
        "  timestamp  INTEGER NOT NULL,"
        "  starred    INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_clips_timestamp ON clips(timestamp DESC);"
        "CREATE INDEX IF NOT EXISTS idx_clips_starred   ON clips(starred);"
        /* FTS5 virtual table for full-text search on preview + text */
        "CREATE VIRTUAL TABLE IF NOT EXISTS clips_fts USING fts5("
        "  preview, text, content='clips', content_rowid='id'"
        ");"
        /* triggers to keep FTS in sync */
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

    db_exec(self, ddl);
}

void clip_database_close(ClipDatabase *self) {
    if (self && self->db) {
        sqlite3_close(self->db);
        self->db = NULL;
    }
}

/* ── row → ClipItem ──────────────────────────────────────────── */

static ClipItem *row_to_item(sqlite3_stmt *stmt) {
    ClipItem *item  = g_new0(ClipItem, 1);
    item->id        = sqlite3_column_int64(stmt, 0);
    item->type      = (ClipItemType)sqlite3_column_int(stmt, 1);

    const gchar *text = (const gchar *)sqlite3_column_text(stmt, 2);
    item->text      = text ? g_strdup(text) : NULL;

    const void *blob = sqlite3_column_blob(stmt, 3);
    int blob_len     = sqlite3_column_bytes(stmt, 3);
    if (blob && blob_len > 0)
        item->blob   = g_bytes_new(blob, blob_len);

    const gchar *mime = (const gchar *)sqlite3_column_text(stmt, 4);
    item->mime_type  = mime ? g_strdup(mime) : NULL;

    const gchar *prev = (const gchar *)sqlite3_column_text(stmt, 5);
    item->preview    = prev ? g_strdup(prev) : NULL;

    item->timestamp  = sqlite3_column_int64(stmt, 6);
    item->starred    = sqlite3_column_int(stmt, 7) != 0;
    return item;
}

static GPtrArray *run_select(ClipDatabase *self, sqlite3_stmt *stmt) {
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)clip_item_free);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        g_ptr_array_add(arr, row_to_item(stmt));
    if (rc != SQLITE_DONE)
        g_warning("SQLite step error: %s", sqlite3_errmsg(self->db));
    sqlite3_finalize(stmt);
    return arr;
}

/* ── CRUD ────────────────────────────────────────────────────── */

const gchar *clip_database_get_path(ClipDatabase *self) {
    return self->path;
}

gboolean clip_database_delete(ClipDatabase *self, gint64 id) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db,
        "DELETE FROM clips WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void clip_database_touch(ClipDatabase *self, gint64 id) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db,
        "UPDATE clips SET timestamp=? WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

gboolean clip_database_star(ClipDatabase *self, gint64 id, gboolean starred) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db,
        "UPDATE clips SET starred=? WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int  (stmt, 1, starred ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void clip_database_clear(ClipDatabase *self) {
    db_exec(self, "DELETE FROM clips WHERE starred=0;");
}

/* ── queries ─────────────────────────────────────────────────── */

GPtrArray *clip_database_recent(ClipDatabase *self, gint limit) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db,
        "SELECT id,type,text,blob,mime_type,preview,timestamp,starred"
        " FROM clips ORDER BY starred DESC, timestamp DESC LIMIT ?;",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, limit);
    return run_select(self, stmt);
}

GPtrArray *clip_database_starred(ClipDatabase *self) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db,
        "SELECT id,type,text,blob,mime_type,preview,timestamp,starred"
        " FROM clips WHERE starred=1 ORDER BY timestamp DESC;",
        -1, &stmt, NULL);
    return run_select(self, stmt);
}

GPtrArray *clip_database_search(ClipDatabase *self,
                                const gchar  *query,
                                gint          limit) {
    if (!query || query[0] == '\0')
        return clip_database_recent(self, limit);

    /* build FTS query – append * for prefix matching */
    gchar *fts_query = g_strdup_printf("%s*", query);

    sqlite3_stmt *stmt;
    const gchar *sql =
        "SELECT c.id,c.type,c.text,c.blob,c.mime_type,c.preview,c.timestamp,c.starred"
        " FROM clips c"
        " JOIN clips_fts f ON c.id = f.rowid"
        " WHERE clips_fts MATCH ?"
        " ORDER BY c.starred DESC, rank, c.timestamp DESC"
        " LIMIT ?;";

    sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, limit);
    g_free(fts_query);

    return run_select(self, stmt);
}

/* end of database.c */
