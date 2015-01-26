/*
 * Copyright (C) 2015 Endless Mobile
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

#include "wakefield-compositor.h"

#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <wayland-server.h>

struct WakefieldPointer
{
  struct wl_list resource_list;
  struct wl_resource *cursor_surface;
};

struct WakefieldSeat
{
  struct WakefieldPointer pointer;
  /* struct WakefieldKeyboard keyboard; */
};

struct WakefieldSurfacePendingState
{
  struct wl_resource *buffer;
  int scale;

  cairo_region_t *input_region;
  struct wl_list frame_callbacks;
};

struct WakefieldSurface
{
  WakefieldCompositor *compositor;

  struct wl_resource *resource;

  cairo_region_t *damage;
  struct WakefieldSurfacePendingState pending, current;
};

struct _WakefieldCompositorPrivate
{
  struct wl_display *wl_display;
  struct wl_client *client;
  int client_fd;

  struct WakefieldSurface *surface;
  struct WakefieldSeat seat;
};
typedef struct _WakefieldCompositorPrivate WakefieldCompositorPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (WakefieldCompositor, wakefield_compositor, GTK_TYPE_WIDGET);

/* Utility methods */

/* Generic implementation for the resource destructors */

static void
resource_release (struct wl_client *client,
                  struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static void
wakefield_compositor_realize (GtkWidget *widget)
{
  GtkAllocation allocation;
  GdkWindow *window;
  GdkWindowAttr attributes;
  gint attributes_mask;

  gtk_widget_set_realized (widget, TRUE);

  gtk_widget_get_allocation (widget, &allocation);

  attributes.x = allocation.x;
  attributes.y = allocation.y;
  attributes.visual = gtk_widget_get_visual (widget);
  attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL;

  attributes.width = allocation.width;
  attributes.height = allocation.height;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.event_mask = (GDK_POINTER_MOTION_MASK |
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_SCROLL_MASK |
                           GDK_FOCUS_CHANGE_MASK |
                           GDK_KEY_PRESS_MASK |
                           GDK_KEY_RELEASE_MASK |
                           GDK_ENTER_NOTIFY_MASK |
                           GDK_LEAVE_NOTIFY_MASK |
                           GDK_EXPOSURE_MASK);

  window = gdk_window_new (gtk_widget_get_parent_window (widget),
                           &attributes, attributes_mask);
  gtk_widget_set_window (widget, window);
  gtk_widget_register_window (widget, window);
}

static cairo_format_t
cairo_format_for_wl_shm_format (enum wl_shm_format format)
{
  switch (format)
    {
    case WL_SHM_FORMAT_ARGB8888:
      return CAIRO_FORMAT_ARGB32;
    case WL_SHM_FORMAT_XRGB8888:
      return CAIRO_FORMAT_RGB24;
    default:
      g_assert_not_reached ();
    }
}

static uint32_t
get_time (void)
{
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
draw_surface (cairo_t                 *cr,
              struct WakefieldSurface *surface)
{
  struct wl_shm_buffer *shm_buffer;

  shm_buffer = wl_shm_buffer_get (surface->current.buffer);
  if (shm_buffer)
    {
      cairo_surface_t *cr_surface;

      wl_shm_buffer_begin_access (shm_buffer);

      cr_surface = cairo_image_surface_create_for_data (wl_shm_buffer_get_data (shm_buffer),
                                                        cairo_format_for_wl_shm_format (wl_shm_buffer_get_format (shm_buffer)),
                                                        wl_shm_buffer_get_width (shm_buffer),
                                                        wl_shm_buffer_get_height (shm_buffer),
                                                        wl_shm_buffer_get_stride (shm_buffer));
      cairo_surface_set_device_scale (cr_surface, surface->current.scale, surface->current.scale);

      cairo_set_source_surface (cr, cr_surface, 0, 0);

      /* XXX: Do scaling of our surface to match our allocation. */
      cairo_paint (cr);

      cairo_surface_destroy (cr_surface);

      wl_shm_buffer_end_access (shm_buffer);
    }
  else
    g_assert_not_reached ();

  wl_buffer_send_release (surface->current.buffer);

  /* Trigger frame callbacks. */
  {
    struct wl_resource *cr;
    /* XXX: Should we use the frame clock for this? */
    uint32_t time = get_time ();

    wl_resource_for_each (cr, &surface->current.frame_callbacks)
      {
        wl_callback_send_done (cr, time);
      }

    wl_list_init (&surface->current.frame_callbacks);
  }
}

static gboolean
wakefield_compositor_draw (GtkWidget *widget,
                           cairo_t   *cr)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  if (priv->surface)
    draw_surface (cr, priv->surface);  

  return TRUE;
}

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

/* Break the surface and seat code out since it's getting too tricky */
#include "wakefield-surface.c"
#include "wakefield-seat.c"

static void
bind_output (struct wl_client *client,
             void *data,
             uint32_t version,
             uint32_t id)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (data);
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_output_interface, version, id);
  wl_resource_set_destructor (cr, unbind_resource);
  /* wl_list_insert (&priv->output_resources, cr); */
  wl_output_send_scale (cr, gtk_widget_get_scale_factor (GTK_WIDGET (compositor)));
}

