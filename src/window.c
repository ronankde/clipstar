#include "window.h"
#include "item.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <string.h>

#define RESULTS_LIMIT 100
#define SEARCH_DEBOUNCE_MS 150

struct _ClipWindow {
    GtkApplicationWindow  parent_instance;

    ClipDatabase *db;

    /* widgets */
    GtkWidget *search_entry;
    GtkWidget *list_box;
    GtkWidget *stack;          /* "empty" vs "results" pages */
    GtkWidget *filter_bar;
    GtkWidget *btn_starred;
    GtkWidget *btn_clear;
    GtkWidget *status_label;
    GtkWidget *scrolled;

    /* state */
    guint      search_timeout;
    gboolean   show_starred;
    GPtrArray *current_items;  /* items currently displayed */
};

G_DEFINE_TYPE(ClipWindow, clip_window, GTK_TYPE_APPLICATION_WINDOW)

/* ── forward declarations ────────────────────────────────────── */
static void     populate_list              (ClipWindow *self, GPtrArray *items);
static void     do_search                  (ClipWindow *self);
static gboolean search_debounce_cb         (gpointer user_data);
static void     on_search_changed          (GtkSearchEntry *entry, gpointer user_data);
static void     on_row_activated           (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data);
static void     on_star_toggled            (GtkToggleButton *btn, gpointer user_data);
static void     on_clear_clicked           (GtkButton *btn, gpointer user_data);
static void     on_delete_item             (GtkButton *btn, gpointer user_data);
static void     on_copy_item               (GtkButton *btn, gpointer user_data);
static void     execute_copy_and_close     (ClipWindow *self, guint idx);

static void     restore_files_to_clipboard (GdkClipboard *clipboard, const gchar *paths);
static void     restore_item_to_clipboard  (ClipWindow *self, ClipItem *item);

