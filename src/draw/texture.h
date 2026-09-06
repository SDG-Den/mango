#ifndef MANGO_TEXTURE_H
#define MANGO_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

struct Client;
struct wlr_buffer;

typedef struct {
	float color[4];
	float degree;
} GradientStop;

typedef struct {
	GradientStop *stops;
	int stopcount;
} GradientBorder;

typedef enum {
	TEXTURE_GRADIENT,
	TEXTURE_STYLE_COUNT,
} TextureStyle;

// never initialize positionally or memcmp/memcpy this whole struct
// this will cause breakage if another field is ever added to this struct.
typedef struct {
	TextureStyle style;
	GradientBorder gradient;
	char *string;
} BorderTextureKey;

struct TextureOps {
	bool (*key_empty)(const BorderTextureKey *key);
	bool (*key_equal)(const BorderTextureKey *a, const BorderTextureKey *b);
	bool (*key_copy)(const BorderTextureKey *source,
					 BorderTextureKey *destination);
	void (*key_destroy)(BorderTextureKey *key);
	bool bypass_cache;
	struct wlr_buffer *(*render)(const BorderTextureKey *key,
								 struct Client *target);
};
void texture_style_register(TextureStyle style, struct TextureOps ops);
struct wlr_buffer *texture_cache_get(const BorderTextureKey *key,
									 struct Client *target, bool *from_cache);
void init_texture_system(void);
bool texture_style_bypasses_cache(TextureStyle style);

#endif
