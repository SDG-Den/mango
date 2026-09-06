#include "texture.h"

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

struct texture_image_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

struct TextureCacheEntry {
	BorderTextureKey key;
	struct wlr_buffer *canvas;
	uint32_t use_count;
};

static struct TextureCacheEntry *texture_cache = NULL;
static size_t texture_cache_count = 0;
static size_t texture_cache_cap = 0;

// buffer handlers
static void texture_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct texture_image_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	cairo_surface_destroy(buf->surface);
	free(buf);
}
static bool texture_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
												 uint32_t flags, void **data,
												 uint32_t *format,
												 size_t *stride) {
	(void)flags;
	struct texture_image_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}
static void texture_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {}
static const struct wlr_buffer_impl texture_buffer_impl = {
	.destroy = texture_buffer_destroy,
	.begin_data_ptr_access = texture_buffer_begin_data_ptr_access,
	.end_data_ptr_access = texture_buffer_end_data_ptr_access,
};

// canvas max size helper
static void max_needed_canvas_size(int *width, int *height) {
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

// cairo/general border draw stuff
static void cairo_rounded_rect(cairo_t *render, double x, double y, double w,
							   double h, double r) {
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

static struct wlr_buffer *texture_make_ring(struct wlr_buffer *canvas,
											int width, int height, int border,
											double corners) {
	if (canvas == NULL || width <= 0 || height <= 0 || border < 0)
		return NULL;

	struct texture_image_buffer *source = wl_container_of(canvas, source, base);

	struct texture_image_buffer *ring = calloc(1, sizeof(*ring));
	if (ring == NULL)
		return NULL;

	ring->surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	wlr_buffer_init(&ring->base, &texture_buffer_impl, width, height);

	int canvas_width = cairo_image_surface_get_width(source->surface);
	int canvas_height = cairo_image_surface_get_height(source->surface);
	double horizontal_sample_offset = ((canvas_width - width) / 2.0) * -1.0;
	double vertical_sample_offset = ((canvas_height - height) / 2.0) * -1.0;

	cairo_t *render = cairo_create(ring->surface);

	cairo_set_source_surface(render, source->surface, horizontal_sample_offset,
							 vertical_sample_offset);
	cairo_paint(render);

	cairo_set_operator(render, CAIRO_OPERATOR_CLEAR);
	cairo_rounded_rect(render, border, border, width - 2 * border,
					   height - 2 * border, corners);
	cairo_fill(render);
	cairo_destroy(render);

	return &ring->base;
}

// gradient-specific functions (key empty check, key equal check, key copy, key
// destroy) to implement another renderer, these must be provided to the texture
// renderer using TextureOps

static bool gradient_key_empty(const BorderTextureKey *key) {
	return key->gradient.stopcount <= 0;
}

static bool gradient_key_equal(const BorderTextureKey *a,
							   const BorderTextureKey *b) {
	if (a->gradient.stopcount != b->gradient.stopcount)
		return false;

	for (int index = 0; index < a->gradient.stopcount; index++) {
		if (memcmp(a->gradient.stops[index].color,
				   b->gradient.stops[index].color, sizeof(float) * 4) != 0)
			return false;
		if (a->gradient.stops[index].degree != b->gradient.stops[index].degree)
			return false;
	}
	return true;
}

static bool gradient_key_copy(const BorderTextureKey *source,
							  BorderTextureKey *destination) {
	destination->style = source->style;
	destination->string = NULL;

	if (source->gradient.stopcount <= 0) {
		destination->gradient.stops = NULL;
		destination->gradient.stopcount = 0;
		return true;
	}

	destination->gradient.stops =
		malloc((size_t)source->gradient.stopcount * sizeof(GradientStop));
	if (destination->gradient.stops == NULL)
		return false;
	memcpy(destination->gradient.stops, source->gradient.stops,
		   (size_t)source->gradient.stopcount * sizeof(GradientStop));
	destination->gradient.stopcount = source->gradient.stopcount;
	return true;
}

static void gradient_key_destroy(BorderTextureKey *key) {
	free(key->gradient.stops);
	key->gradient.stops = NULL;
	key->gradient.stopcount = 0;
}

static struct wlr_buffer *texture_render_gradient(const BorderTextureKey *key,
												  Client *target) {
	(void)target;

	if (key == NULL || key->gradient.stopcount <= 0)
		return NULL;
	int width, height;
	max_needed_canvas_size(&width, &height);
	if (width <= 0 || height <= 0)
		return NULL;

	struct texture_image_buffer *buf = calloc(1, sizeof(*buf));
	if (buf == NULL)
		return NULL;

	buf->surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	wlr_buffer_init(&buf->base, &texture_buffer_impl, width, height);

	cairo_t *create = cairo_create(buf->surface);

	double center_x = width / 2.0;
	double center_y = height / 2.0;
	double radius = (width > height ? width : height) / 2.0;

	cairo_pattern_t *pattern = cairo_pattern_create_radial(
		center_x, center_y, 0.0, center_x, center_y, radius);

	for (int index = 0; index < key->gradient.stopcount; index++) {
		double degree = key->gradient.stops[index].degree;
		double fraction = (degree / 360.0) + 0.25;
		if (fraction >= 1.0)
			fraction -= 1.0;
		cairo_pattern_add_color_stop_rgba(pattern, fraction,
										  key->gradient.stops[index].color[0],
										  key->gradient.stops[index].color[1],
										  key->gradient.stops[index].color[2],
										  key->gradient.stops[index].color[3]);
	}
	cairo_rectangle(create, 0, 0, width, height);
	cairo_set_source(create, pattern);
	cairo_fill(create);

	cairo_pattern_destroy(pattern);
	cairo_destroy(create);

	return &buf->base;
}

// general texture engine
static struct TextureOps texture_styles[TEXTURE_STYLE_COUNT];

static struct TextureOps *texture_style_ops(TextureStyle style) {
	if (style >= TEXTURE_STYLE_COUNT)
		return NULL;
	return &texture_styles[style];
}

void texture_style_register(TextureStyle style, struct TextureOps ops) {
	struct TextureOps *slot = texture_style_ops(style);
	if (slot == NULL)
		return;
	*slot = ops;
}

bool texture_key_empty(const BorderTextureKey *key) {
	if (key == NULL)
		return true;
	struct TextureOps *ops = texture_style_ops(key->style);
	if (ops == NULL)
		return true;
	return ops->key_empty(key);
}

bool texture_key_equal(const BorderTextureKey *a, const BorderTextureKey *b) {
	if (a == b)
		return true;
	if (a == NULL || b == NULL)
		return false;
	if (a->style != b->style)
		return false;
	struct TextureOps *ops = texture_style_ops(a->style);
	if (ops == NULL)
		return false;
	return ops->key_equal(a, b);
}

bool texture_key_copy(const BorderTextureKey *source,
					  BorderTextureKey *destination) {
	if (source == NULL || destination == NULL)
		return false;
	struct TextureOps *ops = texture_style_ops(source->style);
	if (ops == NULL)
		return false;
	return ops->key_copy(source, destination);
}

void texture_key_destroy(BorderTextureKey *key) {
	if (key == NULL)
		return;
	struct TextureOps *ops = texture_style_ops(key->style);
	if (ops == NULL)
		return;
	ops->key_destroy(key);
}

bool texture_style_bypasses_cache(TextureStyle style) {
	struct TextureOps *ops = texture_style_ops(style);
	if (ops == NULL)
		return false;
	return ops->bypass_cache;
}

static bool texture_cache_store(const BorderTextureKey *key,
								struct wlr_buffer *canvas) {
	if (texture_cache_count == texture_cache_cap) {
		size_t new_cap = texture_cache_cap ? texture_cache_cap * 2 : 8;
		struct TextureCacheEntry *grown =
			realloc(texture_cache, new_cap * sizeof(*grown));
		if (grown == NULL)
			return false;
		texture_cache = grown;
		texture_cache_cap = new_cap;
	}
	struct TextureCacheEntry *entry = &texture_cache[texture_cache_count];
	if (!texture_key_copy(key, &entry->key))
		return false;
	entry->canvas = canvas;
	entry->use_count = 0;

	texture_cache_count++;
	return true;
}

struct wlr_buffer *texture_cache_get(const BorderTextureKey *key,
									 Client *target, bool *from_cache) {
	if (key == NULL || from_cache == NULL)
		return NULL;
	*from_cache = false;

	struct TextureOps *ops = texture_style_ops(key->style);
	if (ops == NULL)
		return NULL;

	if (ops->key_empty(key))
		return NULL;

	if (ops->bypass_cache)
		return ops->render(key, target);

	for (size_t index = 0; index < texture_cache_count; index++) {
		if (texture_key_equal(&texture_cache[index].key, key)) {
			*from_cache = true;
			return texture_cache[index].canvas;
		}
	}
	struct wlr_buffer *canvas = ops->render(key, target);
	if (canvas == NULL)
		return NULL;
	if (!texture_cache_store(key, canvas)) {
		wlr_buffer_drop(canvas);
		return NULL;
	}
	wlr_buffer_lock(canvas);
	return canvas;
}

static void texture_use_count(void) {
	for (size_t index = 0; index < texture_cache_count; index++)
		texture_cache[index].use_count = 0;

	Client *client;
	wl_list_for_each(client, &clients, link) {
		for (size_t index = 0; index < texture_cache_count; index++) {
			if (texture_key_equal(&texture_cache[index].key,
								  &client->active_texture))
				texture_cache[index].use_count++;
			if (texture_key_equal(&texture_cache[index].key,
								  &client->inactive_texture))
				texture_cache[index].use_count++;
		}
	}
}

static void texture_collect_garbage(void) {
	texture_use_count();
	size_t write = 0;

	for (size_t read = 0; read < texture_cache_count; read++) {
		struct TextureCacheEntry *entry = &texture_cache[read];

		if (entry->use_count == 0) {
			texture_key_destroy(&entry->key);
			wlr_buffer_drop(entry->canvas);
			continue;
		}
		if (write != read)
			texture_cache[write] = texture_cache[read];
		write++;
	}
	texture_cache_count = write;
}

static void texture_cache_teardown(void) {
	for (size_t index = 0; index < texture_cache_count; index++) {
		texture_key_destroy(&texture_cache[index].key);
		wlr_buffer_drop(texture_cache[index].canvas);
	}
	free(texture_cache);
	texture_cache = NULL;
	texture_cache_count = 0;
	texture_cache_cap = 0;
}

// register texture styles here

void init_texture_system(void) {
	struct TextureOps gradient_ops = {
		.key_empty = gradient_key_empty,
		.key_equal = gradient_key_equal,
		.key_copy = gradient_key_copy,
		.key_destroy = gradient_key_destroy,
		.bypass_cache = false,
		.render = texture_render_gradient,
	};
	texture_style_register(TEXTURE_GRADIENT, gradient_ops);
}