static gboolean on_enter_shortcut      (GtkWidget *widget, GVariant *args, gpointer user_data);
static gboolean on_esc_shortcut        (GtkWidget *widget, GVariant *args, gpointer user_data);
static gboolean on_search_key_pressed  (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean on_search_key_pressed  (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean on_global_key_pressed  (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);

static GtkWidget *build_row                 (ClipWindow *self, ClipItem *item, gboolean is_first);
static void      update_status              (ClipWindow *self, guint count);
static void      update_row                 (GtkWidget *row, ClipItem *item, gboolean is_first);

/* ── CSS ─────────────────────────────────────────────────────── */
static const gchar *APP_CSS =
    "window.clip-window {"
    "  background-color: #1e1e2e;"
    "  font-family: Adwaita Sans, sans-serif;" /* Standard GNOME font family */
    "  font-size: 11pt;"                     /* Standard GNOME font size */
    "}"
    /* Force the list and scrolled containers to use the same background color */
    /* This prevents white areas when the list has few items */
    "list, scrolledwindow {"
    "  background-color: #1e1e2e;"
    "  border: none;"
    "}"
    ".clip-header {"
    "  background-color: #181825;"
    "  border-bottom: 1px solid #313244;"
    "  padding: 8px 12px;"
    "}"
    ".clip-search {"
    "  border-radius: 8px;"
    "  background-color: #313244;"
    "  color: #cdd6f4;"
    "  border: 1px solid #45475a;"
    "  padding: 6px 12px;"
    "  font-size: 11pt;"
    "}"
    ".clip-search:focus {"
    "  border-color: #89b4fa;"
    "  outline: none;"
    "}"
    ".clip-row {"
    "  padding: 8px 12px;"
    "  border-bottom: 1px solid #181825;"
    "  background-color: #1e1e2e;"
    "}"
    ".clip-row:hover, .clip-row:selected {"
    "  background-color: #313244;"
    "}"
    ".clip-row.starred {"
    "  border-left: 3px solid #f9e2af;"
    "}"
    ".clip-preview {"
    "  color: #cdd6f4;"
    "  font-family: monospace;"
    "  font-size: 11pt;"
    "}"
    ".clip-meta {"
    "  color: #6c7086;"
    "  font-size: 9pt;"
    "}"
    ".clip-type-badge {"
    "  border-radius: 4px;"
    "  padding: 1px 5px;"
    "  font-size: 8pt;"
    "  font-weight: bold;"
    "  color: #1e1e2e;"
    "}"
    ".badge-text  { background-color: #89b4fa; }"
    ".badge-image { background-color: #a6e3a1; }"
    ".badge-file  { background-color: #fab387; }"
    ".badge-folder{ background-color: #f9e2af; }"
    ".badge-uri   { background-color: #cba6f7; }"
    ".clip-action-btn {"
    "  min-width: 28px;"
    "  min-height: 28px;"
    "  padding: 2px;"
    "  border-radius: 6px;"
    "  background: transparent;"
    "  border: none;"
    "  color: #6c7086;"
    "}"
    ".clip-action-btn:hover {"
    "  background-color: #45475a;"
    "  color: #cdd6f4;"
    "}"
    ".star-btn.active {"
    "  color: #f9e2af;"
    "}"
    ".filter-btn {"
    "  border-radius: 6px;"
    "  padding: 4px 10px;"
    "  background: transparent;"
    "  border: 1px solid #45475a;"
    "  color: #6c7086;"
    "  font-size: 10pt;"
    "}"
    ".filter-btn:checked, .filter-btn:active {"
    "  background-color: #313244;"
    "  color: #cdd6f4;"
    "  border-color: #89b4fa;"
    "}"
    ".status-label {"
    "  color: #6c7086;"
    "  font-size: 9pt;"
    "  padding: 4px 12px;"
    "}"
    /* Ensure the empty state page also matches the background */
    ".empty-page {"
    "  background-color: #1e1e2e;"
    "  color: #6c7086;"
    "  font-size: 11pt;"
    "}"
    ".clear-btn {"
    "  color: #f38ba8;"
    "  border: 1px solid #45475a;"
    "  border-radius: 6px;"
    "  padding: 4px 10px;"
    "  background: transparent;"
    "  font-size: 10pt;"
    "}"
    ".clear-btn:hover {"
    "  background-color: #313244;"
    "}";

/* ── GObject boilerplate ─────────────────────────────────────── */
static void clip_window_dispose(GObject *object) {
    ClipWindow *self = CLIP_WINDOW(object);
    if (self->search_timeout) {
        g_source_remove(self->search_timeout);
        self->search_timeout = 0;
    }
    if (self->current_items) {
        g_ptr_array_free(self->current_items, TRUE);
        self->current_items = NULL;
    }
    G_OBJECT_CLASS(clip_window_parent_class)->dispose(object);
}

static void clip_window_class_init(ClipWindowClass *klass) {
    GObjectClass *obj = G_OBJECT_CLASS(klass);
    obj->dispose = clip_window_dispose;
}

static void clip_window_init(ClipWindow *self) {
    /* Load CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, APP_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Window properties */
    gtk_widget_add_css_class(GTK_WIDGET(self), "clip-window");
    gtk_window_set_title(GTK_WINDOW(self), "ClipGnome");
    gtk_window_set_default_size(GTK_WINDOW(self), 480, 600);
    gtk_window_set_deletable(GTK_WINDOW(self), FALSE);

    g_signal_connect(self, "show", G_CALLBACK(clip_window_refresh), NULL);
    
    /* Global Shortcuts (Wayland/IM-proof) */
    GtkEventController *shortcut_ctrl = gtk_shortcut_controller_new();
    gtk_event_controller_set_propagation_phase(shortcut_ctrl, GTK_PHASE_CAPTURE);

    /* Bind Escape */
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                         gtk_callback_action_new(on_esc_shortcut, self, NULL)));

    /* Bind Enter */
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Return, 0),
                         gtk_callback_action_new(on_enter_shortcut, self, NULL)));

    /* Bind Numpad Enter */
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_KP_Enter, 0),
                         gtk_callback_action_new(on_enter_shortcut, self, NULL)));

    gtk_widget_add_controller(GTK_WIDGET(self), shortcut_ctrl);
    
    /* Absolute Window Key Interceptor (To beat the search entry key capture) */
    GtkEventController *global_key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(global_key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(global_key_ctrl, "key-pressed", G_CALLBACK(on_global_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), global_key_ctrl);

    /* ── layout ───────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(self), vbox);

    /* header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(header, "clip-header");
    gtk_box_append(GTK_BOX(vbox), header);

    /* search row */
    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(header), search_row);

    self->search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(self->search_entry, "clip-search");
    gtk_widget_set_hexpand(self->search_entry, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->search_entry),
                                          "Search clipboard history…");
    gtk_box_append(GTK_BOX(search_row), self->search_entry);

    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

    /* Down arrow navigation */
    GtkEventController *search_key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(search_key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(search_key_ctrl, "key-pressed", G_CALLBACK(on_search_key_pressed), self);
    gtk_widget_add_controller(self->search_entry, search_key_ctrl);

    /* Enable type-to-search */
    gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(self->search_entry), GTK_WIDGET(self));

    /* filter row */
    GtkWidget *filter_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(header), filter_row);

    self->btn_starred = gtk_toggle_button_new();
    gtk_button_set_label(GTK_BUTTON(self->btn_starred), "⭐ Starred");
    gtk_widget_add_css_class(self->btn_starred, "filter-btn");
    gtk_box_append(GTK_BOX(filter_row), self->btn_starred);
    g_signal_connect(self->btn_starred, "toggled",
                     G_CALLBACK(on_star_toggled), self);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(filter_row), spacer);

    self->btn_clear = gtk_button_new_with_label("🗑 Clear history");
    gtk_widget_add_css_class(self->btn_clear, "clear-btn");
    gtk_box_append(GTK_BOX(filter_row), self->btn_clear);
    g_signal_connect(self->btn_clear, "clicked",
                     G_CALLBACK(on_clear_clicked), self);

    /* status */
    self->status_label = gtk_label_new("");
    gtk_widget_add_css_class(self->status_label, "status-label");
    gtk_widget_set_halign(self->status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), self->status_label);

    /* stack: results | empty */
    self->stack = gtk_stack_new();
    gtk_widget_set_vexpand(self->stack, TRUE);
    gtk_box_append(GTK_BOX(vbox), self->stack);

    /* results page */
    self->scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    self->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list_box), GTK_SELECTION_BROWSE);
    gtk_widget_set_focusable(self->list_box, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled), self->list_box);
    
    gtk_stack_add_named(GTK_STACK(self->stack), self->scrolled, "results");
    g_signal_connect(self->list_box, "row-activated",
                     G_CALLBACK(on_row_activated), self);

    /* empty page */
    GtkWidget *empty_label = gtk_label_new("No clipboard items yet.\nCopy something!");
    gtk_widget_add_css_class(empty_label, "empty-page");
    gtk_label_set_justify(GTK_LABEL(empty_label), GTK_JUSTIFY_CENTER);
    gtk_stack_add_named(GTK_STACK(self->stack), empty_label, "empty");
}

