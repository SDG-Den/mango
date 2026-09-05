#ifndef MANGO_GRADIENT_H
#define MANGO_GRADIENT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	float color[4];
	float degree;
} GradientStop;

typedef struct {
	GradientStop *stops;
	int stopcount;
} GradientBorder;

#endif