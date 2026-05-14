#pragma once

#include "database.h"
#include <gtk/gtk.h>

#define CLIP_TYPE_WINDOW (clip_window_get_type())
G_DECLARE_FINAL_TYPE(ClipWindow, clip_window, CLIP, WINDOW, GtkApplicationWindow)

ClipWindow *clip_window_new          (GtkApplication *app, ClipDatabase *db);
void        clip_window_refresh      (ClipWindow *self);
void        clip_window_focus_search (ClipWindow *self);
