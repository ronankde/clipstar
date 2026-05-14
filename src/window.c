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
    GtkWidget *btn_clear;
    GtkWidget *status_label;
    GtkWidget *scrolled;

    /* state */
    guint      search_timeout;
    GPtrArray *current_items;  /* items currently displayed */
};

G_DEFINE_TYPE(ClipWindow, clip_window, GTK_TYPE_APPLICATION_WINDOW)

/* ── forward declarations ────────────────────────────────────── */
static void     populate_list              (ClipWindow *self, GPtrArray *items);
static void     do_search                  (ClipWindow *self);
static gboolean search_debounce_cb         (gpointer user_data);
static void     on_search_changed          (GtkSearchEntry *entry, gpointer user_data);
static void     on_row_activated           (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data);
static void     on_clear_clicked           (GtkButton *btn, gpointer user_data);
static void     on_delete_item             (GtkButton *btn, gpointer user_data);
static void     on_window_show             (GtkWidget *widget, gpointer user_data);
static void     execute_copy_and_close     (ClipWindow *self, guint idx);

static void     restore_item_to_clipboard  (ClipWindow *self, ClipItem *item);

static gboolean on_enter_shortcut      (GtkWidget *widget, GVariant *args, gpointer user_data);
static gboolean on_esc_shortcut        (GtkWidget *widget, GVariant *args, gpointer user_data);
static gboolean on_search_key_pressed  (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static gboolean on_global_key_pressed  (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);

static GtkWidget *build_row                 (ClipWindow *self, ClipItem *item, gboolean is_first);
static void      update_status              (ClipWindow *self, guint count);
static void      update_row                 (GtkWidget *row, ClipItem *item, gboolean is_first);

/* ── CSS ─────────────────────────────────────────────────────── */
static const gchar *APP_CSS =
    "window.clip-window {"
    "  background: transparent;"
    "  font-family: Adwaita Sans, sans-serif;"
    "  font-size: 11pt;"
    "}"

    ".main-container {"
    "  background-color: #242424;"
    "  border-radius: 20px;"
    "  border: 1px solid rgba(255,255,255,0.06);"

    "  box-shadow:"
    "    0 1px 2px rgba(0,0,0,0.14),"
    "    0 4px 12px rgba(0,0,0,0.18);"

    "  margin: 18px;"
    "}"

    ".clip-header {"
    "  background-color: #2b2b2b;"
    "  border-bottom: 1px solid rgba(255,255,255,0.05);"
    "  padding: 14px;"
    "  border-top-left-radius: 20px;"
    "  border-top-right-radius: 20px;"
    "}"

    ".clip-row {"
    "  padding: 10px 14px;"
    "  border-bottom: 1px solid rgba(255,255,255,0.04);"
    "  background-color: transparent;"
    "}"

    ".clip-row:hover,"
    ".clip-row:selected {"
    "  background-color: rgba(255,255,255,0.06);"
    "}"

    ".clip-preview {"
    "  color: #ffffff;"
    "  font-family: monospace;"
    "  font-size: 11pt;"
    "}"

    ".clip-meta,"
    ".status-label,"
    ".empty-page {"
    "  color: #a0a0a0;"
    "}"

    ".clip-search {"
    "  border-radius: 14px;"
    "  background-color: #363636;"
    "  color: #ffffff;"
    "  border: 1px solid rgba(255,255,255,0.08);"
    "  padding: 9px 14px;"
    "  outline: none;"
    "  box-shadow: none;"
    "}"

    ".clip-search selection {"
    "  background-color: #3584e4;"
    "  color: #ffffff;"
    "}"

    ".clip-search text selection {"
    "  background-color: #3584e4;"
    "  color: #ffffff;"
    "}"
  
    ".clip-search:focus {"
    "  border-color: rgba(120,174,237,0.55);"
    "  box-shadow: 0 0 0 2px rgba(120,174,237,0.12);"
    "}"

    ".clip-type-badge {"
    "  border-radius: 999px;"
    "  padding: 2px 8px;"
    "  font-size: 8pt;"
    "  font-weight: 700;"
    "  color: white;"
    "}"

    /* cores estilo GNOME/libadwaita */
    ".badge-text   { background-color: #3584e4; }"
    ".badge-image  { background-color: #33d17a; }"
    ".badge-file   { background-color: #ff7800; }"
    ".badge-folder { background-color: #f6d32d; color: #202020; }"
    ".badge-uri    { background-color: #9141ac; }"

    ".clip-action-btn {"
    "  min-width: 30px;"
    "  min-height: 30px;"
    "  border-radius: 10px;"
    "  background: transparent;"
    "  border: none;"
    "  color: #b0b0b0;"
    "}"

    ".clip-action-btn:hover {"
    "  background-color: rgba(255,255,255,0.06);"
    "}"

    ".clear-btn {"
    "  color: #ff7b63;"
    "  border-radius: 14px;"
    "  border: 1px solid rgba(255,255,255,0.08);"
    "  background: rgba(255,255,255,0.03);"
    "  padding: 6px 12px;"
    "}"

    ".clear-btn:hover {"
    "  background: rgba(255,255,255,0.07);"
    "}"

    "scrollbar {"
    "  background: transparent;"
    "  border: none;"
    "  box-shadow: none;"
    "}"

    "scrollbar slider {"
    "  background-color: rgba(255,255,255,0.12);"
    "  border-radius: 999px;"
    "  min-width: 5px;"
    "  border: none;"
    "}"

    "scrollbar slider:hover {"
    "  background-color: rgba(255,255,255,0.20);"
    "}"

    "list,"
    "listbox,"
    "scrolledwindow {"
    "  background: transparent;"
    "  border: none;"
    "}"

    /* dialog clear history */
    ".clear-dialog {"
    "  background-color: #242424;"
    "  border-radius: 20px;"
    "  border: 1px solid rgba(255,255,255,0.06);"
    "}"

    ".clear-dialog-box {"
    "  padding: 24px;"
    "}"

    ".clear-dialog-title {"
    "  color: #ffffff;"
    "  font-size: 13pt;"
    "  font-weight: 700;"
    "}"

    ".clear-dialog-subtitle {"
    "  color: #a0a0a0;"
    "  font-size: 10pt;"
    "}"

    ".dialog-btn {"
    "  border-radius: 12px;"
    "  padding: 8px 18px;"
    "  border: 1px solid rgba(255,255,255,0.08);"
    "  background: #363636;"
    "  color: white;"
    "}"

    ".dialog-btn:hover {"
    "  background: #404040;"
    "}"

    ".dialog-btn-destructive {"
    "  background: #c01c28;"
    "  color: white;"
    "  border: none;"
    "}"

    ".dialog-btn-destructive:hover {"
    "  background: #e01b24;"
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
    gtk_window_set_title(GTK_WINDOW(self), "ClipStar");
    gtk_window_set_default_size(GTK_WINDOW(self), 480, 600);
    gtk_window_set_decorated(GTK_WINDOW(self), FALSE);
    gtk_window_set_deletable(GTK_WINDOW(self), FALSE);

    g_signal_connect(self, "show", G_CALLBACK(on_window_show), self);
    
    /* Global Shortcuts */
    GtkEventController *shortcut_ctrl = gtk_shortcut_controller_new();
    gtk_event_controller_set_propagation_phase(shortcut_ctrl, GTK_PHASE_CAPTURE);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                         gtk_callback_action_new(on_esc_shortcut, self, NULL)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Return, 0),
                         gtk_callback_action_new(on_enter_shortcut, self, NULL)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctrl),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_KP_Enter, 0),
                         gtk_callback_action_new(on_enter_shortcut, self, NULL)));
    gtk_widget_add_controller(GTK_WIDGET(self), shortcut_ctrl);
    
    GtkEventController *global_key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(global_key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(global_key_ctrl, "key-pressed", G_CALLBACK(on_global_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), global_key_ctrl);

    /* ── layout ───────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(vbox, "main-container");
    gtk_window_set_child(GTK_WINDOW(self), vbox);
    gtk_widget_set_overflow(GTK_WIDGET(vbox), GTK_OVERFLOW_HIDDEN);
    
    /* header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(header, "clip-header");
    gtk_box_append(GTK_BOX(vbox), header);

    /* search row */
    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(header), search_row);

    self->search_entry = gtk_search_entry_new();
    gtk_widget_add_css_class(self->search_entry, "clip-search");
    gtk_widget_set_hexpand(self->search_entry, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->search_entry),
                                          "Search history…");
    gtk_box_append(GTK_BOX(search_row), self->search_entry);
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);

    /* Clear History Button */
    self->btn_clear = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
    gtk_widget_add_css_class(self->btn_clear, "clear-btn");
    gtk_widget_set_tooltip_text(self->btn_clear, "Clear all history");
    gtk_box_append(GTK_BOX(search_row), self->btn_clear);
    g_signal_connect(self->btn_clear, "clicked", G_CALLBACK(on_clear_clicked), self);

    /* Key capture for search */
    GtkEventController *search_key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(search_key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(search_key_ctrl, "key-pressed", G_CALLBACK(on_search_key_pressed), self);
    gtk_widget_add_controller(self->search_entry, search_key_ctrl);
    gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(self->search_entry), GTK_WIDGET(self));

    /* status */
    self->status_label = gtk_label_new("");
    gtk_widget_add_css_class(self->status_label, "status-label");
    gtk_widget_set_halign(self->status_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), self->status_label);

    /* stack */
    self->stack = gtk_stack_new();
    gtk_widget_set_vexpand(self->stack, TRUE);
    gtk_box_append(GTK_BOX(vbox), self->stack);

    /* results */
    self->scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    self->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list_box), GTK_SELECTION_BROWSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scrolled), self->list_box);
    gtk_stack_add_named(GTK_STACK(self->stack), self->scrolled, "results");
    g_signal_connect(self->list_box, "row-activated", G_CALLBACK(on_row_activated), self);

    /* empty */
    GtkWidget *empty_label = gtk_label_new("No clipboard items yet.\nCopy something!");
    gtk_widget_add_css_class(empty_label, "empty-page");
    gtk_label_set_justify(GTK_LABEL(empty_label), GTK_JUSTIFY_CENTER);
    gtk_stack_add_named(GTK_STACK(self->stack), empty_label, "empty");
}