/* ── public API ──────────────────────────────────────────────── */
ClipWindow *clip_window_new(GtkApplication *app, ClipDatabase *db) {
    ClipWindow *self = g_object_new(CLIP_TYPE_WINDOW,
                                    "application", app,
                                    NULL);
    self->db = db;
    clip_window_refresh(self);
    return self;
}

void clip_window_refresh(ClipWindow *self) {
    do_search(self);
}

void clip_window_focus_search(ClipWindow *self) {
    gtk_widget_grab_focus(self->search_entry);
}

/* ── search & list population ────────────────────────────────── */

static gboolean search_debounce_cb(gpointer user_data) {
    ClipWindow *self = CLIP_WINDOW(user_data);
    self->search_timeout = 0;
    do_search(self);
    return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)entry;
    ClipWindow *self = CLIP_WINDOW(user_data);
    if (self->search_timeout)
        g_source_remove(self->search_timeout);
    self->search_timeout = g_timeout_add(SEARCH_DEBOUNCE_MS,
                                         search_debounce_cb, self);
}

static void do_search(ClipWindow *self) {
    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));

    GPtrArray *items;
    if (self->show_starred)
        items = clip_database_starred(self->db);
    else if (query && query[0] != '\0')
        items = clip_database_search(self->db, query, RESULTS_LIMIT);
    else
        items = clip_database_recent(self->db, RESULTS_LIMIT);

    if (self->current_items)
        g_ptr_array_free(self->current_items, TRUE);
    self->current_items = items;

    populate_list(self, items);
    update_status(self, items->len);
}

