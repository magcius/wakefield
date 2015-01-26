/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include <wayland-client.h>

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	uint32_t formats;
};

struct buffer {
	struct wl_buffer *buffer;
	void *shm_data;
	int busy;
};

struct window {
	struct display *display;
	int width, height;
	struct wl_surface *surface;
	struct buffer buffers[2];
	struct buffer *prev_buffer;
	struct wl_callback *callback;
};

struct smoke {
	struct display *display;
	struct window *window;

	int width, height;
	int current;
	struct { float *d, *u, *v; } b[2];
};

static int running = 1;

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;

	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int
create_tmpfile_cloexec(char *tmpname)
{
	int fd;

	fd = mkostemp(tmpname, O_CLOEXEC);
	if (fd >= 0)
		unlink(tmpname);

	return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 *
 * If the C library implements posix_fallocate(), it is used to
 * guarantee that disk space is available for the file at the
 * given size. If disk space is insufficent, errno is set to ENOSPC.
 * If posix_fallocate() is not supported, program may receive
 * SIGBUS on accessing mmap()'ed file contents instead.
 */
static int
os_create_anonymous_file(off_t size)
{
	static const char template[] = "/weston-shared-XXXXXX";
	const char *path;
	char *name;
	int fd;
	int ret;

	path = getenv("XDG_RUNTIME_DIR");
	if (!path) {
		errno = ENOENT;
		return -1;
	}

	name = malloc(strlen(path) + sizeof(template));
	if (!name)
		return -1;

	strcpy(name, path);
	strcat(name, template);

	fd = create_tmpfile_cloexec(name);

	free(name);

	if (fd < 0)
		return -1;

	ret = posix_fallocate(fd, 0, size);
	if (ret != 0) {
		close(fd);
		errno = ret;
		return -1;
	}

	return fd;
}

static int
create_shm_buffer(struct display *display, struct buffer *buffer,
		  int width, int height, uint32_t format)
{
	struct wl_shm_pool *pool;
	int fd, size, stride;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n",
			size);
		return -1;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(display->shm, fd, size);
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0,
						   width, height,
						   stride, format);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	buffer->shm_data = data;

	return 0;
}

static struct window *
create_window(struct display *display, int width, int height)
{
	struct window *window;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->surface = wl_compositor_create_surface(display->compositor);

	return window;
}

static void
destroy_window(struct window *window)
{
	if (window->callback)
		wl_callback_destroy(window->callback);

	if (window->buffers[0].buffer)
		wl_buffer_destroy(window->buffers[0].buffer);
	if (window->buffers[1].buffer)
		wl_buffer_destroy(window->buffers[1].buffer);

	wl_surface_destroy(window->surface);
	free(window);
}

static struct buffer *
window_next_buffer(struct window *window)
{
	struct buffer *buffer;
	int ret = 0;

	if (!window->buffers[0].busy)
		buffer = &window->buffers[0];
	else if (!window->buffers[1].busy)
		buffer = &window->buffers[1];
	else
		return NULL;

	if (!buffer->buffer) {
		ret = create_shm_buffer(window->display, buffer,
					window->width, window->height,
					WL_SHM_FORMAT_XRGB8888);

		if (ret < 0)
			return NULL;
	}

	return buffer;
}

static void diffuse(struct smoke *smoke, uint32_t time,
		    float *source, float *dest)
{
	float *s, *d;
	int x, y, k, stride;
	float t, a = 0.0002;

	stride = smoke->width;

	for (k = 0; k < 5; k++) {
		for (y = 1; y < smoke->height - 1; y++) {
			s = source + y * stride;
			d = dest + y * stride;
			for (x = 1; x < smoke->width - 1; x++) {
				t = d[x - 1] + d[x + 1] +
					d[x - stride] + d[x + stride];
				d[x] = (s[x] + a * t) / (1 + 4 * a) * 0.995;
			}
		}
	}
}

static void advect(struct smoke *smoke, uint32_t time,
		   float *uu, float *vv, float *source, float *dest)
{
	float *s, *d;
	float *u, *v;
	int x, y, stride;
	int i, j;
	float px, py, fx, fy;

	stride = smoke->width;

	for (y = 1; y < smoke->height - 1; y++) {
		d = dest + y * stride;
		u = uu + y * stride;
		v = vv + y * stride;

		for (x = 1; x < smoke->width - 1; x++) {
			px = x - u[x];
			py = y - v[x];
			if (px < 0.5)
				px = 0.5;
			if (py < 0.5)
				py = 0.5;
			if (px > smoke->width - 1.5)
				px = smoke->width - 1.5;
			if (py > smoke->height - 1.5)
				py = smoke->height - 1.5;
			i = (int) px;
			j = (int) py;
			fx = px - i;
			fy = py - j;
			s = source + j * stride + i;
			d[x] = (s[0] * (1 - fx) + s[1] * fx) * (1 - fy) +
				(s[stride] * (1 - fx) + s[stride + 1] * fx) * fy;
		}
	}
}