/* ── public API ──────────────────────────────────────────────── */
ClipWindow *clip_window_new(GtkApplication *app, ClipDatabase *db) {
    ClipWindow *self = g_object_new(CLIP_TYPE_WINDOW, "application", app, NULL);
    self->db = db;
    clip_window_refresh(self);
    return self;
}

void clip_window_refresh(ClipWindow *self) { do_search(self); }
void clip_window_focus_search(ClipWindow *self) { gtk_widget_grab_focus(self->search_entry); }

void clip_window_focus_list(ClipWindow *self) {
    gtk_list_box_unselect_all(GTK_LIST_BOX(self->list_box));

    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(self->scrolled));

    gtk_adjustment_set_value(adj, 0.0);

    GtkListBoxRow *row =
        gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(self->list_box), 0);

    if (row) {
        gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), row);
        gtk_widget_grab_focus(GTK_WIDGET(row));
    } else {
        gtk_widget_grab_focus(self->search_entry);
    }
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
    if (self->search_timeout) g_source_remove(self->search_timeout);
    self->search_timeout = g_timeout_add(SEARCH_DEBOUNCE_MS, search_debounce_cb, self);
}

static void do_search(ClipWindow *self) {
    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    GPtrArray *items;
    if (query && query[0] != '\0') items = clip_database_search(self->db, query, RESULTS_LIMIT);
    else items = clip_database_recent(self->db, RESULTS_LIMIT);

    if (self->current_items) g_ptr_array_free(self->current_items, TRUE);
    self->current_items = items;
    populate_list(self, items);
    update_status(self, items->len);
}

