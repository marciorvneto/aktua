#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "plot.h"

#define SVG_ATTR(fp, name, fmt, ...) (fprintf(fp, name "=\"" fmt "\" ", __VA_ARGS__))

// Same order as PlotStyle, so a choice's index is its enum value.
// Sized from the enum: a style without a string leaves a NULL that ends the list early.
const char *const PLOT_STYLES[PLOT_STYLE_COUNT + 1] = {
	"lines",
	"stairs",
	"dots",
	NULL
};

// ---- Helpers for Heckbert's algorithm ---

static double nice_num(double x, bool round_ticks){
  // Closest "nice" number times a power of 10
  int    exponent      = floor(log10(x));
  double fraction      = x / pow(10.0, exponent);
  double nice_fraction = 0;

  if(round_ticks){
    if(fraction < 1.5)       nice_fraction = 1.0;
    else if(fraction < 3.0)  nice_fraction = 2.0;
    else if(fraction < 7.0)  nice_fraction = 5.0;
    else                     nice_fraction = 10.0;
  }else{
    if(fraction <= 1.0)      nice_fraction = 1.0;
    else if(fraction <= 2.0) nice_fraction = 2.0;
    else if(fraction <= 5.0) nice_fraction = 5.0;
    else                     nice_fraction = 10.0;
  }
  
  return nice_fraction * pow(10, exponent);
}

static PlotAxis compute_axis(double min, double max, size_t target_ticks){
  if(target_ticks < 2) target_ticks = 2;
  if(!(isfinite(min) && isfinite(max))){
    min = 0;
    max = 1;
  }
  double scale = MAX(fabs(min), fabs(max));
  if(max - min <= 1e-12 * scale || max == min){
    double pad  =  (scale == 0) ? 1.0 : 0.1 * scale;
    min        -= pad;
    max        += pad;
  }

  double raw_range = nice_num(max - min, false);
  double raw_step  = nice_num(raw_range / (target_ticks - 1), true);

  double axis_min  = floor(min / raw_step + 1e-9) * raw_step;
  double axis_max  = ceil(max / raw_step - 1e-9)  * raw_step;

  size_t num_ticks = (size_t)round((axis_max - axis_min)/raw_step) + 1;

  PlotAxis axis = {
    .lo        = axis_min,
    .hi        = axis_max,
    .num_ticks = num_ticks,
    .step      = raw_step
    
  };

  return axis;
}

static PlotAxis compute_log_axis(double min, double max, size_t target_ticks){
  if(target_ticks < 2) target_ticks = 2;
  if(!(isfinite(min) && isfinite(max))){ min = 0; max = 1; }

  double lo = floor(min);
  double hi = ceil(max);
  if(lo == hi) hi = lo + 1; // Plot at least one decade

  double step = 1;
  if((hi - lo)/step + 1 > target_ticks){
    step = ceil((hi - lo) / (target_ticks - 1));
    lo   = floor(lo / step) * step;
    hi   = ceil(hi / step) * step;
  }

  size_t num_ticks = (size_t)round((hi - lo)/step) + 1;

  PlotAxis axis = {
    .lo        = lo,
    .hi        = hi,
    .num_ticks = num_ticks,
    .step      = step,
    .log       = true
  };
  return axis;
}

// ----- Braille-specific

#define TICK_LABEL_MAX 32

typedef struct {
	size_t  cols;
	size_t  rows;
	uint8_t *cells; // cols x rows
} BrailleCanvas;

// dot position     bit
// (0,0) (1,0)     0x01  0x08
// (0,1) (1,1)     0x02  0x10
// (0,2) (1,2)     0x04  0x20
// (0,3) (1,3)     0x40  0x80
static const uint8_t BRAILLE_BITS[4][2] = {
	{0x01, 0x08},
	{0x02, 0x10},
	{0x04, 0x20},
	{0x40, 0x80},
};

// px in [0, 2*cols), py in [0, 4*rows)
static void canvas_set(BrailleCanvas *c, long px, long py){
	if(px < 0 || py < 0) return;
	if((size_t)px >= 2 * c->cols || (size_t)py >= 4 * c->rows) return;
	c->cells[(py / 4) * c->cols + (px / 2)] |= BRAILLE_BITS[py % 4][px % 2];
}

