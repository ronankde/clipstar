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
    gchar *dir = g_build_filename(data_dir, "clipstar", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, CLIP_STAR_DB_NAME, NULL);
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
    db_exec(self, "PRAGMA journal_mode=WAL;");
    db_exec(self, "PRAGMA synchronous=NORMAL;");
    db_exec(self, "PRAGMA foreign_keys=ON;");
    return TRUE;
}

void clip_database_migrate(ClipDatabase *self) {
    db_exec(self, 
        "CREATE TABLE IF NOT EXISTS clips ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  type       INTEGER NOT NULL DEFAULT 0,"
        "  text       TEXT,"
        "  blob       BLOB,"
        "  mime_type  TEXT,"
        "  preview    TEXT,"
        "  timestamp  INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_clips_timestamp ON clips(timestamp DESC);");

    /* Recriamos o índice FTS para garantir que está limpo e sem resquícios antigos */
    db_exec(self, "DROP TABLE IF EXISTS clips_fts;");
    db_exec(self, "CREATE VIRTUAL TABLE clips_fts USING fts5(preview, text);");

    /* Removemos gatilhos velhos que podem estar com a sintaxe errada */
    db_exec(self, "DROP TRIGGER IF EXISTS clips_ai;");
    db_exec(self, "DROP TRIGGER IF EXISTS clips_ad;");
    db_exec(self, "DROP TRIGGER IF EXISTS clips_au;");

    /* Sintaxe CORRETA para deletar e atualizar FTS independente */
    const gchar *triggers =
        "CREATE TRIGGER clips_ai AFTER INSERT ON clips BEGIN"
        "  INSERT INTO clips_fts(rowid, preview, text)"
        "    VALUES (new.id, new.preview, CASE WHEN new.type IN (0, 4) THEN new.text ELSE '' END);"
        "END;"
        
        "CREATE TRIGGER clips_ad AFTER DELETE ON clips BEGIN"
        "  DELETE FROM clips_fts WHERE rowid = old.id;" /* Sintaxe corrigida! */
        "END;"
        
        "CREATE TRIGGER clips_au AFTER UPDATE ON clips BEGIN"
        "  DELETE FROM clips_fts WHERE rowid = old.id;" /* Sintaxe corrigida! */
        "  INSERT INTO clips_fts(rowid, preview, text)"
        "    VALUES (new.id, new.preview, CASE WHEN new.type IN (0, 4) THEN new.text ELSE '' END);"
        "END;";
    
    db_exec(self, triggers);

    /* Repopula o índice */
    db_exec(self, 
        "INSERT INTO clips_fts(rowid, preview, text) "
        "SELECT id, preview, CASE WHEN type IN (0, 4) THEN text ELSE '' END FROM clips;");
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
    sqlite3_prepare_v2(self->db, "DELETE FROM clips WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    
    /* Agora o banco de dados vai nos avisar se algo falhar! */
    if (rc != SQLITE_DONE) {
        g_warning("Falha silenciosa do SQLite ao deletar: %s", sqlite3_errmsg(self->db));
    }
    
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void clip_database_touch(ClipDatabase *self, gint64 id) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db, "UPDATE clips SET timestamp=? WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void clip_database_clear(ClipDatabase *self) {
    db_exec(self, "DELETE FROM clips;");
    db_exec(self, "DELETE FROM clips_fts;");
}

/* ── queries ─────────────────────────────────────────────────── */

GPtrArray *clip_database_recent(ClipDatabase *self, gint limit) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(self->db,
        "SELECT id,type,text,blob,mime_type,preview,timestamp"
        " FROM clips ORDER BY timestamp DESC LIMIT ?;",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, limit);
    return run_select(self, stmt);
}

GPtrArray *clip_database_search(ClipDatabase *self, const gchar *query, gint limit) {
    if (!query || query[0] == '\0')
        return clip_database_recent(self, limit);

    GString *fts_str = g_string_new("\"");
    for (const gchar *p = query; *p != '\0'; p++) {
        if (*p == '"') g_string_append(fts_str, "\"\"");
        else g_string_append_c(fts_str, *p);
    }
    g_string_append(fts_str, "\"*");
    gchar *fts_query = g_string_free(fts_str, FALSE);

    sqlite3_stmt *stmt;
    const gchar *sql =
        "SELECT c.id,c.type,c.text,c.blob,c.mime_type,c.preview,c.timestamp"
        " FROM clips c"
        " JOIN clips_fts f ON c.id = f.rowid"
        " WHERE clips_fts MATCH ?"
        " ORDER BY rank, c.timestamp DESC"
        " LIMIT ?;";

    sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, limit);
    g_free(fts_query);

    return run_select(self, stmt);
}