static void update_status(ClipWindow *self, guint count) {
    gchar *txt = (count == 0) ? g_strdup("No items") : (count == 1) ? g_strdup("1 item") : g_strdup_printf("%u items", count);
    gtk_label_set_text(GTK_LABEL(self->status_label), txt);
    g_free(txt);
}

static void populate_list(ClipWindow *self, GPtrArray *items) {
    GtkWidget *row_widget = gtk_widget_get_first_child(self->list_box);

    if (!items || items->len == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "empty");
        while (row_widget != NULL) {
            GtkWidget *next = gtk_widget_get_next_sibling(row_widget);
            gtk_list_box_remove(GTK_LIST_BOX(self->list_box), row_widget);
            row_widget = next;
        }
        return;
    }
    
    gtk_stack_set_visible_child_name(GTK_STACK(self->stack), "results");
    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));
    gboolean is_default_view = (query == NULL || query[0] == '\0');

    for (guint i = 0; i < items->len; i++) {
        ClipItem *item = g_ptr_array_index(items, i);
        gboolean is_first = (is_default_view && i == 0);
        if (row_widget != NULL) {
            update_row(row_widget, item, is_first);
            row_widget = gtk_widget_get_next_sibling(row_widget);
        } else {
            gtk_list_box_append(GTK_LIST_BOX(self->list_box), build_row(self, item, is_first));
        }
    }
    while (row_widget != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(row_widget);
        gtk_list_box_remove(GTK_LIST_BOX(self->list_box), row_widget);
        row_widget = next;
    }
}

