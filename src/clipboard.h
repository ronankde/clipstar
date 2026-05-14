#pragma once

#include "item.h"
#include <glib-object.h>

/* ClipMonitor watches both the CLIPBOARD and PRIMARY selections.
 * It emits "new-item" whenever fresh content is detected.
 */

#define CLIP_TYPE_MONITOR (clip_monitor_get_type())
G_DECLARE_FINAL_TYPE(ClipMonitor, clip_monitor, CLIP, MONITOR, GObject)

ClipMonitor *clip_monitor_new   (void);
void         clip_monitor_start (ClipMonitor *self);
void         clip_monitor_stop  (ClipMonitor *self);

/* signal: new-item(ClipMonitor*, ClipItem*, gpointer user_data) */