// To UTF-8
static void put_braille(FILE *out, uint8_t bits){
	int cp = 0x2800 + bits;
	fputc(0xE0 | (cp >> 12),         out);
	fputc(0x80 | ((cp >> 6) & 0x3F), out);
	fputc(0x80 | (cp & 0x3F),        out);
}


static long to_px(double x, const PlotAxis *ax, size_t width){
	return lround((x - ax->lo) / (ax->hi - ax->lo) * (width - 1));
}

static long to_py(double y, const PlotAxis *ay, size_t height){
	return lround((ay->hi - y) / (ay->hi - ay->lo) * (height - 1));
}

// Maps the x coordinate of a point to a rectangle
// with top-left corner at (0,0) and given width
static double canvas_map_x(double x, const PlotAxis *ax, double width){
	// ax->lo   -->  0
	// x        -->  mapped_x (want this)
	// ax->hi   -->  width
	//--------------------
	// (x - ax->lo)/(ax->hi - ax->lo) = mapped_x / width
	return width * (x - ax->lo)/(ax->hi - ax->lo);
}

// Maps the y coordinate of a point to a rectangle
// with top-left corner at (0,0) and given height
static double canvas_map_y(double y, const PlotAxis *ay, double height){
	// ay->lo   -->  height 
	// y        -->  mapped_y (want this)
	// ay->hi   -->  0
	//--------------------
	// (y - ay->lo)/(ay->hi - ay->lo) = -(mapped_y - height) / height
	return height * (1.0 - (y - ay->lo)/(ay->hi - ay->lo));
}



// Bresenham
static void canvas_line(BrailleCanvas *c, long x0, long y0, long x1, long y1){
	long dx =  labs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	long dy = -labs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	long err = dx + dy; 
	for(;;){
		canvas_set(c, x0, y0);
		if(x0 == x1 && y0 == y1) break;
		long e2 = 2 * err;
		if(e2 >= dy){ err += dy; x0 += sx; }
		if(e2 <= dx){ err += dx; y0 += sy; }
	} 
}

static void canvas_svg_polyline_begin(
		FILE *out,
		const char *stroke,
		double stroke_width)
{

	fprintf(out, "<polyline ");
		SVG_ATTR(out, "fill",            "%s", "none");
		SVG_ATTR(out, "stroke",          "%s", stroke);
		SVG_ATTR(out, "stroke-width",    "%g", stroke_width);
		SVG_ATTR(out, "stroke-linejoin", "%s", "round");
		fprintf(out,  "points=\"");
}
static void canvas_svg_polyline_point(
		FILE *out,
		double x,
		double y)
{
		fprintf(out, " %.2f,%.2f",x,y);
}

static void canvas_svg_polyline_end(FILE *out)
{
		fprintf(out, "\" />\n");
}

static void canvas_svg_circle(FILE *out, double cx, double cy, double r, const char *fill)
{
	fprintf(out, "<circle ");
	SVG_ATTR(out, "cx",   "%.2f", cx);
	SVG_ATTR(out, "cy",   "%.2f", cy);
	SVG_ATTR(out, "r",    "%g",   r);
	SVG_ATTR(out, "fill", "%s",   fill);
	fprintf(out, "/>\n");
}

static void svg_write_text(FILE *out, const char *s){
	for(const char *p = s; *p; p++){
		switch(*p){
			case '<':
				fprintf(out, "&lt;");
				break;
			case '>':
				fprintf(out, "&gt;");
				break;
			case '&':
				fprintf(out, "&amp;");
				break;
			case '"':
				fprintf(out, "&quot;");
				break;
			default:
				fputc(*p, out);
		}
	}
}

// Number of decimals needed to print every multiple of step exactly
static int tick_decimals(double step){
  for(int d = 0; d < 10; d++){
    double scaled = step * pow(10.0, d);
    if(fabs(scaled - round(scaled)) < 1e-6 * scaled) return d;
  }
  return 10;
}

