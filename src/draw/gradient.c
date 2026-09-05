#include "gradient.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>

#ifndef M_PI
#define M_PI 3.141592653589793238462643383279502884
#endif

struct gradient_image_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void gradient_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct gradient_image_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool gradient_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
												  uint32_t flags, void **data,
												  uint32_t *format, size_t *stride) {
	(void)flags;
	struct gradient_image_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void gradient_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl gradient_buffer_impl = {
	.destroy = gradient_buffer_destroy,
	.begin_data_ptr_access = gradient_buffer_begin_data_ptr_access,
	.end_data_ptr_access = gradient_buffer_end_data_ptr_access,
};

static struct wlr_buffer *gradient_render_canvas(const GradientBorder *gradient, int width, int height) {

    if (gradient == NULL || gradient->stopcount <= 0 || width <= 0 || height <= 0 )
        return NULL;

    struct gradient_image_buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL)
        return NULL;

    buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    wlr_buffer_init(&buf->base, &gradient_buffer_impl, width, height);

    cairo_t *create = cairo_create(buf->surface);

    double center_x = width / 2.0;
    double center_y = height / 2.0;
    double radius = (width > height ? width : height) / 2.0;

    cairo_pattern_t *pattern =
        cairo_pattern_create_radial(center_x, center_y, 0.0, center_x, center_y, radius);

    for (int index = 0; index < gradient->stopcount; index++) {
        double degree = gradient->stops[index].degree;
        double fraction = (degree / 360.0) + 0.25;
        if (fraction >= 1.0)
            fraction -= 1.0;
        cairo_pattern_add_color_stop_rgba(pattern, fraction, 
            gradient->stops[index].color[0],
            gradient->stops[index].color[1],
            gradient->stops[index].color[2],
            gradient->stops[index].color[3]);
        }
    cairo_rectangle(create, 0, 0, width, height);
    cairo_set_source(create, pattern);
    cairo_fill(create);

    cairo_pattern_destroy(pattern);
    cairo_destroy(create);

    return &buf->base;

}

