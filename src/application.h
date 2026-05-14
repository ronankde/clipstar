#pragma once

#include <gtk/gtk.h>

#define CLIP_STAR_APP_ID "io.github.clipstar"
#define CLIP_STAR_VERSION "1.0.0"
#define CLIP_STAR_DB_NAME "clipstar.db"

/* Max history entries to keep in DB */
#define CLIP_MAX_HISTORY 5000

/* Max file size to store inline (bytes): 10 MB */
#define CLIP_MAX_FILE_SIZE (10 * 1024 * 1024)

typedef struct _ClipStarApp ClipStarApp;

#define CLIP_TYPE_STAR_APP (clip_star_app_get_type())
G_DECLARE_FINAL_TYPE(ClipStarApp, clip_star_app, CLIP, STAR_APP, GtkApplication)

ClipStarApp *clip_star_app_new(void);