static const char *const SUPERSCRIPT[10] = { "⁰","¹","²","³","⁴","⁵","⁶","⁷","⁸","⁹" };
// Writes the label for tick value t: "0.25" on a linear axis, "10⁻⁴" on a log one
static void format_tick(const PlotAxis *axis, double t, char *buf, size_t size){
  if(axis->log){
    // The tick is always a whole number exponent
    long k = lround(t);
    long kabs = labs(k);

    char digits[24];
    snprintf(digits, sizeof(digits), "%ld", kabs);
    size_t len = 0;
    len += snprintf(buf + len, size - len, "10");
    if(k < 0) len += snprintf(buf + len, size - len, "⁻");
    for(char *d = digits; *d; d++){
      size_t idx = *d - '0';
      len += snprintf(buf + len, size - len, "%s", SUPERSCRIPT[idx]);
    }
    return;
  }
  snprintf(buf, size, "%.*f", tick_decimals(axis->step) ,t);
}

// k-th tick: the first multiple of step inside [lo, hi], then every step after
static double tick_value(const PlotAxis *axis, size_t k){
        double first = ceil(axis->lo / axis->step - 1e-9) * axis->step;
        double t     = first + k * axis->step;
        // Avoid printing "-0" for a tick that is zero up to rounding
        if(fabs(t) < 1e-9 * axis->step) t = 0.0;
        return t;
}

static size_t num_ticks(const PlotAxis *axis){
        double first = ceil(axis->lo / axis->step - 1e-9) * axis->step;
        if(first > axis->hi + 1e-9 * axis->step) return 0;
        return (size_t)floor((axis->hi - first) / axis->step + 1e-9) + 1;
}

static void braille_centered(FILE *out, size_t offset, size_t span, const char *text){
	size_t w   = utf8_width(text);
	size_t pad = offset + (w < span ? (span - w) / 2 : 0);
	fprintf(out, "%*s%s\n", (int)pad, "", text);
}


static void plot_braille_frame(
		Arena *a,
		FILE *out, 
		const PlotData *data,
		const BrailleCanvas *c, 
		const PlotAxis *ax, 
		const PlotAxis *ay
		)
{ 
	size_t scratch = arena_save(a);

	// ---- y labels: one optional label per character row ----

	char  (*row_label)[TICK_LABEL_MAX] = arena_alloc(a, c->rows * TICK_LABEL_MAX);
	memset(row_label, 0, c->rows * TICK_LABEL_MAX);

	size_t width  = 0;
	size_t ny     = num_ticks(ay);
	for(size_t k = 0; k < ny; k++){
		double t   = tick_value(ay, k);
		long   row = to_py(t, ay, 4 * c->rows) / 4;
		if(row < 0 || (size_t)row >= c->rows) continue;
    format_tick(ay, t, row_label[row], TICK_LABEL_MAX);
		width = MAX(width, utf8_width(row_label[row]));
	}

	// ---- Title ----

	if(data->title != NULL){
		braille_centered(out, width + 2, c->cols, data->title);
	}

	// ---- Y label ----

	if(data->y_label != NULL){
		fprintf(out, "%s\n", data->y_label);
	}


	// ---- Rows: label, axis, braille cells ----

	for(size_t r = 0; r < c->rows; r++){
		bool has_label = row_label[r][0] != '\0';
  fprintf(out, "%*s%s %s", (int)(width - utf8_width(row_label[r])), "", row_label[r], has_label ? "┤" : "│");
		for(size_t col = 0; col < c->cols; col++){
			put_braille(out, c->cells[r * c->cols + col]);
		}
		fputc('\n', out);
	}

	// ---- x axis line, with ┬ under each tick ----

	bool  *has_tick = arena_alloc(a, c->cols * sizeof(bool));
	memset(has_tick, 0, c->cols * sizeof(bool));

	size_t nx = num_ticks(ax);
	for(size_t k = 0; k < nx; k++){
		long col = to_px(tick_value(ax, k), ax, 2 * c->cols) / 2;
		if(col >= 0 && (size_t)col < c->cols) has_tick[col] = true;
	}

	fprintf(out, "%*s └", (int)width, "");
	for(size_t col = 0; col < c->cols; col++){
		fputs(has_tick[col] ? "┬" : "─", out);
	}
	fputc('\n', out);

	// ---- x labels, centered under their tick ----

	// Labels are ASCII, so one byte is one terminal column here
	size_t offset = width + 2;
  long cur      = 0;

	for(size_t k = 0; k < nx; k++){
		double t   = tick_value(ax, k);
		long   col = to_px(t, ax, 2 * c->cols) / 2;
		if(col < 0 || (size_t)col >= c->cols) continue;

		char label[TICK_LABEL_MAX];
    format_tick(ax, t, label, TICK_LABEL_MAX);
    size_t w   = (long)utf8_width(label);
		long start = (long)offset + col - w / 2;
    if(start < 0) start = 0;
    if(cur > 0 && start <= cur) continue; // would touch the previous label

    fprintf(out, "%*s%s", (int)(start - cur), "", label);
    cur = start + w;
	}
  fputc('\n', out);

	// ---- X label ----

	if(data->x_label != NULL){
		braille_centered(out, width + 2, c->cols, data->x_label);
	}


	arena_restore(a, scratch);
}