static void cairo_rounded_rect(cairo_t *render, double x, double y, double w, double h, double r) {
    if (r <= 0.0) {
        cairo_rectangle(render, x, y, w, h);
        return;
    }
    cairo_new_sub_path(render);
    cairo_arc(render, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(render, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(render, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(render, x + r, y + r, r, M_PI, M_PI * 3.0 / 2.0);
    cairo_close_path(render);
}


static struct wlr_buffer *gradient_make_ring(struct wlr_buffer *canvas, int width, int height, int border, double corners) {
    if (canvas == NULL || width <= 0 || height <= 0 || border < 0)
        return NULL;

    struct gradient_image_buffer *source = wl_container_of(canvas, source, base);
    
    struct gradient_image_buffer *ring = calloc(1, sizeof(*ring));
    if ( ring == NULL)
        return NULL;
    
    ring->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    wlr_buffer_init(&ring->base, &gradient_buffer_impl, width, height);

    int canvas_width = cairo_image_surface_get_width(source->surface);
    int canvas_height = cairo_image_surface_get_height(source->surface);
    double horizontal_sample_offset = ((canvas_width - width) / 2.0) * -1.0;
    double vertical_sample_offset = ((canvas_height - height) / 2.0) * -1.0;


    cairo_t *render = cairo_create(ring->surface);

    cairo_set_source_surface(render, source->surface, horizontal_sample_offset, vertical_sample_offset);
    cairo_paint(render);

    cairo_set_operator(render, CAIRO_OPERATOR_CLEAR);
    cairo_rounded_rect(render, border, border, width - 2 * border, height - 2 * border, corners);
    cairo_fill(render);
    cairo_destroy(render);

    return &ring->base;
}

static void gradient_canvas_size(int *width, int *height) {
    *width = 0;
    *height = 0;

    Monitor *monitor;
    wl_list_for_each(monitor, &mons, link) {
        if (monitor->m.width > *width)
            *width = monitor->m.width;
        if (monitor->m.height > *height)
            *height = monitor->m.height;
    }

}

struct GradientCacheEntry {
    GradientBorder key;
    struct wlr_buffer *canvas;
    uint32_t use_count;
};

static struct GradientCacheEntry *gradient_cache = NULL;
static size_t gradient_cache_count = 0;
static size_t gradient_cache_cap = 0;

static bool gradient_key_is_equal(const GradientBorder *a, const GradientBorder *b) {
    if (a->stopcount != b->stopcount)
        return false;

    for  (int index = 0; index < a->stopcount; index++) {
        if (memcmp(a->stops[index].color, b->stops[index].color, sizeof(float) * 4) != 0)
            return false;
        if (a->stops[index].degree != b->stops[index].degree)
            return false;
    }
    return true;

}

static bool gradient_cache_store(const GradientBorder *gradient, struct wlr_buffer *canvas) {
    if (gradient_cache_count == gradient_cache_cap) {
        size_t new_cap = gradient_cache_cap ? gradient_cache_cap * 2 : 8;
        struct GradientCacheEntry *grown =
            realloc(gradient_cache, new_cap * sizeof(*grown));
        if (grown == NULL)
            return false;
        gradient_cache = grown;
        gradient_cache_cap = new_cap;

    }
    struct GradientCacheEntry *entry = &gradient_cache[gradient_cache_count];
    GradientStop *key_copy = malloc(gradient->stopcount * sizeof(GradientStop));
    if (key_copy == NULL)
        return false;
    memcpy(key_copy, gradient->stops, gradient->stopcount * sizeof(GradientStop));
    entry->key.stops = key_copy;
    entry->key.stopcount = gradient->stopcount;
    entry->canvas = canvas;
    entry->use_count = 0;

    gradient_cache_count++;
    return true;
}

struct wlr_buffer *get_gradient_texture(const GradientBorder *gradient) {
    if (gradient == NULL || gradient->stopcount <= 0)
        return NULL;

    for(size_t index = 0; index < gradient_cache_count; index++) {
        if (gradient_key_is_equal(&gradient_cache[index].key, gradient))
            return gradient_cache[index].canvas;
    }

    int width, height;
    gradient_canvas_size(&width, &height);

    struct wlr_buffer *canvas = gradient_render_canvas(gradient, width, height);
    if (canvas == NULL)
        return NULL;
    if (!gradient_cache_store(gradient, canvas)) {
        wlr_buffer_drop(canvas);
        return NULL;
    }
    return canvas;
}


static void gradient_use_count(void) {
    for (size_t index = 0; index < gradient_cache_count; index++)
        gradient_cache[index].use_count = 0;

    Client *client;
    wl_list_for_each(client, &clients, link) {
        for (size_t index = 0; index < gradient_cache_count; index++) {
        if (gradient_key_is_equal(&gradient_cache[index].key, &client->active_gradient))
            gradient_cache[index].use_count++;
        if (gradient_key_is_equal(&gradient_cache[index].key, &client->inactive_gradient))
            gradient_cache[index].use_count++;
        }
    }
}

static void gradient_collect_garbage(void) {
    gradient_use_count();
    size_t write = 0;

    for (size_t read = 0; read < gradient_cache_count; read++) {
        struct GradientCacheEntry *entry = &gradient_cache[read];

        if (entry->use_count == 0) {
            free(entry->key.stops);
            wlr_buffer_drop(entry->canvas);
            continue;
        }
        if (write != read)
            gradient_cache[write] = gradient_cache[read];
        write++;
    }
    gradient_cache_count = write;
}

static void gradient_cache_teardown(void) {
    for (size_t index = 0; index < gradient_cache_count; index++){
        free(gradient_cache[index].key.stops);
        wlr_buffer_drop(gradient_cache[index].canvas);
    }
    free(gradient_cache);
    gradient_cache = NULL;
    gradient_cache_count = 0;
    gradient_cache_cap = 0;
}