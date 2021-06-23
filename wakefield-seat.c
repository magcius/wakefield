/*
 * Copyright (C) 2015 Endless Mobile
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Written by:
 *     Jasper St. Pierre <jstpierre@mecheye.net>
 */

static void
pointer_set_cursor (struct wl_client *client,
                    struct wl_resource *resource,
                    uint32_t serial,
                    struct wl_resource *surface_resource,
                    int32_t x, int32_t y)
{
  g_warning ("TODO: Set cursor\n");
}

static const struct wl_pointer_interface pointer_interface = {
  pointer_set_cursor,
  resource_release,
};

static void
seat_get_pointer (struct wl_client    *client,
                  struct wl_resource  *seat_resource,
                  uint32_t             id)
{
  struct WakefieldSeat *seat = wl_resource_get_user_data (seat_resource);
  struct WakefieldPointer *pointer = &seat->pointer;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_pointer_interface, wl_resource_get_version (seat_resource), id);
  wl_resource_set_implementation (cr, &pointer_interface, pointer, unbind_resource);
  wl_list_insert (&pointer->resource_list, wl_resource_get_link (cr));
}

static void
broadcast_button (GtkWidget      *widget,
                  GdkEventButton *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *resource;
  uint32_t serial = wl_display_next_serial (priv->wl_display);
  uint32_t button;

  /* XXX: Convert to evdev */
  button = event->button;

  wl_resource_for_each (resource, &priv->seat.pointer.resource_list)
    {
      wl_pointer_send_button (resource, serial,
                              event->time,
                              button,
                              (event->type == GDK_BUTTON_PRESS ? 1 : 0));
    }
}

static gboolean
wakefield_compositor_button_press_event (GtkWidget      *widget,
                                         GdkEventButton *event)
{
  broadcast_button (widget, event);
  return TRUE;
}

static gboolean
wakefield_compositor_button_release_event (GtkWidget      *widget,
                                           GdkEventButton *event)
{
  broadcast_button (widget, event);
  return TRUE;
}

static gboolean
wakefield_compositor_motion_notify_event (GtkWidget      *widget,
                                          GdkEventMotion *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *resource;

  wl_resource_for_each (resource, &priv->seat.pointer.resource_list)
    {
      wl_pointer_send_motion (resource,
                              event->time,
                              wl_fixed_from_int (event->x),
                              wl_fixed_from_int (event->y));
    }

  return FALSE;
}

static gboolean
wakefield_compositor_enter_notify_event (GtkWidget        *widget,
                                         GdkEventCrossing *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *resource;
  uint32_t serial = wl_display_next_serial (priv->wl_display);

  wl_resource_for_each (resource, &priv->seat.pointer.resource_list)
    {
      wl_pointer_send_enter (resource, serial,
                             priv->surface->resource,
                             wl_fixed_from_int (event->x),
                             wl_fixed_from_int (event->y));
    }

  return FALSE;
}

static gboolean
wakefield_compositor_leave_notify_event (GtkWidget        *widget,
                                         GdkEventCrossing *event)
{
  WakefieldCompositor *compositor = WAKEFIELD_COMPOSITOR (widget);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct wl_resource *resource;
  uint32_t serial = wl_display_next_serial (priv->wl_display);

  wl_resource_for_each (resource, &priv->seat.pointer.resource_list)
    {
      wl_pointer_send_leave (resource, serial, priv->surface->resource);
    }

  return FALSE;
}

static void
wakefield_pointer_init (struct WakefieldPointer *pointer)
{
  wl_list_init (&pointer->resource_list);
  pointer->cursor_surface = NULL;
}

#define SEAT_VERSION 4

static const struct wl_seat_interface seat_interface = {
  seat_get_pointer,
  NULL, /* get_keyboard */
  NULL, /* get_touch */
};

static void
bind_seat (struct wl_client *client,
           void *data,
           uint32_t version,
           uint32_t id)
{
  struct WaylandSeat *seat = data;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_seat_interface, version, id);
  wl_resource_set_implementation (cr, &seat_interface, seat, unbind_resource);
  wl_seat_send_capabilities (cr, WL_SEAT_CAPABILITY_POINTER);
  wl_seat_send_name (cr, "seat0");
}

static void
wakefield_seat_init (struct WakefieldSeat *seat,
                     struct wl_display    *wl_display)
{
  wakefield_pointer_init (&seat->pointer);
  /* wakefield_keyboard_init (&seat->keyboard); */

  wl_global_create (wl_display, &wl_seat_interface, SEAT_VERSION, seat, bind_seat);
}