static void update_status(ClipWindow *self, guint count) {
    gchar *txt;
    if (count == 0)
        txt = g_strdup("No items");
    else if (count == 1)
        txt = g_strdup("1 item");
    else
        txt = g_strdup_printf("%u items", count);
    gtk_label_set_text(GTK_LABEL(self->status_label), txt);
    g_free(txt);
}

static void populate_list(ClipWindow *self, GPtrArray *items) {
    if (!items || items->len == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "empty");
        return;
    }

    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "results");

    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    gboolean is_default_view = (query == NULL || query[0] == '\0') && !self->show_starred;

    /* Get the first existing row to start recycling */
    GtkWidget *row_widget = gtk_widget_get_first_child(self->list_box);

    for (guint i = 0; i < items->len; i++) {
        ClipItem *item = g_ptr_array_index(items, i);
        gboolean is_first = (is_default_view && i == 0);

        if (row_widget != NULL) {
            /* If a row already exists, update its content (prevents hover flickering) */
            update_row(row_widget, item, is_first);
            row_widget = gtk_widget_get_next_sibling(row_widget);
        } else {
            /* If we need more rows than we have, create new ones */
            GtkWidget *new_row = build_row(self, item, is_first);
            gtk_list_box_append(GTK_LIST_BOX(self->list_box), new_row);
        }
    }

    /* Remove any excess rows that are no longer needed */
    while (row_widget != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(row_widget);
        gtk_list_box_remove(GTK_LIST_BOX(self->list_box), row_widget);
        row_widget = next;
    }
}

/* ── row builder ─────────────────────────────────────────────── */

static gchar *format_time(gint64 ts) {
    gint64 now  = g_get_real_time() / G_USEC_PER_SEC;
    gint64 diff = now - ts;

    if (diff < 60)          return g_strdup("just now");
    if (diff < 3600)        return g_strdup_printf("%lld min ago", (long long)(diff / 60));
    if (diff < 86400)       return g_strdup_printf("%lld hr ago",  (long long)(diff / 3600));
    if (diff < 86400 * 7)   return g_strdup_printf("%lld days ago",(long long)(diff / 86400));

    GDateTime *dt = g_date_time_new_from_unix_local(ts);
    gchar *s = g_date_time_format(dt, "%b %d %Y");
    g_date_time_unref(dt);
    return s;
}

static const gchar *type_badge_class(ClipItemType t) {
    switch (t) {
        case CLIP_TYPE_TEXT:   return "badge-text";
        case CLIP_TYPE_IMAGE:  return "badge-image";
        case CLIP_TYPE_FILE:   return "badge-file";
        case CLIP_TYPE_FOLDER: return "badge-folder";
        case CLIP_TYPE_URI:    return "badge-uri";
    }
    return "badge-text";
}

static const gchar *type_label(ClipItemType t) {
    switch (t) {
        case CLIP_TYPE_TEXT:   return "TEXT";
        case CLIP_TYPE_IMAGE:  return "IMG";
        case CLIP_TYPE_FILE:   return "FILE";
        case CLIP_TYPE_FOLDER: return "FOLDER";
        case CLIP_TYPE_URI:    return "URI";
    }
    return "?";
}

static void on_star_item(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    ClipWindow *self = CLIP_WINDOW(g_object_get_data(G_OBJECT(btn), "window"));
    gint64   id      = (gint64)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "item-id"));
    gboolean starred = (gboolean)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "starred"));
    clip_database_star(self->db, id, !starred);
    clip_window_refresh(self);
}

