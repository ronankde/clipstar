#pragma once

#include "item.h"
#include <glib.h>

typedef struct _ClipDatabase ClipDatabase;

ClipDatabase *clip_database_new     (void);
void          clip_database_free    (ClipDatabase *db);

gboolean      clip_database_open    (ClipDatabase *db);
void          clip_database_migrate (ClipDatabase *db);
void          clip_database_close   (ClipDatabase *db);
const gchar  *clip_database_get_path(ClipDatabase *db);

/* CRUD */
gboolean      clip_database_delete  (ClipDatabase *db, gint64 id);
void          clip_database_touch   (ClipDatabase *db, gint64 id);
void          clip_database_clear   (ClipDatabase *db);

/* Queries */
GPtrArray    *clip_database_search  (ClipDatabase *db, const gchar *query, gint limit);
GPtrArray    *clip_database_recent  (ClipDatabase *db, gint limit);
