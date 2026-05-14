#pragma once

#include <gtk/gtk.h>

#define CLIP_GNOME_APP_ID "io.github.clipgnome"
#define CLIP_GNOME_VERSION "1.0.0"
#define CLIP_GNOME_DB_NAME "clipgnome.db"

/* Max history entries to keep in DB */
#define CLIP_MAX_HISTORY 500

/* Max file size to store inline (bytes): 10 MB */
#define CLIP_MAX_FILE_SIZE (10 * 1024 * 1024)

typedef struct _ClipGnomeApp ClipGnomeApp;

#define CLIP_TYPE_GNOME_APP (clip_gnome_app_get_type())
G_DECLARE_FINAL_TYPE(ClipGnomeApp, clip_gnome_app, CLIP, GNOME_APP, GtkApplication)

ClipGnomeApp *clip_gnome_app_new(void);