/* ── row builder ─────────────────────────────────────────────── */

static gchar *format_time(gint64 ts) {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 diff = now - ts;
    if (diff < 60) return g_strdup("just now");
    if (diff < 3600) return g_strdup_printf("%lld min ago", (long long)(diff / 60));
    if (diff < 86400) return g_strdup_printf("%lld hr ago", (long long)(diff / 3600));
    if (diff < 86400 * 7) return g_strdup_printf("%lld days ago",(long long)(diff / 86400));
    GDateTime *dt = g_date_time_new_from_unix_local(ts);
    gchar *s = g_date_time_format(dt, "%b %d %Y");
    g_date_time_unref(dt);
    return s;
}

static const gchar *type_badge_class(ClipItemType t) {
    switch (t) {
        case CLIP_TYPE_IMAGE: return "badge-image";
        case CLIP_TYPE_FILE: return "badge-file";
        case CLIP_TYPE_FOLDER: return "badge-folder";
        case CLIP_TYPE_URI: return "badge-uri";
        default: return "badge-text";
    }
}

static const gchar *type_label(ClipItemType t) {
    switch (t) {
        case CLIP_TYPE_TEXT: return "TEXT";
        case CLIP_TYPE_IMAGE: return "IMG";
        case CLIP_TYPE_FILE: return "FILE";
        case CLIP_TYPE_FOLDER: return "FOLDER";
        case CLIP_TYPE_URI: return "URI";
        default: return "?";
    }
}

static void update_row(GtkWidget *row, ClipItem *item, gboolean is_first) {
    GtkWidget *badge      = g_object_get_data(G_OBJECT(row), "badge");
    GtkWidget *prev_label = g_object_get_data(G_OBJECT(row), "prev_label");
    GtkWidget *meta       = g_object_get_data(G_OBJECT(row), "meta");
    GtkWidget *del_btn    = g_object_get_data(G_OBJECT(row), "del_btn");

    /* Swap badge CSS class: only touch what changed */
    const gchar *old_cls = g_object_get_data(G_OBJECT(badge), "badge-class");
    const gchar *new_cls = type_badge_class(item->type);
    if (old_cls && g_strcmp0(old_cls, new_cls) != 0)
        gtk_widget_remove_css_class(badge, old_cls);
    gtk_widget_add_css_class(badge, new_cls);
    g_object_set_data(G_OBJECT(badge), "badge-class", (gpointer)new_cls);
    gtk_label_set_text(GTK_LABEL(badge), type_label(item->type));

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

    gchar *time_str = format_time(item->timestamp);
    gtk_label_set_text(GTK_LABEL(meta), time_str);
    g_free(time_str);

    gint64 *id_ptr = g_object_get_data(G_OBJECT(del_btn), "item-id");
    if (id_ptr) *id_ptr = item->id;
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

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer user_data) {
    (void)lb;
    execute_copy_and_close(CLIP_WINDOW(user_data), (guint)gtk_list_box_row_get_index(row));
}