static void project(struct smoke *smoke, uint32_t time,
		    float *u, float *v, float *p, float *div)
{
	int x, y, k, l, s;
	float h;

	h = 1.0 / smoke->width;
	s = smoke->width;
	memset(p, 0, smoke->height * smoke->width);
	for (y = 1; y < smoke->height - 1; y++) {
		l = y * s;
		for (x = 1; x < smoke->width - 1; x++) {
			div[l + x] = -0.5 * h * (u[l + x + 1] - u[l + x - 1] +
						 v[l + x + s] - v[l + x - s]);
			p[l + x] = 0;
		}
	}

	for (k = 0; k < 5; k++) {
		for (y = 1; y < smoke->height - 1; y++) {
			l = y * s;
			for (x = 1; x < smoke->width - 1; x++) {
				p[l + x] = (div[l + x] +
					    p[l + x - 1] +
					    p[l + x + 1] +
					    p[l + x - s] +
					    p[l + x + s]) / 4;
			}
		}
	}

	for (y = 1; y < smoke->height - 1; y++) {
		l = y * s;
		for (x = 1; x < smoke->width - 1; x++) {
			u[l + x] -= 0.5 * (p[l + x + 1] - p[l + x - 1]) / h;
			v[l + x] -= 0.5 * (p[l + x + s] - p[l + x - s]) / h;
		}
	}
}

static void render(struct smoke *smoke, struct buffer *buffer)
{
	int stride = smoke->width * 4;
	int32_t x, y;
	uint8_t *dest = (uint8_t *) buffer->shm_data;

	for (y = 1; y < smoke->height - 1; y++) {
		uint32_t *d;
		float *s;

		s = smoke->b[smoke->current].d + y * smoke->height;
		d = (uint32_t *) (dest + y * stride);
		for (x = 1; x < smoke->width - 1; x++) {
			int c, a;

			c = (int) (s[x] * 800);
			if (c > 255)
				c = 255;
			a = c;
			if (a < 0x33)
				a = 0x33;
			d[x] = (a << 24) | (c << 16) | (c << 8) | c;
		}
	}
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time)
{
	struct smoke *smoke = data;
	struct window *window = smoke->window;
	struct buffer *buffer;

	buffer = window_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
			!callback ? "Failed to create the first buffer.\n" :
			"Both buffers busy at redraw(). Server bug?\n");
		abort();
	}

	diffuse(smoke, time / 30, smoke->b[0].u, smoke->b[1].u);
	diffuse(smoke, time / 30, smoke->b[0].v, smoke->b[1].v);
	project(smoke, time / 30,
		smoke->b[1].u, smoke->b[1].v,
		smoke->b[0].u, smoke->b[0].v);
	advect(smoke, time / 30,
	       smoke->b[1].u, smoke->b[1].v,
	       smoke->b[1].u, smoke->b[0].u);
	advect(smoke, time / 30,
	       smoke->b[1].u, smoke->b[1].v,
	       smoke->b[1].v, smoke->b[0].v);
	project(smoke, time / 30,
		smoke->b[0].u, smoke->b[0].v,
		smoke->b[1].u, smoke->b[1].v);

	diffuse(smoke, time / 30, smoke->b[0].d, smoke->b[1].d);
	advect(smoke, time / 30,
	       smoke->b[0].u, smoke->b[0].v,
	       smoke->b[1].d, smoke->b[0].d);

	render(smoke, window_next_buffer(window));

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);

	if (callback)
		wl_callback_destroy(callback);

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, smoke);
	wl_surface_commit(window->surface);
	buffer->busy = 1;
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
smoke_handle_motion(struct smoke *smoke, wl_fixed_t sx, wl_fixed_t sy)
{
	double x = wl_fixed_to_double(sx);
	double y = wl_fixed_to_double(sy);
	int i, i0, i1, j, j0, j1, k, d = 5;

	if (x - d < 1)
		i0 = 1;
	else
		i0 = x - d;
	if (i0 + 2 * d > smoke->width - 1)
		i1 = smoke->width - 1;
	else
		i1 = i0 + 2 * d;

	if (y - d < 1)
		j0 = 1;
	else
		j0 = y - d;
	if (j0 + 2 * d > smoke->height - 1)
		j1 = smoke->height - 1;
	else
		j1 = j0 + 2 * d;

	for (i = i0; i < i1; i++)
		for (j = j0; j < j1; j++) {
			k = j * smoke->width + i;
			smoke->b[0].u[k] += 256 - (random() & 512);
			smoke->b[0].v[k] += 256 - (random() & 512);
			smoke->b[0].d[k] += 1;
		}
}