#define WL_OUTPUT_VERSION 2

static void
wakefield_output_init (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  /* wl_list_init (&priv->output_resources); */
  wl_global_create (priv->wl_display, &wl_output_interface,
                    WL_OUTPUT_VERSION, compositor, bind_output);
}

static void
wakefield_compositor_class_init (WakefieldCompositorClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->realize = wakefield_compositor_realize;
  widget_class->draw = wakefield_compositor_draw;
  widget_class->enter_notify_event = wakefield_compositor_enter_notify_event;
  widget_class->leave_notify_event = wakefield_compositor_leave_notify_event;
  widget_class->button_press_event = wakefield_compositor_button_press_event;
  widget_class->button_release_event = wakefield_compositor_button_release_event;
  widget_class->motion_notify_event = wakefield_compositor_motion_notify_event;
}

static GSource * wayland_event_source_new (struct wl_display *display);

static void
wakefield_compositor_init (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  int fds[2];

  gtk_widget_set_has_window (GTK_WIDGET (compositor), TRUE);

  priv->wl_display = wl_display_create ();
  wl_display_init_shm (priv->wl_display);

  wakefield_surface_init (compositor);
  wakefield_seat_init (&priv->seat, priv->wl_display);
  wakefield_output_init (compositor);

  socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds);
  priv->client = wl_client_create (priv->wl_display, fds[0]);
  priv->client_fd = fds[1];

  /* XXX: For testing */
  wl_display_add_socket_auto (priv->wl_display);

  /* Attach the wl_event_loop to ours */
  g_source_attach (wayland_event_source_new (priv->wl_display), NULL);
}

int
wakefield_compositor_get_fd (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);

  return priv->client_fd;
}



/* Wayland GSource */

typedef struct
{
  GSource source;
  struct wl_display *display;
} WaylandEventSource;

static gboolean
wayland_event_source_prepare (GSource *base, int *timeout)
{
  WaylandEventSource *source = (WaylandEventSource *)base;

  *timeout = -1;

  wl_display_flush_clients (source->display);

  return FALSE;
}

static gboolean
wayland_event_source_dispatch (GSource *base,
                               GSourceFunc callback,
                               void *data)
{
  WaylandEventSource *source = (WaylandEventSource *)base;
  struct wl_event_loop *loop = wl_display_get_event_loop (source->display);

  wl_event_loop_dispatch (loop, 0);

  return TRUE;
}

static GSourceFuncs wayland_event_source_funcs =
{
  wayland_event_source_prepare,
  NULL,
  wayland_event_source_dispatch,
  NULL
};

static GSource *
wayland_event_source_new (struct wl_display *display)
{
  WaylandEventSource *source;
  struct wl_event_loop *loop = wl_display_get_event_loop (display);

  source = (WaylandEventSource *) g_source_new (&wayland_event_source_funcs,
                                                sizeof (WaylandEventSource));
  source->display = display;
  g_source_add_unix_fd (&source->source,
                        wl_event_loop_get_fd (loop),
                        G_IO_IN | G_IO_ERR);

  return &source->source;
}