static gboolean on_global_key_pressed(GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)ctrl; (void)keycode;
    ClipWindow *self = CLIP_WINDOW(user_data);
    
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
    gboolean in_list = focus && (focus == self->list_box || gtk_widget_is_ancestor(focus, self->list_box));

    if (in_list) {
        /* 1. Ao digitar caracteres normais na lista, foca na barra de pesquisa.
         * A verificação 'state' garante que comandos como Ctrl+C continuem funcionando. */
        if ((state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) == 0) {
            guint32 unicode = gdk_keyval_to_unicode(keyval);
            if (unicode != 0 && g_unichar_isprint(unicode)) {
                gtk_widget_grab_focus(self->search_entry);
                return FALSE; /* Retorna FALSE para o evento chegar no search_entry e a letra ser digitada */
            }
        }

        /* 2. O Backspace também redireciona o foco de volta à barra suavemente. */
        if (keyval == GDK_KEY_BackSpace) {
            gtk_widget_grab_focus(self->search_entry);
            return FALSE;
        }

        /* 3. Apenas as teclas Delete removem o item selecionado. */
        if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_KP_Delete) {
            GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_box));
            if (!row) return FALSE;
            
            guint idx = (guint)gtk_list_box_row_get_index(row);
            if (!self->current_items || idx >= self->current_items->len) return FALSE;
            
            ClipItem *item = g_ptr_array_index(self->current_items, idx);
            clip_database_delete(self->db, item->id);
            clip_window_refresh(self);
            
            GtkListBoxRow *next_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list_box), idx);
            if (!next_row && idx > 0) next_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->list_box), idx - 1);
            if (next_row) {
                gtk_widget_grab_focus(GTK_WIDGET(next_row));
                gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), next_row);
            } else gtk_widget_grab_focus(self->search_entry);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_enter_shortcut(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget; (void)args;
    ClipWindow *self = CLIP_WINDOW(user_data);
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_box));
    execute_copy_and_close(self, row ? (guint)gtk_list_box_row_get_index(row) : 0);
    return TRUE;
}

static gboolean on_esc_shortcut(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget; (void)args;
    ClipWindow *self = CLIP_WINDOW(user_data);
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gtk_list_box_unselect_all(GTK_LIST_BOX(self->list_box));
    gtk_widget_grab_focus(self->search_entry);
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
    return TRUE;
}

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
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(row, "clip-row");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_append(GTK_BOX(hbox), vbox);

    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(vbox), top_row);

    GtkWidget *badge = gtk_label_new(type_label(item->type));
    gtk_widget_add_css_class(badge, "clip-type-badge");
    gtk_widget_add_css_class(badge, type_badge_class(item->type));
    gtk_box_append(GTK_BOX(top_row), badge);

    const gchar *preview = item->preview ? item->preview : "(empty)";
    GtkWidget *prev_label = gtk_label_new(NULL);
    if (is_first) {
        gchar *escaped = g_markup_escape_text(preview, -1);
        gchar *markup = g_strdup_printf("<b>%s</b>", escaped);
        gtk_label_set_markup(GTK_LABEL(prev_label), markup);
        g_free(markup); g_free(escaped);
    } else gtk_label_set_text(GTK_LABEL(prev_label), preview);

    gtk_widget_add_css_class(prev_label, "clip-preview");
    gtk_label_set_ellipsize(GTK_LABEL(prev_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(prev_label), 60);
    gtk_label_set_xalign(GTK_LABEL(prev_label), 0.0f);
    gtk_box_append(GTK_BOX(top_row), prev_label);

    gchar *time_str = format_time(item->timestamp);
    GtkWidget *meta = gtk_label_new(time_str);
    g_free(time_str);
    gtk_widget_add_css_class(meta, "clip-meta");
    gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
    gtk_box_append(GTK_BOX(vbox), meta);

    GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(del_btn, "clip-action-btn");
    gtk_widget_set_tooltip_text(del_btn, "Delete");
    gint64 *id_ptr = g_new(gint64, 1);
    *id_ptr = item->id;
    g_object_set_data_full(G_OBJECT(del_btn), "item-id", id_ptr, g_free);
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_item), self);
    gtk_box_append(GTK_BOX(hbox), del_btn);

    g_object_set_data(G_OBJECT(row), "badge", badge);
    g_object_set_data(G_OBJECT(row), "prev_label", prev_label);
    g_object_set_data(G_OBJECT(row), "meta", meta);
    g_object_set_data(G_OBJECT(row), "del_btn", del_btn);

    return row;
}

