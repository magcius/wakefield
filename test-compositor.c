
#include <gtk/gtk.h>
#include "wakefield-compositor.h"

int
main (int argc, char **argv)
{
  gtk_init (&argc, &argv);

  GtkWidget *window, *compositor;

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  compositor = g_object_new (WAKEFIELD_TYPE_COMPOSITOR, NULL);

  gtk_container_add (GTK_CONTAINER (window), compositor);
  gtk_widget_show_all (window);
  gtk_main ();

  return 0;
}

