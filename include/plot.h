#ifndef PLOT_H
#define PLOT_H

#include <stdio.h>
#include "core.h"

typedef enum {
	PLOT_LINES,
	PLOT_STAIRS,
	PLOT_DOTS,
	PLOT_STYLE_COUNT
} PlotStyle;

extern const char *const PLOT_STYLES[PLOT_STYLE_COUNT + 1];

typedef struct {
  const double *x_data;
  const double *y_data;
  size_t        num_points;
  const char   *name;
	PlotStyle     style;
} PlotSeries;

typedef struct {
  const  PlotSeries *series;
  size_t             num_series;
  const  char       *title;
  const  char       *x_label;
  const  char       *y_label;
} PlotData;

typedef struct {
  double lo, hi;
  double step;
  size_t num_ticks;
} PlotAxis;

// May extend this in the future. At the moment
// we only support one axis
typedef struct {
  PlotAxis x, y;
} PlotLayout;

int plot_layout(const PlotData *data, PlotLayout *out);
void plot_braille(Arena *a, FILE *out, const PlotData *data,
		const PlotLayout *layout, size_t cols, size_t rows);
void plot_svg(FILE *out, const PlotData *data, const PlotLayout *layout,
							double width, double height);

#endif // !PLOT_H
