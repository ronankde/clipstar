#include "clipboard.h"
#include "item.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>

/*
 * ClipMonitor — frontend monitor.
 *
 * All clipboard capture and database insertion is handled exclusively
 * by clipstar-daemon.  The frontend only reads from the database to
 * display history and writes back to the clipboard on restore.
 * This object is kept as a stub so the rest of the codebase compiles
 * unchanged; clip_monitor_start/stop are no-ops.
 */

struct _ClipMonitor {
    GObject   parent_instance;
    gboolean  running;
};

enum { SIGNAL_NEW_ITEM, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(ClipMonitor, clip_monitor, G_TYPE_OBJECT)

static void clip_monitor_class_init(ClipMonitorClass *klass) {
    signals[SIGNAL_NEW_ITEM] =
        g_signal_new("new-item",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void clip_monitor_init(ClipMonitor *self) { self->running = FALSE; }

ClipMonitor *clip_monitor_new(void)          { return g_object_new(CLIP_TYPE_MONITOR, NULL); }
void         clip_monitor_start(ClipMonitor *self) { self->running = TRUE;  }
void         clip_monitor_stop (ClipMonitor *self) { self->running = FALSE; }