/* Recycles an existing row by updating its content instead of destroying it */
static void update_row(GtkWidget *row, ClipItem *item, gboolean is_first) {
    /* Retrieve the cached widgets from the row */
    GtkWidget *badge      = g_object_get_data(G_OBJECT(row), "badge");
    GtkWidget *prev_label = g_object_get_data(G_OBJECT(row), "prev_label");
    GtkWidget *meta       = g_object_get_data(G_OBJECT(row), "meta");
    GtkWidget *copy_btn   = g_object_get_data(G_OBJECT(row), "copy_btn");
    GtkWidget *star_btn   = g_object_get_data(G_OBJECT(row), "star_btn");
    GtkWidget *del_btn    = g_object_get_data(G_OBJECT(row), "del_btn");

    /* Update starred row style */
    if (item->starred) gtk_widget_add_css_class(row, "starred");
    else gtk_widget_remove_css_class(row, "starred");

    /* Update Badge */
    gtk_label_set_text(GTK_LABEL(badge), type_label(item->type));
    gtk_widget_remove_css_class(badge, "badge-text");
    gtk_widget_remove_css_class(badge, "badge-image");
    gtk_widget_remove_css_class(badge, "badge-file");
    gtk_widget_remove_css_class(badge, "badge-folder");
    gtk_widget_remove_css_class(badge, "badge-uri");
    gtk_widget_add_css_class(badge, type_badge_class(item->type));

    /* Update Preview Text */
    const gchar *preview = item->preview ? item->preview : "(empty)";
    if (is_first) {
        gchar *escaped = g_markup_escape_text(preview, -1);
        gchar *markup  = g_strdup_printf("<b>%s</b>", escaped);
        gtk_label_set_markup(GTK_LABEL(prev_label), markup);
        g_free(markup);
        g_free(escaped);
    } else {
        gtk_label_set_text(GTK_LABEL(prev_label), preview);
    }

    /* Update Timestamp */
    gchar *time_str = format_time(item->timestamp);
    gtk_label_set_text(GTK_LABEL(meta), time_str);
    g_free(time_str);

    /* Update Buttons' Data */
    g_object_set_data(G_OBJECT(copy_btn), "item-id", GINT_TO_POINTER((gint)item->id));
    
    gtk_button_set_label(GTK_BUTTON(star_btn), item->starred ? "★" : "☆");
    gtk_widget_set_tooltip_text(star_btn, item->starred ? "Unstar" : "Star");
    if (item->starred) gtk_widget_add_css_class(star_btn, "active");
    else gtk_widget_remove_css_class(star_btn, "active");
    g_object_set_data(G_OBJECT(star_btn), "item-id", GINT_TO_POINTER((gint)item->id));
    g_object_set_data(G_OBJECT(star_btn), "starred", GINT_TO_POINTER((gint)item->starred));
    
    g_object_set_data(G_OBJECT(del_btn), "item-id", GINT_TO_POINTER((gint)item->id));
}

static void execute_copy_and_close(ClipWindow *self, guint idx) {
    if (!self->current_items || idx >= self->current_items->len) return;
    ClipItem *item = g_ptr_array_index(self->current_items, idx);
    
    restore_item_to_clipboard(self, item);
    
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gtk_list_box_unselect_all(GTK_LIST_BOX(self->list_box));
    gtk_widget_grab_focus(self->search_entry);

    clip_window_refresh(self);
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
}

/* 2. List Box: Native signal for Mouse Click */
static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer user_data) {
    (void)lb;
    ClipWindow *self = CLIP_WINDOW(user_data);
    guint idx = (guint)gtk_list_box_row_get_index(row);
    execute_copy_and_close(self, idx);
}

