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

#define COMPOSITOR_VERSION 3

struct WakefieldRegion
{
  struct wl_resource *resource;
  cairo_region_t *region;
};

static void
wl_region_add (struct wl_client *client,
               struct wl_resource *resource,
               gint32 x,
               gint32 y,
               gint32 width,
               gint32 height)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };
  cairo_region_union_rectangle (region->region, &rectangle);
}

static void
wl_region_subtract (struct wl_client *client,
                    struct wl_resource *resource,
                    gint32 x,
                    gint32 y,
                    gint32 width,
                    gint32 height)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };
  cairo_region_subtract_rectangle (region->region, &rectangle);
}

static const struct wl_region_interface region_interface = {
  resource_release,
  wl_region_add,
  wl_region_subtract
};

static void
wl_region_destructor (struct wl_resource *resource)
{
  struct WakefieldRegion *region = wl_resource_get_user_data (resource);

  cairo_region_destroy (region->region);
  g_slice_free (struct WakefieldRegion, region);
}

static void
wl_compositor_create_region (struct wl_client *client,
                             struct wl_resource *compositor_resource,
                             uint32_t id)
{
  struct WakefieldRegion *region = g_slice_new0 (struct WakefieldRegion);

  region->resource = wl_resource_create (client, &wl_region_interface, wl_resource_get_version (compositor_resource), id);
  wl_resource_set_implementation (region->resource, &region_interface, region, wl_region_destructor);

  region->region = cairo_region_create ();
}

static void
wl_surface_attach (struct wl_client *client,
                   struct wl_resource *surface_resource,
                   struct wl_resource *buffer_resource,
                   gint32 dx, gint32 dy)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  /* Ignore dx/dy in our case */
  surface->pending.buffer = buffer_resource;
}

static void
wl_surface_damage (struct wl_client *client,
                   struct wl_resource *surface_resource,
                   int32_t x, int32_t y, int32_t width, int32_t height)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  cairo_rectangle_int_t rectangle = { x, y, width, height };
  cairo_region_union_rectangle (surface->damage, &rectangle);
}

#define WL_CALLBACK_VERSION 1

static void
wl_surface_frame (struct wl_client *client,
                  struct wl_resource *surface_resource,
                  uint32_t callback_id)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);

  struct wl_resource *callback = wl_resource_create (client, &wl_callback_interface,
                                                     WL_CALLBACK_VERSION, callback_id);
  wl_resource_set_destructor (callback, unbind_resource);
  wl_list_insert (&surface->pending.frame_callbacks, wl_resource_get_link (callback));
}

static void
wl_surface_set_opaque_region (struct wl_client *client,
                              struct wl_resource *surface_resource,
                              struct wl_resource *region_resource)
{
  /* XXX: Do we need this? */
}

static void
wl_surface_set_input_region (struct wl_client *client,
                             struct wl_resource *surface_resource,
                             struct wl_resource *region_resource)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (surface_resource);
  g_clear_pointer (&surface->pending.input_region, cairo_region_destroy);
  if (region_resource)
    {
      struct WakefieldRegion *region = wl_resource_get_user_data (region_resource);
      surface->pending.input_region = cairo_region_copy (region->region);
    }
}

static void
wl_surface_commit (struct wl_client *client,
                   struct wl_resource *resource)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (resource);

  if (surface->pending.buffer)
    surface->current.buffer = surface->pending.buffer;

  /* XXX: Should we reallocate / redraw the entire region if the buffer
   * scale changes? */
  if (surface->pending.scale > 0)
    surface->current.scale = surface->pending.scale;

  wl_list_insert_list (&surface->current.frame_callbacks,
                       &surface->pending.frame_callbacks);
  wl_list_init (&surface->pending.frame_callbacks);

  /* process damage */
  gtk_widget_queue_draw_region (GTK_WIDGET (surface->compositor), surface->damage);

  /* ... and then empty it */
  {
    cairo_rectangle_int_t nothing = { 0, 0, 0, 0 };
    cairo_region_intersect_rectangle (surface->damage, &nothing);
  }

  /* XXX: Stop leak when we start using the input region. */
  surface->pending.input_region = NULL;

  surface->pending.buffer = NULL;
  surface->pending.scale = 0;
}

static void
wl_surface_set_buffer_transform (struct wl_client *client,
                                 struct wl_resource *resource,
                                 int32_t transform)
{
  /* TODO */
}

static void
wl_surface_set_buffer_scale (struct wl_client *client,
                             struct wl_resource *resource,
                             int32_t scale)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (resource);
  surface->pending.scale = scale;
}

static void
destroy_pending_state (struct WakefieldSurfacePendingState *state)
{
  g_clear_pointer (&state->input_region, cairo_region_destroy);
}

static void
wl_surface_destructor (struct wl_resource *resource)
{
  struct WakefieldSurface *surface = wl_resource_get_user_data (resource);

  destroy_pending_state (&surface->pending);
  destroy_pending_state (&surface->current);

  /* XXX */
  {
    WakefieldCompositor *compositor = surface->compositor;
    WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
    priv->surface = NULL;
  }
}

static const struct wl_surface_interface surface_interface = {
  resource_release,
  wl_surface_attach,
  wl_surface_damage,
  wl_surface_frame,
  wl_surface_set_opaque_region,
  wl_surface_set_input_region,
  wl_surface_commit,
  wl_surface_set_buffer_transform,
  wl_surface_set_buffer_scale
};

static void
wl_compositor_create_surface (struct wl_client *client,
                              struct wl_resource *compositor_resource,
                              uint32_t id)
{
  WakefieldCompositor *compositor = wl_resource_get_user_data (compositor_resource);
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  struct WakefieldSurface *surface;

  /* XXX: For now, treat the first surface created as the proper
   * preview surface and leak all the rest until we get a
   * special Wakefield extension... */

  if (priv->surface)
    return;

  surface = g_slice_new0 (struct WakefieldSurface);
  surface->compositor = compositor;
  surface->damage = cairo_region_create ();

  surface->resource = wl_resource_create (client, &wl_surface_interface, wl_resource_get_version (compositor_resource), id);
  wl_resource_set_implementation (surface->resource, &surface_interface, surface, wl_surface_destructor);

  wl_list_init (&surface->pending.frame_callbacks);
  wl_list_init (&surface->current.frame_callbacks);

  surface->current.scale = 1;

  priv->surface = surface;
}

const static struct wl_compositor_interface compositor_interface = {
  wl_compositor_create_surface,
  wl_compositor_create_region
};

static void
bind_compositor (struct wl_client *client,
                 void *data,
                 uint32_t version,
                 uint32_t id)
{
  WakefieldCompositor *compositor = data;
  struct wl_resource *cr;

  cr = wl_resource_create (client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation (cr, &compositor_interface, compositor, NULL);
}

static void
wakefield_surface_init (WakefieldCompositor *compositor)
{
  WakefieldCompositorPrivate *priv = wakefield_compositor_get_instance_private (compositor);
  wl_global_create (priv->wl_display, &wl_compositor_interface, COMPOSITOR_VERSION, compositor, bind_compositor);
}