static void restore_item_to_clipboard(ClipWindow *self, ClipItem *item) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    if (item->type == CLIP_TYPE_IMAGE && item->blob) {
        GdkTexture *texture = gdk_texture_new_from_bytes(item->blob, NULL);
        if (texture) { gdk_clipboard_set_texture(clipboard, texture); g_object_unref(texture); }
    } else if ((item->type == CLIP_TYPE_FILE || item->type == CLIP_TYPE_FOLDER) && item->text) {
        GSList *file_list = NULL;
        gchar **lines = g_strsplit(item->text, "\n", -1);
        for (gint i = 0; lines && lines[i]; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (line[0] == '\0' || !g_strcmp0(line, "copy") || !g_strcmp0(line, "cut")) continue;
            file_list = g_slist_prepend(file_list, g_file_new_for_path(line));
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
    } else if (item->text) gdk_clipboard_set_text(clipboard, item->text);
    clip_database_touch(self->db, item->id);
}

static void on_delete_item(GtkButton *btn, gpointer user_data) {
    ClipWindow *self = CLIP_WINDOW(user_data);
    gint64 *id_ptr = g_object_get_data(G_OBJECT(btn), "item-id");
    if (!id_ptr) return;
    clip_database_delete(self->db, *id_ptr);
    clip_window_refresh(self);
}

static void on_clear_cancel(GtkButton *btn, gpointer user_data) { 
    (void)user_data;
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)))); 
}

static void on_clear_confirm(GtkButton *btn, gpointer user_data) {
    ClipWindow *self = CLIP_WINDOW(user_data);
    clip_database_clear(self->db);
    clip_window_refresh(self);
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))));
}

static void on_clear_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;

    ClipWindow *self = CLIP_WINDOW(user_data);

    GtkWidget *dialog = gtk_window_new();

    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    gtk_widget_add_css_class(dialog, "clear-dialog");

    GtkWidget *outer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer_box, "clear-dialog-box");

    gtk_window_set_child(GTK_WINDOW(dialog), outer_box);

    GtkWidget *title = gtk_label_new("Clear clipboard history?");
    gtk_widget_add_css_class(title, "clear-dialog-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

    gtk_box_append(GTK_BOX(outer_box), title);

    GtkWidget *subtitle =
        gtk_label_new("This action will permanently remove all stored clipboard items.");

    gtk_widget_add_css_class(subtitle, "clear-dialog-subtitle");
    gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);

    gtk_widget_set_margin_top(subtitle, 8);

    gtk_box_append(GTK_BOX(outer_box), subtitle);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttons, 24);

    gtk_box_append(GTK_BOX(outer_box), buttons);

    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(btn_cancel, "dialog-btn");

    g_signal_connect(btn_cancel,
                     "clicked",
                     G_CALLBACK(on_clear_cancel),
                     NULL);

    gtk_box_append(GTK_BOX(buttons), btn_cancel);

    GtkWidget *btn_confirm = gtk_button_new_with_label("Clear");

    gtk_widget_add_css_class(btn_confirm, "dialog-btn");
    gtk_widget_add_css_class(btn_confirm, "dialog-btn-destructive");

    g_signal_connect(btn_confirm,
                     "clicked",
                     G_CALLBACK(on_clear_confirm),
                     self);

    gtk_box_append(GTK_BOX(buttons), btn_confirm);

    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_window_show(GtkWidget *widget, gpointer user_data) {
    (void)widget;

    ClipWindow *self = CLIP_WINDOW(user_data);

    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    clip_window_refresh(self);
    gtk_list_box_unselect_all(GTK_LIST_BOX(self->list_box));

    GtkAdjustment *adj =
        gtk_scrolled_window_get_vadjustment(
            GTK_SCROLLED_WINDOW(self->scrolled));

    gtk_adjustment_set_value(adj, 0.0);

    GtkListBoxRow *row =
        gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(self->list_box), 0);

    if (row) {
        gtk_list_box_select_row(GTK_LIST_BOX(self->list_box), row);
        gtk_widget_grab_focus(GTK_WIDGET(row));
    } else {
        gtk_widget_grab_focus(self->search_entry);
    }
}