/* Intercepts keys at the Window level BEFORE the Type-to-Search can steal them */
static gboolean on_global_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    ClipWindow *self = CLIP_WINDOW(user_data);

    /* Listen for Delete or Backspace */
    if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete || keyval == GDK_KEY_BackSpace) {
        
        /* Where is the user physically focused right now? */
        GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
        
        /* If the focus is currently inside the List Box, STEAL the key event! */
        if (focus && (focus == self->list_box || gtk_widget_is_ancestor(focus, self->list_box))) {
            
            GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_box));
            if (!row) return FALSE;

            guint idx = (guint)gtk_list_box_row_get_index(row);
            if (!self->current_items || idx >= self->current_items->len) return FALSE;

            /* Delete the item */
            ClipItem *item = g_ptr_array_index(self->current_items, idx);
            clip_database_delete(self->db, item->id);

            clip_window_refresh(self);

            /* Smart Focus: Jump to next item */
            GtkListBoxRow *next_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list_box), idx);
            if (!next_row && idx > 0) {
                next_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list_box), idx - 1);
            }

            if (next_row) {
                gtk_widget_grab_focus(GTK_WIDGET(next_row));
                gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), next_row);
            } else {
                gtk_widget_grab_focus(self->search_entry);
            }
            
            /* TRUE stops the event here. The search bar will never know Delete was pressed. */
            return TRUE; 
        }
    }
    return FALSE;
}

/* 3. Global Shortcut: ENTER (Wayland/IM Proof) */
static gboolean on_enter_shortcut(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget; (void)args;
    ClipWindow *self = CLIP_WINDOW(user_data);
    
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_box));
    guint idx = row ? (guint)gtk_list_box_row_get_index(row) : 0;
    
    execute_copy_and_close(self, idx);
    return TRUE;
}

/* 4. Global Shortcut: ESCAPE */
static gboolean on_esc_shortcut(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget; (void)args;
    ClipWindow *self = CLIP_WINDOW(user_data);
    
    /* Reset UI state before hiding to prevent Ghost State on next launch */
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gtk_list_box_unselect_all(GTK_LIST_BOX(self->list_box));
    gtk_widget_grab_focus(self->search_entry);
    
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
    return TRUE;
}

