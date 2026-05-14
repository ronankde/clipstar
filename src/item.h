#pragma once

#include <glib.h>
#include <gio/gio.h>

typedef enum {
    CLIP_TYPE_TEXT   = 0,
    CLIP_TYPE_IMAGE  = 1,
    CLIP_TYPE_FILE   = 2,
    CLIP_TYPE_FOLDER = 3,  /* must match CS_TYPE_FOLDER=3 in clipstar-daemon */
    CLIP_TYPE_URI    = 4,
} ClipItemType;

typedef struct _ClipItem ClipItem;

struct _ClipItem {
    gint64       id;          /* DB row id, 0 if not saved yet */
    ClipItemType type;
    gchar       *text;        /* TEXT / URI: content; FILE: original path(s) */
    GBytes      *blob;        /* IMAGE / FILE: raw bytes                     */
    gchar       *mime_type;   /* e.g. "text/plain", "image/png"              */
    gchar       *preview;     /* short human-readable preview for UI         */
    gint64       timestamp;   /* unix seconds                                */
    gboolean     starred;     /* user-pinned item                            */
};

ClipItem  *clip_item_new_text   (const gchar *text);
ClipItem  *clip_item_new_image  (GBytes *data, const gchar *mime);
ClipItem  *clip_item_new_file   (const gchar *paths, GBytes *data, const gchar *mime);
ClipItem  *clip_item_new_uri    (const gchar *uri_list);
void       clip_item_free       (ClipItem *item);

/* returns a newly-allocated short preview string (max ~80 chars) */
gchar     *clip_item_make_preview (const ClipItem *item);