int plot_layout(const PlotData *data, PlotLayout *out){
	double x_min = INFINITY, x_max = -INFINITY;
	double y_min = INFINITY, y_max = -INFINITY;
	for(size_t s = 0; s < data->num_series; s++){
		const PlotSeries *ser = &data->series[s];
		for(size_t i = 0; i < ser->num_points; i++){
			double x = ser->x_data[i], y = ser->y_data[i];
			if(!isfinite(x) || !isfinite(y)) continue;
			x_min = MIN(x_min, x);  x_max = MAX(x_max, x);
			y_min = MIN(y_min, y);  y_max = MAX(y_max, y);
		}
	}
	if(x_min > x_max) return 0;
	out->x     = data->x_log ? compute_log_axis(x_min, x_max, 6) : compute_axis(x_min, x_max, 6);
	out->y     = data->y_log ? compute_log_axis(y_min, y_max, 5) : compute_axis(y_min, y_max, 5);
	return 1;
}


void plot_braille(Arena *a, FILE *out, const PlotData *data,
		const PlotLayout *layout, size_t cols, size_t rows){
	size_t scratch = arena_save(a);
	BrailleCanvas c = {
		.cols  = cols,
		.rows  = rows,
		.cells = arena_alloc(a, cols * rows),
	};

	for(size_t s = 0; s < data->num_series; s++){
		const PlotSeries *ser = &data->series[s];
		bool have_prev = false;
		long px_prev = 0, py_prev = 0;
		for(size_t i = 0; i < ser->num_points; i++){
			double x = ser->x_data[i], y = ser->y_data[i];
			if(!isfinite(x) || !isfinite(y)){ have_prev = false; continue; }
			long px = to_px(x, &layout->x, 2 * cols);
			long py = to_py(y, &layout->y, 4 * rows);
			if(ser->style == PLOT_DOTS){
				// A lone braille dot is hard to see, so draw a 2x2 block
				canvas_set(&c, px, py);     canvas_set(&c, px + 1, py);
				canvas_set(&c, px, py + 1); canvas_set(&c, px + 1, py + 1);
			}else if(have_prev){
				switch(ser->style){
					case PLOT_STAIRS:
						canvas_line(&c, px_prev, py_prev, px, py_prev);   // hold
						canvas_line(&c, px,      py_prev, px, py);        // then jump
						break;
					case PLOT_LINES:
					default:
						canvas_line(&c, px_prev, py_prev, px, py);
						break;
				}
			}else{
				canvas_set(&c, px, py);
			}
			px_prev = px; py_prev = py; have_prev = true;
		}
	}

	plot_braille_frame(a, out, data, &c, &layout->x, &layout->y);
	arena_restore(a, scratch);
}

// SVG

static const char *const PALETTE_COLORS[] = {
"#007589",
"#9a8202",
"#a74449",
"#005fd6",
"#9e5598",
"#3f842e",
};