/* 5. Search Bar: Down Arrow to move focus */
static gboolean on_search_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)ctrl; (void)keycode; (void)state;
    ClipWindow *self = CLIP_WINDOW(user_data);
    
    if (keyval == GDK_KEY_Down) {
        GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_box));
        if (!row) row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list_box), 0);
        if (row) {
            gtk_widget_grab_focus(GTK_WIDGET(row));
            gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), row);
        }
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *build_row(ClipWindow *self, ClipItem *item, gboolean is_first) {
    GtkWidget *row     = gtk_list_box_row_new();
    GtkWidget *hbox    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "clip-row");
    if (item->starred) gtk_widget_add_css_class(row, "starred");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    /* Left: badge + preview */
    GtkWidget *vbox  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), vbox);

    /* Top row: badge + preview text */
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(vbox), top_row);

    GtkWidget *badge = gtk_label_new(type_label(item->type));
    gtk_widget_add_css_class(badge, "clip-type-badge");
    gtk_widget_add_css_class(badge, type_badge_class(item->type));
    gtk_box_append(GTK_BOX(top_row), badge);

    const gchar *preview = item->preview ? item->preview : "(empty)";
    GtkWidget *prev_label = gtk_label_new(NULL);
    
    if (is_first) {
        /* Highlight the current clipboard item using Pango markup */
        gchar *escaped = g_markup_escape_text(preview, -1);
        gchar *markup  = g_strdup_printf("<b>%s</b>", escaped);
        gtk_label_set_markup(GTK_LABEL(prev_label), markup);
        g_free(markup);
        g_free(escaped);
    } else {
        /* Standard text formatting for older items */
        gtk_label_set_text(GTK_LABEL(prev_label), preview);
    }

    gtk_widget_add_css_class(prev_label, "clip-preview");
    gtk_label_set_ellipsize(GTK_LABEL(prev_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(prev_label), 60);
    gtk_label_set_xalign(GTK_LABEL(prev_label), 0.0f);
    gtk_box_append(GTK_BOX(top_row), prev_label);

    /* Bottom: timestamp */
    gchar *time_str = format_time(item->timestamp);
    GtkWidget *meta = gtk_label_new(time_str);
    g_free(time_str);
    gtk_widget_add_css_class(meta, "clip-meta");
    gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
    gtk_box_append(GTK_BOX(vbox), meta);

    /* Right: action buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(hbox), btn_box);

    /* Copy button */
    GtkWidget *copy_btn = gtk_button_new_from_icon_name("edit-copy-symbolic");
    gtk_widget_add_css_class(copy_btn, "clip-action-btn");
    gtk_widget_set_tooltip_text(copy_btn, "Copy to clipboard");
    g_object_set_data(G_OBJECT(copy_btn), "item-id", GINT_TO_POINTER((gint)item->id));
    g_object_set_data(G_OBJECT(copy_btn), "window",  self);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_item), self);
    gtk_box_append(GTK_BOX(btn_box), copy_btn);

    /* Star button */
    GtkWidget *star_btn = gtk_button_new_with_label(item->starred ? "★" : "☆");
    gtk_widget_add_css_class(star_btn, "clip-action-btn");
    gtk_widget_add_css_class(star_btn, "star-btn");
    if (item->starred) gtk_widget_add_css_class(star_btn, "active");
    gtk_widget_set_tooltip_text(star_btn, item->starred ? "Unstar" : "Star");
    g_object_set_data(G_OBJECT(star_btn), "item-id",  GINT_TO_POINTER((gint)item->id));
    g_object_set_data(G_OBJECT(star_btn), "starred",  GINT_TO_POINTER((gint)item->starred));
    g_object_set_data(G_OBJECT(star_btn), "window",   self);
    g_signal_connect(star_btn, "clicked", G_CALLBACK(on_star_item), NULL);
    gtk_box_append(GTK_BOX(btn_box), star_btn);

    /* Delete button */
    GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(del_btn, "clip-action-btn");
    gtk_widget_set_tooltip_text(del_btn, "Delete");
    g_object_set_data(G_OBJECT(del_btn), "item-id", GINT_TO_POINTER((gint)item->id));
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_item), self);
    gtk_box_append(GTK_BOX(btn_box), del_btn);

    /* Cache internal widgets inside the row object for future recycling */
    g_object_set_data(G_OBJECT(row), "badge", badge);
    g_object_set_data(G_OBJECT(row), "prev_label", prev_label);
    g_object_set_data(G_OBJECT(row), "meta", meta);
    g_object_set_data(G_OBJECT(row), "copy_btn", copy_btn);
    g_object_set_data(G_OBJECT(row), "star_btn", star_btn);
    g_object_set_data(G_OBJECT(row), "del_btn", del_btn);

    return row;
}

/* ── signal handlers ─────────────────────────────────────────── */

/*
 * Custom GdkContentProvider that serves files in the two formats
 * Nautilus requires to enable "Paste":
 *
 *   1. x-special/gnome-copied-files  →  "copy\nfile:///path/one\nfile:///path/two\n"
 *   2. text/uri-list                  →  "file:///path/one\r\nfile:///path/two\r\n"
 *
 * Using gdk_clipboard_set_value(GDK_TYPE_FILE_LIST) alone is not
 * reliable across Wayland compositor/portal versions because it does
 * not always expose the gnome-copied-files MIME type that Nautilus
 * checks before enabling the Paste action.
 */

