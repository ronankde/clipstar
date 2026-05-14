#include "application.h"
#include "window.h"
#include "database.h"
#include <glib.h>
#include <gio/gio.h>

struct _ClipGnomeApp {
    GtkApplication parent_instance;
    ClipDatabase  *db;
    ClipWindow    *window;
    GFileMonitor  *db_monitor;  /* watches the .db-wal file for daemon writes */
    guint          refresh_timeout;
};

G_DEFINE_TYPE(ClipGnomeApp, clip_gnome_app, GTK_TYPE_APPLICATION)

static void on_activate(GApplication *app);
static void on_toggle_window(GSimpleAction *action, GVariant *param, gpointer user_data);

/* Debounced refresh — coalesce rapid WAL writes into one UI update */
static gboolean do_refresh(gpointer user_data) {
    ClipGnomeApp *self = CLIP_GNOME_APP(user_data);
    self->refresh_timeout = 0;
    if (self->window)
        clip_window_refresh(self->window);
    return G_SOURCE_REMOVE;
}

static void on_db_changed(GFileMonitor *mon, GFile *file, GFile *other,
                           GFileMonitorEvent event, gpointer user_data) {
    (void)mon; (void)file; (void)other;
    if (event != G_FILE_MONITOR_EVENT_CHANGED &&
        event != G_FILE_MONITOR_EVENT_CREATED) return;

    ClipGnomeApp *self = CLIP_GNOME_APP(user_data);
    /* debounce: wait 150 ms after the last write before refreshing */
    if (self->refresh_timeout)
        g_source_remove(self->refresh_timeout);
    self->refresh_timeout = g_timeout_add(150, do_refresh, self);
}

static void clip_gnome_app_dispose(GObject *object) {
    ClipGnomeApp *self = CLIP_GNOME_APP(object);
    if (self->refresh_timeout) {
        g_source_remove(self->refresh_timeout);
        self->refresh_timeout = 0;
    }
    g_clear_object(&self->db_monitor);
    g_clear_object(&self->db);
    G_OBJECT_CLASS(clip_gnome_app_parent_class)->dispose(object);
}

static void clip_gnome_app_class_init(ClipGnomeAppClass *klass) {
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->dispose = clip_gnome_app_dispose;

    GApplicationClass *app_class = G_APPLICATION_CLASS(klass);
    app_class->activate = on_activate;
}

static void clip_gnome_app_init(ClipGnomeApp *self) {
    const GActionEntry entries[] = {
        { "toggle-window", on_toggle_window, NULL, NULL, NULL },
        { "quit",          NULL,             NULL, NULL, NULL },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(self), entries,
                                    G_N_ELEMENTS(entries), self);

    const char *toggle_accels[] = { "<Super>v", NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                          "app.toggle-window", toggle_accels);
}

static void on_activate(GApplication *gapp) {
    ClipGnomeApp *self = CLIP_GNOME_APP(gapp);

    if (!self->db) {
        self->db = clip_database_new();
        if (!clip_database_open(self->db)) {
            g_critical("Failed to open database");
            g_application_quit(gapp);
            return;
        }
        clip_database_migrate(self->db);

        /* Watch the WAL file — the daemon writes here on every INSERT */
        gchar *wal_path = g_strdup_printf("%s-wal",
                              clip_database_get_path(self->db));
        GFile *wal_file = g_file_new_for_path(wal_path);
        g_free(wal_path);

        self->db_monitor = g_file_monitor_file(wal_file,
                               G_FILE_MONITOR_NONE, NULL, NULL);
        g_object_unref(wal_file);

        if (self->db_monitor)
            g_signal_connect(self->db_monitor, "changed",
                             G_CALLBACK(on_db_changed), self);
    }

    if (!self->window)
        self->window = clip_window_new(GTK_APPLICATION(self), self->db);

    gtk_window_present(GTK_WINDOW(self->window));
}

static void on_toggle_window(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action; (void)param;
    ClipGnomeApp *self = CLIP_GNOME_APP(user_data);

    if (!self->window) {
        on_activate(G_APPLICATION(self));
        return;
    }

    if (gtk_widget_get_visible(GTK_WIDGET(self->window)))
        gtk_widget_set_visible(GTK_WIDGET(self->window), FALSE);
    else {
        gtk_window_present(GTK_WINDOW(self->window));
        clip_window_focus_search(self->window);
    }
}

ClipGnomeApp *clip_gnome_app_new(void) {
    return g_object_new(CLIP_TYPE_GNOME_APP,
                        "application-id", CLIP_GNOME_APP_ID,
                        "flags",          G_APPLICATION_DEFAULT_FLAGS,
                        NULL);
}
