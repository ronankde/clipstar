#include <gtk/gtk.h>
#include <glib.h>
#include "application.h"

int main(int argc, char *argv[]) {
    ClipStarApp *app = clip_star_app_new();
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