static void
pointer_enter(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface,
	      wl_fixed_t sx, wl_fixed_t sy)
{
	struct smoke *smoke = data;
	smoke_handle_motion(smoke, sx, sy);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer,
	      uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_motion(void *data, struct wl_pointer *pointer,
	       uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct smoke *smoke = data;
	smoke_handle_motion(smoke, sx, sy);
}

static void
pointer_button(void *data, struct wl_pointer *pointer,
	       uint32_t serial, uint32_t time,
	       uint32_t button, uint32_t state)
{
}

static void
pointer_axis(void *data, struct wl_pointer *pointer,
	     uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_enter,
	pointer_leave,
	pointer_motion,
	pointer_button,
	pointer_axis,
};

static void
listen_on_pointer(struct smoke *smoke)
{
	struct wl_pointer *pointer = wl_seat_get_pointer (smoke->display->seat);
	wl_pointer_add_listener(pointer, &pointer_listener, smoke);
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct display *d = data;

	d->formats |= (1 << format);
}

struct wl_shm_listener shm_listener = {
	shm_format
};

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry,
					 id, &wl_compositor_interface, 1);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry,
					  id, &wl_shm_interface, 1);
		wl_shm_add_listener(d->shm, &shm_listener, d);
	} else if (strcmp(interface, "wl_seat") == 0) {
		d->seat = wl_registry_bind(registry,
					   id, &wl_seat_interface, 4);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static struct display *
create_display(void)
{
	struct display *display;

	display = malloc(sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	display->display = wl_display_connect(NULL);
	assert(display->display);

	display->formats = 0;
	display->registry = wl_display_get_registry(display->display);
	wl_registry_add_listener(display->registry,
				 &registry_listener, display);
	wl_display_roundtrip(display->display);
	if (display->shm == NULL) {
		fprintf(stderr, "No wl_shm global\n");
		exit(1);
	}

	wl_display_roundtrip(display->display);

	/*
	 * Why do we need two roundtrips here?
	 *
	 * wl_display_get_registry() sends a request to the server, to which
	 * the server replies by emitting the wl_registry.global events.
	 * The first wl_display_roundtrip() sends wl_display.sync. The server
	 * first processes the wl_display.get_registry which includes sending
	 * the global events, and then processes the sync. Therefore when the
	 * sync (roundtrip) returns, we are guaranteed to have received and
	 * processed all the global events.
	 *
	 * While we are inside the first wl_display_roundtrip(), incoming
	 * events are dispatched, which causes registry_handle_global() to
	 * be called for each global. One of these globals is wl_shm.
	 * registry_handle_global() sends wl_registry.bind request for the
	 * wl_shm global. However, wl_registry.bind request is sent after
	 * the first wl_display.sync, so the reply to the sync comes before
	 * the initial events of the wl_shm object.
	 *
	 * The initial events that get sent as a reply to binding to wl_shm
	 * include wl_shm.format. These tell us which pixel formats are
	 * supported, and we need them before we can create buffers. They
	 * don't change at runtime, so we receive them as part of init.
	 *
	 * When the reply to the first sync comes, the server may or may not
	 * have sent the initial wl_shm events. Therefore we need the second
	 * wl_display_roundtrip() call here.
	 *
	 * The server processes the wl_registry.bind for wl_shm first, and
	 * the second wl_display.sync next. During our second call to
	 * wl_display_roundtrip() the initial wl_shm events are received and
	 * processed. Finally, when the reply to the second wl_display.sync
	 * arrives, it guarantees we have processed all wl_shm initial events.
	 *
	 * This sequence contains two examples on how wl_display_roundtrip()
	 * can be used to guarantee, that all reply events to a request
	 * have been received and processed. This is a general Wayland
	 * technique.
	 */

	if (!(display->formats & (1 << WL_SHM_FORMAT_XRGB8888))) {
		fprintf(stderr, "WL_SHM_FORMAT_XRGB32 not available\n");
		exit(1);
	}

	return display;
}

static void
destroy_display(struct display *display)
{
	if (display->shm)
		wl_shm_destroy(display->shm);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}

static void
signal_int(int signum)
{
	running = 0;
}

int
main(int argc, char **argv)
{
	struct sigaction sigint;
	struct smoke smoke;
	int ret = 0;
	int size;

	smoke.width = 250;
	smoke.height = 250;

	smoke.display = create_display();
	smoke.window = create_window(smoke.display, smoke.width, smoke.height);
	if (!smoke.window)
		return 1;

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND;
	sigaction(SIGINT, &sigint, NULL);

	size = smoke.height * smoke.width;
	smoke.b[0].d = calloc(size, sizeof(float));
	smoke.b[0].u = calloc(size, sizeof(float));
	smoke.b[0].v = calloc(size, sizeof(float));
	smoke.b[1].d = calloc(size, sizeof(float));
	smoke.b[1].u = calloc(size, sizeof(float));
	smoke.b[1].v = calloc(size, sizeof(float));
	smoke.current = 0;

	listen_on_pointer(&smoke);

	redraw(&smoke, NULL, 0);

	while (running && ret != -1)
		ret = wl_display_dispatch(smoke.display->display);

	fprintf(stderr, "simple-shm exiting\n");

	destroy_window(smoke.window);
	destroy_display(smoke.display);

	return 0;
}