void plot_svg(FILE *out, const PlotData *data, const PlotLayout *layout,
							double width, double height){

  double margin_left   = 48;
  double margin_right  = 24;
  double margin_top    = 16;
  double margin_bottom = 36;

	if(data->title   != NULL) margin_top    += 24;
	if(data->x_label != NULL) margin_bottom += 20;
	if(data->y_label != NULL) margin_left   += 20;

	// Legend measuring
	
	double legend_text_w = 0;
	bool has_legend      = false;
	for(size_t i = 0; i < data->num_series; i++){
		if(data->series[i].name){
			legend_text_w = MAX(legend_text_w, 7.0 * utf8_width(data->series[i].name)); // TODO: change the 7.0
			has_legend    = true;
		}
	}
	double swatch_w = 20.0;
	double legend_w = swatch_w + 6 + legend_text_w;
	if(has_legend) margin_right += 16 + legend_w;

	double rect_width  = width - margin_left - margin_right;
	double rect_height = height - margin_top - margin_bottom;

	fprintf(out, "<svg xmlns=\"http://www.w3.org/2000/svg\" ");
		SVG_ATTR(out, "width",       "%g",          width);
		SVG_ATTR(out, "height",      "%g",          height);
		SVG_ATTR(out, "viewBox",     "%g %g %g %g", 0.0, 0.0, width, height);
		SVG_ATTR(out, "font-family", "%s",          "system-ui, sans-serif");
		SVG_ATTR(out, "font-size",   "%g",          12.0);
		SVG_ATTR(out, "style",       "%s",          "font-variant-numeric: tabular-nums");
	fprintf(out, ">\n");

	// Plot rectangle background

	fprintf(out, "<rect ");
		SVG_ATTR(out, "x",      "%g", 0.0);
		SVG_ATTR(out, "y",      "%g", 0.0);
		SVG_ATTR(out, "width",  "%g", width);
		SVG_ATTR(out, "height", "%g", height);
		SVG_ATTR(out, "fill",   "%s", "#ffffff");
	fprintf(out, " />\n");


	// Plot the grid lines

	size_t nx = num_ticks(&layout->x);
	for(size_t k = 0; k < nx; k++){
		double t      = tick_value(&layout->x, k);
		double tick_x = canvas_map_x(t, &layout->x, rect_width) + margin_left;

		fprintf(out,  "<line ");
		SVG_ATTR(out, "stroke",          "%s", "#e0e0e0");
		SVG_ATTR(out, "shape-rendering", "%s", "crispEdges");
		SVG_ATTR(out, "x1",              "%g", tick_x);
		SVG_ATTR(out, "y1",              "%g", margin_top);
		SVG_ATTR(out, "x2",              "%g", tick_x);
		SVG_ATTR(out, "y2",              "%g", margin_top + rect_height);
		fprintf(out,  " />\n");
	}

	size_t ny = num_ticks(&layout->y);
	for(size_t k = 0; k < ny; k++){
		double t      = tick_value(&layout->y, k);
		double tick_y = canvas_map_y(t, &layout->y, rect_height) + margin_top;

		fprintf(out,  "<line ");
		SVG_ATTR(out, "stroke",          "%s", "#e0e0e0");
		SVG_ATTR(out, "shape-rendering", "%s", "crispEdges");
		SVG_ATTR(out, "x1",              "%g", margin_left);
		SVG_ATTR(out, "y1",              "%g", tick_y);
		SVG_ATTR(out, "x2",              "%g", margin_left + rect_width);
		SVG_ATTR(out, "y2",              "%g", tick_y);
		fprintf(out,  " />\n");
	}

	// Plot rectangle open frame

	fprintf(out,  "<line ");
	SVG_ATTR(out, "stroke",          "%s", "#333");
	SVG_ATTR(out, "shape-rendering", "%s", "crispEdges");
	SVG_ATTR(out, "x1",              "%g", margin_left);
	SVG_ATTR(out, "y1",              "%g", margin_top);
	SVG_ATTR(out, "x2",              "%g", margin_left);
	SVG_ATTR(out, "y2",              "%g", margin_top + rect_height);
	fprintf(out,  " />\n");

	fprintf(out,  "<line ");
	SVG_ATTR(out, "stroke",          "%s", "#333");
	SVG_ATTR(out, "shape-rendering", "%s", "crispEdges");
	SVG_ATTR(out, "x1",              "%g", margin_left);
	SVG_ATTR(out, "y1",              "%g", margin_top + rect_height);
	SVG_ATTR(out, "x2",              "%g", margin_left + rect_width);
	SVG_ATTR(out, "y2",              "%g", margin_top + rect_height);
	fprintf(out,  " />\n");


	// Plot the series

	for(size_t s = 0; s < data->num_series; s++){
		const PlotSeries *ser = &data->series[s];
		bool open             = false;
		double my_prev        = 0;

		for(size_t i = 0; i < ser->num_points; i++){
			const char *color = PALETTE_COLORS[s % LEN(PALETTE_COLORS)];
			double x = ser->x_data[i], y = ser->y_data[i];
			if(!isfinite(x) || !isfinite(y)){ 
				if(open) canvas_svg_polyline_end(out);
				open = false;
				continue; 
			}
			double mx = canvas_map_x(x, &layout->x, rect_width) + margin_left;
			double my = canvas_map_y(y, &layout->y, rect_height) + margin_top;

			if(ser->style == PLOT_DOTS){
				canvas_svg_circle(out, mx, my, 2, color);
				continue;
			}

			if(!open) {
				canvas_svg_polyline_begin(out, color, 2.0);
				canvas_svg_polyline_point(out, mx, my);
				open = true;
			}else{
				if(ser->style == PLOT_DOTS) continue;
				if(ser->style == PLOT_STAIRS){
					canvas_svg_polyline_point(out, mx, my_prev);
					canvas_svg_polyline_point(out, mx, my);
				}else if(ser->style == PLOT_LINES){
					canvas_svg_polyline_point(out, mx, my);
				}
			}

			my_prev = my;
		}
		if(open) canvas_svg_polyline_end(out);
	}

	// Plot the tick marks and labels

	nx = num_ticks(&layout->x);
	for(size_t k = 0; k < nx; k++){
		double t      = tick_value(&layout->x, k);
		int dec       = tick_decimals(layout->x.step);
		double tick_x = canvas_map_x(t, &layout->x, rect_width) + margin_left;

		fprintf(out,  "<line ");
		SVG_ATTR(out, "stroke",          "%s", "#333");
		SVG_ATTR(out, "shape-rendering", "%s", "crispEdges");
		SVG_ATTR(out, "x1",              "%g", tick_x);
		SVG_ATTR(out, "y1",              "%g", margin_top + rect_height);
		SVG_ATTR(out, "x2",              "%g", tick_x);
		SVG_ATTR(out, "y2",              "%g", margin_top + rect_height + 5);
		fprintf(out,  " />\n");

		fprintf(out, "<text ");
		SVG_ATTR(out, "fill",        "%s", "#555");
		SVG_ATTR(out, "text-anchor", "%s", "middle");
		SVG_ATTR(out, "x",           "%g", tick_x);
		SVG_ATTR(out, "y",           "%g", margin_top + rect_height + 18);
		fprintf(out,  " >");
		fprintf(out,  "%.*f", dec, t);
		fprintf(out,  "</text>\n");
	}

	ny = num_ticks(&layout->y);
	for(size_t k = 0; k < ny; k++){
		double t      = tick_value(&layout->y, k);
		int dec       = tick_decimals(layout->y.step);
		double tick_y = canvas_map_y(t, &layout->y, rect_height) + margin_top;

		fprintf(out,  "<line ");
		SVG_ATTR(out, "stroke",          "%s", "#333");
		SVG_ATTR(out, "shape-rendering", "%s", "crispEdges");
		SVG_ATTR(out, "x1",              "%g", margin_left);
		SVG_ATTR(out, "y1",              "%g", tick_y);
		SVG_ATTR(out, "x2",              "%g", margin_left - 5);
		SVG_ATTR(out, "y2",              "%g", tick_y);
		fprintf(out,  " />\n");

		fprintf(out, "<text ");
		SVG_ATTR(out, "fill",              "%s", "#555");
		SVG_ATTR(out, "text-anchor",       "%s", "end");
		SVG_ATTR(out, "dominant-baseline", "%s", "middle");
		SVG_ATTR(out, "x",                 "%g", margin_left - 8);
		SVG_ATTR(out, "y",                 "%g", tick_y);
		fprintf(out, " >");
		fprintf(out, "%.*f", dec, t);
		fprintf(out, "</text>\n");
	}

	// Title and labels

	if(data->title != NULL){
		fprintf(out, "<text ");
		SVG_ATTR(out, "x",                 "%g", margin_left + rect_width / 2);
		SVG_ATTR(out, "y",                 "%g", margin_top / 2.0);
		SVG_ATTR(out, "fill",              "%s", "#222");
		SVG_ATTR(out, "text-anchor",       "%s", "middle");
		SVG_ATTR(out, "dominant-baseline", "%s", "middle");
		fprintf(out, " >");
		svg_write_text(out, data->title);
		fprintf(out, "</text>\n");
	}

	if(data->x_label != NULL){
		fprintf(out, "<text ");
		SVG_ATTR(out, "x",                 "%g", margin_left + rect_width / 2);
		SVG_ATTR(out, "y",                 "%g", height - 10);
		SVG_ATTR(out, "fill",              "%s", "#555");
		SVG_ATTR(out, "text-anchor",       "%s", "middle");
		SVG_ATTR(out, "dominant-baseline", "%s", "middle");
		fprintf(out, " >");
		svg_write_text(out, data->x_label);
		fprintf(out, "</text>\n");
	}

	if(data->y_label != NULL){
		double x = 10;
		double y = margin_top + rect_height / 2.0;
		fprintf(out, "<text ");
		SVG_ATTR(out, "x",                 "%g", x);
		SVG_ATTR(out, "fill",              "%s", "#555");
		SVG_ATTR(out, "y",                 "%g", y);
		SVG_ATTR(out, "text-anchor",       "%s", "middle");
		SVG_ATTR(out, "dominant-baseline", "%s", "middle");
		SVG_ATTR(out, "transform", "rotate(-90, %g, %g)", x, y);
		fprintf(out, " >");
		svg_write_text(out, data->y_label);
		fprintf(out, "</text>\n");
	}

	// Legend
	if(has_legend){
		double row_h = 18;
		double x0    = margin_left + rect_width + 16;
		for(size_t i = 0; i < data->num_series; i++){
			double y = margin_top + i * row_h + row_h / 2;
			if(data->series[i].name){
				const char *color = PALETTE_COLORS[i % LEN(PALETTE_COLORS)];

				if(data->series[i].style == PLOT_DOTS){
					fprintf(out, "<circle ");
					SVG_ATTR(out, "cx",   "%g", x0 + swatch_w / 2);
					SVG_ATTR(out, "cy",   "%g", y);
					SVG_ATTR(out, "r",    "%g", 2.0);
					SVG_ATTR(out, "fill", "%s", color);
					fprintf(out, "/>\n");
				}else{
					fprintf(out, "<line ");
					SVG_ATTR(out, "x1",           "%g", x0);
					SVG_ATTR(out, "y1",           "%g", y);
					SVG_ATTR(out, "x2",           "%g", x0 + swatch_w);
					SVG_ATTR(out, "y2",           "%g", y);
					SVG_ATTR(out, "stroke",       "%s", color);
					SVG_ATTR(out, "stroke-width", "%g", 2.0);
					fprintf(out, "/>\n");
				}

				fprintf(out, "<text ");
				SVG_ATTR(out, "x",                 "%g", x0 + swatch_w + 6);
				SVG_ATTR(out, "fill",              "%s", "#555");
				SVG_ATTR(out, "y",                 "%g", y);
				SVG_ATTR(out, "dominant-baseline", "%s", "middle");
				fprintf(out, ">");
				svg_write_text(out, data->series[i].name);
				fprintf(out, "</text>\n");

			}
		}
	}


	fprintf(out, "</svg>\n");
}