static void restore_item_to_clipboard(ClipWindow *self, ClipItem *item) {
    GdkDisplay   *display   = gdk_display_get_default();
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);

    if (item->type == CLIP_TYPE_IMAGE && item->blob) {
        GdkTexture *texture = gdk_texture_new_from_bytes(item->blob, NULL);
        if (texture) {
            gdk_clipboard_set_texture(clipboard, texture);
            g_object_unref(texture);
        }
    } 
    else if ((item->type == CLIP_TYPE_FILE || item->type == CLIP_TYPE_FOLDER) && item->text) {
        GSList *file_list = NULL;
        gchar **lines = g_strsplit(item->text, "\n", -1);
        
        for (gint i = 0; lines && lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);

            if (line[0] == '\0' || g_strcmp0(line, "copy") == 0 || g_strcmp0(line, "cut") == 0) continue;
            
            GFile *file = g_file_new_for_path(line);
            file_list = g_slist_prepend(file_list, file);
        }
        g_strfreev(lines);

        if (file_list) {
            file_list = g_slist_reverse(file_list);
            
            GValue value = G_VALUE_INIT;
            g_value_init(&value, GDK_TYPE_FILE_LIST);
            g_value_take_boxed(&value, gdk_file_list_new_from_list(file_list));
            
            gdk_clipboard_set_value(clipboard, &value);
            
            g_value_unset(&value);
            g_slist_free_full(file_list, g_object_unref);
        }
    } 
    else if (item->text) {
        gdk_clipboard_set_text(clipboard, item->text);
    }

    clip_database_touch(self->db, item->id);
}

static void on_copy_item(GtkButton *btn, gpointer user_data) {
    ClipWindow *self = CLIP_WINDOW(user_data);
    gint id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "item-id"));

    for (guint i = 0; i < self->current_items->len; i++) {
        ClipItem *item = g_ptr_array_index(self->current_items, i);
        if (item->id == (gint64)id) {
            restore_item_to_clipboard(self, item);
            clip_window_refresh(self);
            break;
        }
    }
}

static void on_delete_item(GtkButton *btn, gpointer user_data) {
    /* This handler is connected to delete button only */
    ClipWindow *self = CLIP_WINDOW(user_data);
    gint id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "item-id"));
    clip_database_delete(self->db, (gint64)id);
    clip_window_refresh(self);
}

static void on_star_toggled(GtkToggleButton *btn, gpointer user_data) {
    ClipWindow *self = CLIP_WINDOW(user_data);
    self->show_starred = gtk_toggle_button_get_active(btn);
    do_search(self);
}

/* Handles the cancellation of the clear history dialog */
static void on_clear_cancel(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    /* Find the root window of the clicked button and destroy it */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
    gtk_window_destroy(GTK_WINDOW(root));
}

/* Handles the confirmation of the clear history dialog */
static void on_clear_confirm(GtkButton *btn, gpointer user_data) {
    ClipWindow *self = CLIP_WINDOW(user_data);
    
    /* Clear the database and refresh the main UI */
    clip_database_clear(self->db);
    clip_window_refresh(self);
    
    /* Destroy the modal dialog */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
    gtk_window_destroy(GTK_WINDOW(root));
}

/* Triggers when the 'Clear history' button is clicked in the main window */
static void on_clear_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ClipWindow *self = CLIP_WINDOW(user_data);

    /* Build a custom modal window to replace GtkAlertDialog. 
     * This is required in pure GTK4 to apply custom CSS classes like "destructive-action" 
     * because GtkAlertDialog often uses out-of-process system portals.
     */
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), "Clear History");
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    
    /* Layout containers */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(vbox, 24);
    gtk_widget_set_margin_bottom(vbox, 24);
    gtk_widget_set_margin_start(vbox, 24);
    gtk_widget_set_margin_end(vbox, 24);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    /* Warning Label */
    GtkWidget *label = gtk_label_new("Clear all clipboard history?\nStarred items will be kept.");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(vbox), label);

    /* Button box */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), hbox);

    /* Cancel Button */
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_clear_cancel), NULL);
    gtk_box_append(GTK_BOX(hbox), btn_cancel);

    /* Destructive Clear Button */
    GtkWidget *btn_confirm = gtk_button_new_with_label("Clear");
    /* Apply the standard GTK red destructive styling */
    gtk_widget_add_css_class(btn_confirm, "destructive-action"); 
    g_signal_connect(btn_confirm, "clicked", G_CALLBACK(on_clear_confirm), self);
    gtk_box_append(GTK_BOX(hbox), btn_confirm);

    gtk_window_present(GTK_WINDOW(dialog));
}
