#include "item.h"
#include <string.h>
#include <glib.h>

static ClipItem *clip_item_alloc(void) {
    ClipItem *item = g_new0(ClipItem, 1);
    item->timestamp = g_get_real_time() / G_USEC_PER_SEC;
    return item;
}

ClipItem *clip_item_new_text(const gchar *text) {
    g_return_val_if_fail(text != NULL, NULL);
    ClipItem *item   = clip_item_alloc();
    item->type       = CLIP_TYPE_TEXT;
    item->text       = g_strdup(text);
    item->mime_type  = g_strdup("text/plain;charset=utf-8");
    item->preview    = clip_item_make_preview(item);
    return item;
}

ClipItem *clip_item_new_image(GBytes *data, const gchar *mime) {
    g_return_val_if_fail(data != NULL, NULL);
    ClipItem *item   = clip_item_alloc();
    item->type       = CLIP_TYPE_IMAGE;
    item->blob       = g_bytes_ref(data);
    item->mime_type  = g_strdup(mime ? mime : "image/png");
    item->preview    = clip_item_make_preview(item);
    return item;
}

ClipItem *clip_item_new_file(const gchar *paths, GBytes *data, const gchar *mime) {
    g_return_val_if_fail(paths != NULL, NULL);
    ClipItem *item   = clip_item_alloc();

    /*
     * Normalize every line to a plain local path.
     * Input may be:
     *   - local paths already  ("/home/user/file.txt")
     *   - file:// URIs         ("file:///home/user/file.txt")
     *   - gnome payload        ("copy\nfile:///...\n")
     * We strip the action line and convert all file:// URIs to paths.
     */
    gchar **raw_lines = g_strsplit(paths, "\n", -1);
    GString *normalized = g_string_new(NULL);
    gboolean is_dir = FALSE;
    gchar *first_local = NULL;

    for (gint i = 0; raw_lines && raw_lines[i]; i++) {
        gchar *line = g_strstrip(raw_lines[i]);
        if (line[0] == '\0') continue;

        /* skip gnome action lines */
        if (g_strcmp0(line, "copy") == 0 || g_strcmp0(line, "cut") == 0) continue;

        gchar *local = NULL;
        if (g_str_has_prefix(line, "file://")) {
            local = g_filename_from_uri(line, NULL, NULL);
        } else {
            local = g_strdup(line);
        }

        if (!local) continue;

        if (normalized->len) g_string_append_c(normalized, '\n');
        g_string_append(normalized, local);

        if (!first_local) {
            first_local = g_strdup(local);
            is_dir = g_file_test(local, G_FILE_TEST_IS_DIR);
        }
        g_free(local);
    }
    g_strfreev(raw_lines);

    item->type = is_dir ? CLIP_TYPE_FOLDER : CLIP_TYPE_FILE;
    item->text = g_string_free(normalized, FALSE);
    g_free(first_local);

    if (data)
        item->blob   = g_bytes_ref(data);
    item->mime_type  = g_strdup(mime ? mime : "application/octet-stream");
    item->preview    = clip_item_make_preview(item);
    return item;
}

ClipItem *clip_item_new_uri(const gchar *uri_list) {
    g_return_val_if_fail(uri_list != NULL, NULL);
    ClipItem *item   = clip_item_alloc();
    item->type       = CLIP_TYPE_URI;
    item->text       = g_strdup(uri_list);
    item->mime_type  = g_strdup("text/uri-list");
    item->preview    = clip_item_make_preview(item);
    return item;
}

void clip_item_free(ClipItem *item) {
    if (!item) return;
    g_free(item->text);
    g_free(item->mime_type);
    g_free(item->preview);
    if (item->blob)
        g_bytes_unref(item->blob);
    g_free(item);
}

gchar *clip_item_make_preview(const ClipItem *item) {
    if (!item) return g_strdup("");

    switch (item->type) {
        case CLIP_TYPE_TEXT:
        case CLIP_TYPE_URI: {
            if (!item->text) return g_strdup("");
            
            gchar *collapsed = g_strdelimit(g_strdup(item->text), "\n\r\t", ' ');
            g_strstrip(collapsed);
            
            if (g_utf8_strlen(collapsed, -1) > 80) {
                gchar *end = g_utf8_offset_to_pointer(collapsed, 80);
                gchar *truncated = g_strndup(collapsed, end - collapsed);
                g_free(collapsed);
                gchar *result = g_strdup_printf("%s…", truncated);
                g_free(truncated);
                return result;
            }
            return collapsed;
        }

        case CLIP_TYPE_IMAGE: {
            gsize size = item->blob ? g_bytes_get_size(item->blob) : 0;
            if (size < 1024)
                return g_strdup_printf("Image (%zu B)", size);
            else if (size < 1024 * 1024)
                return g_strdup_printf("Image (%.1f KB)", size / 1024.0);
            else
                return g_strdup_printf("Image (%.1f MB)", size / (1024.0 * 1024.0));
        }

        case CLIP_TYPE_FILE:
        case CLIP_TYPE_FOLDER: {
            if (!item->text) 
                return g_strdup(item->type == CLIP_TYPE_FOLDER ? "Folder" : "File");

            gchar **lines = g_strsplit(item->text, "\n", -1);
            gchar *first_valid_path = NULL;
            gint valid_count = 0;

            for (gint i = 0; lines && lines[i]; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (line[0] == '\0' || g_strcmp0(line, "copy") == 0 || g_strcmp0(line, "cut") == 0) 
                    continue;
                
                if (!first_valid_path) 
                    first_valid_path = line;
                
                valid_count++;
            }

            gchar *result;
            if (valid_count == 0) {
                result = g_strdup(item->type == CLIP_TYPE_FOLDER ? "Folder" : "File");
            } else if (valid_count == 1) {
                gchar *basename = g_path_get_basename(first_valid_path);
                result = g_strdup(basename);
                g_free(basename);
            } else {
                gchar *basename = g_path_get_basename(first_valid_path);
                result = g_strdup_printf("%s (+%d items)", basename, valid_count - 1);
                g_free(basename);
            }

            g_strfreev(lines);
            return result;
        }
    }

    return g_strdup("");
}
