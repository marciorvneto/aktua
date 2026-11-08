#include <ctype.h>
#include <complex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <float.h>
#include <sys/types.h>

#define DISPLAY_SIG 6

#define LEN(x) sizeof((x))/sizeof((x)[0])
#define POLY_CANCEL_TOL (64 * DBL_EPSILON)
#define ALIGNMENT 16 	// Might change that in the future
#define ALIGN_ALLOC(offset, align) ((offset + ((align) - 1)) & (~((align) - 1))) // Alignment trick
#define ENV_INITIAL_VARS 64


#define UNREACHABLE(x) do { fprintf(stderr, "Unreachable: " x); exit(1); } while(0);
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define MIN(x,y) ((x) < (y) ? (x) : (y))

#define GCD_TOL 1e-9
#define ABERTH_TOL 1e-12
#define ABERTH_MAX_ITER 1000

#define ARGS_VARIADIC SIZE_MAX

//============================== 
//
//    Arena
//
//============================== 

typedef struct {
	char *base;
	size_t offset;
	size_t capacity;
} Arena;

Arena arena_create(size_t capacity){
	Arena a;
	a.base     = malloc(capacity);
	a.offset   = 0;
	a.capacity = capacity;
	return a;
}

void arena_destroy(Arena *a){
	free(a->base);
}

void *arena_alloc(Arena *a, size_t size){
	size_t aligned_offset = ALIGN_ALLOC(a->offset, ALIGNMENT);
	if(aligned_offset + size > a->capacity){
		fprintf(stderr, "Arena capacity exceeded. Currently at %zu, requested %zu\n", a->offset, size);
		return NULL;
	}
	a->offset = aligned_offset + size;
	memset(a->base + aligned_offset, 0, size);
	return a->base + aligned_offset;
}

size_t arena_save(Arena *a){
	return a->offset;
}

void arena_restore(Arena *a, size_t offset){
	a->offset = offset;
}

#define CONCAT_INTERNAL(x,y) x##y
#define CONCAT(x, y) CONCAT_INTERNAL(x, y)
#define SCRATCH_SCOPE(arena) for(size_t CONCAT(scratch_, __LINE__) = arena_save(arena), _once = 1; _once == 1; arena_restore(arena, CONCAT(scratch_, __LINE__)), _once = 0)

//============================== 
//
//    Formatting
//
//============================== 


int format_number(char *buf, size_t n, double x, int sig){
  if(isnan(x)) return snprintf(buf, n, "nan");
  if(isinf(x)) return snprintf(buf, n, x > 0 ? "inf" : "-inf");
  if(x == 0)   x = 0;
  if(x == trunc(x) && fabs(x) < 1e15) return snprintf(buf, n, "%.0f", x);
  return snprintf(buf, n, "%.*g", sig, x);
}

//============================== 
//
//    Plotting
//
//============================== 


typedef enum {
	PLOT_LINES,
	PLOT_STAIRS,
	PLOT_DOTS,
	PLOT_STYLE_COUNT
} PlotStyle;

// Same order as PlotStyle, so a choice's index is its enum value.
// Sized from the enum: a style without a string leaves a NULL that ends the list early.
static const char *const PLOT_STYLES[PLOT_STYLE_COUNT + 1] = {
	"lines",
	"stairs",
	"dots",
	NULL
};

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

// Helpers for Heckbert's algorithm
double nice_num(double x, bool round_ticks){
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

PlotAxis compute_axis(double min, double max, size_t target_ticks){
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
void canvas_set(BrailleCanvas *c, long px, long py){
	if(px < 0 || py < 0) return;
	if((size_t)px >= 2 * c->cols || (size_t)py >= 4 * c->rows) return;
	c->cells[(py / 4) * c->cols + (px / 2)] |= BRAILLE_BITS[py % 4][px % 2];
}

// To UTF-8
void put_braille(FILE *out, uint8_t bits){
	int cp = 0x2800 + bits;
	fputc(0xE0 | (cp >> 12),         out);
	fputc(0x80 | ((cp >> 6) & 0x3F), out);
	fputc(0x80 | (cp & 0x3F),        out);
}

long to_px(double x, const PlotAxis *ax, size_t width){
	return lround((x - ax->lo) / (ax->hi - ax->lo) * (width - 1));
}
long to_py(double y, const PlotAxis *ay, size_t height){
	return lround((ay->hi - y) / (ay->hi - ay->lo) * (height - 1));
}

// Bresenham
void canvas_line(BrailleCanvas *c, long x0, long y0, long x1, long y1){
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

// Number of decimals needed to print every multiple of step exactly
int tick_decimals(double step){
        for(int d = 0; d < 10; d++){
                double scaled = step * pow(10.0, d);
                if(fabs(scaled - round(scaled)) < 1e-6 * scaled) return d;
        }
        return 10;
}

// k-th tick: the first multiple of step inside [lo, hi], then every step after
double tick_value(const PlotAxis *axis, size_t k){
        double first = ceil(axis->lo / axis->step - 1e-9) * axis->step;
        double t     = first + k * axis->step;
        // Avoid printing "-0" for a tick that is zero up to rounding
        if(fabs(t) < 1e-9 * axis->step) t = 0.0;
        return t;
}

size_t num_ticks(const PlotAxis *axis){
        double first = ceil(axis->lo / axis->step - 1e-9) * axis->step;
        if(first > axis->hi + 1e-9 * axis->step) return 0;
        return (size_t)floor((axis->hi - first) / axis->step + 1e-9) + 1;
}


void plot_braille_frame(
		Arena *a,
		FILE *out, 
		const BrailleCanvas *c, 
		const PlotAxis *ax, 
		const PlotAxis *ay
		)
{ 
	size_t scratch = arena_save(a);

	// ---- y labels: one optional label per character row ----

	char  (*row_label)[TICK_LABEL_MAX] = arena_alloc(a, c->rows * TICK_LABEL_MAX);
	memset(row_label, 0, c->rows * TICK_LABEL_MAX);

	int    y_dec  = tick_decimals(ay->step);
	size_t width  = 0;
	size_t ny     = num_ticks(ay);
	for(size_t k = 0; k < ny; k++){
		double t   = tick_value(ay, k);
		long   row = to_py(t, ay, 4 * c->rows) / 4;
		if(row < 0 || (size_t)row >= c->rows) continue;
		snprintf(row_label[row], TICK_LABEL_MAX, "%.*f", y_dec, t);
		width = MAX(width, strlen(row_label[row]));
	}

	// ---- Rows: label, axis, braille cells ----

	for(size_t r = 0; r < c->rows; r++){
		bool has_label = row_label[r][0] != '\0';
		fprintf(out, "%*s %s", (int)width, row_label[r], has_label ? "┤" : "│");
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
	size_t offset   = width + 2;         
	size_t line_len = offset + c->cols + TICK_LABEL_MAX;
	char  *line     = arena_alloc(a, line_len + 1);
	memset(line, ' ', line_len);
	line[line_len]  = '\0';

	int  x_dec    = tick_decimals(ax->step);
	long last_end = -1;                    // column after the previous label
	for(size_t k = 0; k < nx; k++){
		double t   = tick_value(ax, k);
		long   col = to_px(t, ax, 2 * c->cols) / 2;
		if(col < 0 || (size_t)col >= c->cols) continue;

		char label[TICK_LABEL_MAX];
		int  len   = snprintf(label, sizeof(label), "%.*f", x_dec, t);
		long start = (long)offset + col - len / 2;
		if(start < 0) start = 0;
		if(start <= last_end) continue;    // would touch the previous label
		memcpy(line + start, label, len);
		last_end = start + len;
	}

	// Trim trailing spaces
	size_t end = line_len;
	while(end > 0 && line[end - 1] == ' ') end--;
	fprintf(out, "%.*s\n", (int)end, line);

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
	out->x = compute_axis(x_min, x_max, 6);
	out->y = compute_axis(y_min, y_max, 5);
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

	plot_braille_frame(a, out, &c, &layout->x, &layout->y);
	arena_restore(a, scratch);
}

//============================== 
//
//    Data structures
//
//============================== 

typedef enum {
  FN_NATIVE,
  FN_USER
} FunctionKind;

struct AST;

typedef struct {
  int         native_index;
  size_t      argc_min;
  size_t      argc_max;
} NativeFunctionData;

typedef struct {
  FunctionKind kind; 
  union {
    struct AST*        definition;
    NativeFunctionData native_data;
  };
} Function;

int function_repr(Arena *a, size_t argc_min, size_t argc_max, const char *id, char* buf){
	if(argc_max == ARGS_VARIADIC){
		int printed = sprintf(buf, "%s: fn(%zu+)", id, argc_min);
		buf[printed] = '\0';
		return printed;
	}else if(argc_min == argc_max){
		int printed = sprintf(buf, "%s: fn(%zu)", id, argc_min);
		buf[printed] = '\0';
		return printed;
	}else{
		int printed = sprintf(buf, "%s: fn(%zu-%zu)", id, argc_min, argc_max);
		buf[printed] = '\0';
		return printed;
	}
}

typedef struct Value Value;
typedef struct {
	Value*  items;
	size_t size;
} Tuple;

// --------------- Tensors ----------------------

#define MAX_DIMS 8
typedef enum { DTYPE_REAL, DTYPE_COMPLEX } DType;

typedef struct {
  DType dtype;
  union {
    double         *re;
    double complex *cx;
  } data;
  size_t ndim;
  size_t count;
  size_t sizes[MAX_DIMS];
  size_t strides[MAX_DIMS];
} Tensor;

int tensor_init(Arena *a, Tensor *t, size_t ndim, size_t *sizes, DType dtype){
  if(ndim == 0 || ndim > MAX_DIMS) return 1;

  size_t nele = 1;
  for(size_t i = 0; i < ndim; i++) nele *= sizes[i];

  t->ndim  = ndim;
  t->dtype = dtype;
  t->count = nele;

  if(dtype == DTYPE_REAL) {
    if((t->data.re = arena_alloc(a, nele * sizeof(double))) == NULL) return 1;
  }else{
    if((t->data.cx = arena_alloc(a, nele * sizeof(double complex))) == NULL) return 1;
  }


  for(size_t i = 0; i < ndim; i++) t->sizes[i] = sizes[i];

  // Example
  // sizes   = [2,  3,  4, 5]
  // strides = [60, 20, 5, 1]
  //

  t->strides[ndim - 1] = 1;
  for(size_t d = ndim - 1; d-- > 0;){
    t->strides[d] = t->sizes[d + 1] * t->strides[d + 1];
  }

  return 0;
}

// Allocates out with the same sizes AND strides of src, but a fresh
// data buffer.
int tensor_init_like(Arena *a, Tensor *out, Tensor *src, DType dtype){
	size_t elsize = dtype == DTYPE_REAL ? sizeof(double) : sizeof(double complex);
	void *buf     = arena_alloc(a, src->count * elsize);
	if(buf == NULL) return 1;
	*out          = *src;
	out->dtype    = dtype;
	if(dtype ==  DTYPE_REAL) out->data.re = buf;
	else                     out->data.cx = buf;
	return 0;
}

double complex tensor_get_cx(Tensor *t, size_t offset){
  return t->dtype == DTYPE_COMPLEX ? t->data.cx[offset] : t->data.re[offset];
}

void tensor_unravel_index(Tensor *t, size_t *indices, size_t index){
  // Unravels the indices following a row-major convention
  // Idea: Given a tensor's sizes, such as
  //       sizes   = [a,b,c,d]
  //       a set of row-major indices
  //       indices = [i,j,k,l]
  //       maps to the index:
  //       index   = bcd*i + cd*j + d*k + l
  //
  //       By repeatedly taking the % between the index and
  //       the sizes, we can recover the coefficients
  
  for(size_t d = t->ndim; d-- > 0;){
    indices[d] =  index % t->sizes[d];
    index     /= t->sizes[d];
  }
}

size_t tensor_get_offset(Tensor *t, size_t *indices){
  size_t offset = 0;
  for(size_t d = t->ndim; d-- > 0;) offset += t->strides[d] * indices[d];
  return offset;
}

void tensor_swap_axes(Tensor *t, size_t a, size_t b){
  size_t tmp    = t->sizes[a];
  t->sizes[a]   = t->sizes[b];
  t->sizes[b]   = tmp;
  tmp           = t->strides[a];
  t->strides[a] = t->strides[b];
  t->strides[b] = tmp;
}

int tensor_dot(Arena *arena, Tensor *a, Tensor *b, double complex *out){
  if(a->ndim != 1 || b->ndim != 1) return 1;
  if(a->sizes[0] != b->sizes[0])   return 1;
  *out = 0;
  for(size_t k = 0; k < a->count; k++){
    *out += tensor_get_cx(a, k * a->strides[0]) *
            tensor_get_cx(b, k * b->strides[0]);
  }
  return 0;
}

int tensor_contract(Arena *arena, Tensor *a, Tensor *b, Tensor *out){
  // Assume the following sizes:
  // a: [a1,a2,a3,a4]
  // b: [b1,b2,b3]
  // a @ b -> [a1,a2,a3,a4] @ [b1,b2,b3] -> [a1,a2,a3 | b2, b3]
  // Notice that a4 must be equal to b1.

  if(a->sizes[a->ndim - 1] != b->sizes[0]) return 1;
  size_t ndim_out          =  a->ndim + b->ndim - 2;
  DType dtype              =  DTYPE_COMPLEX;

  if(ndim_out > MAX_DIMS) return 1;

  if(a->dtype == DTYPE_REAL && b->dtype == DTYPE_REAL) dtype = DTYPE_REAL;

  size_t out_sizes[MAX_DIMS] = {0};
  //  0,  1,  2,  3,  4,  5,  6
  //  --------------------------
  // [a0, a1, a2, a3]             -> ndims = 4
  // [b0, b1, b2]                 -> ndims = 3
  // [a0, a1, a2, b1, b2]         -> ndims = 5
  //          ^
  for(size_t i = 0; i < a->ndim - 1; i++) out_sizes[i]               = a->sizes[i];
  for(size_t i = 1; i < b->ndim    ; i++) out_sizes[i + a->ndim - 2] = b->sizes[i];
  
  if(tensor_init(arena, out, ndim_out, out_sizes, dtype) != 0) return 1;

  // If C      = A @ B
  //    Cijklm = sum (Aijkd * Bdlm)
  //                 d in [0, last dim)
  size_t last_ndim            = b->sizes[0];
  size_t out_indices[MAX_DIMS] = {0};
  for(size_t linear = 0; linear < out->count; linear++){
    double sum          = 0;
    double complex csum = 0;

    size_t a_base = 0;
    size_t b_base = 0;
    tensor_unravel_index(out, out_indices, linear); // row-first indices, consistent with out
    for(size_t d = 0; d < a->ndim - 1; d++) a_base += a->strides[d] * out_indices[d];
    for(size_t d = 1; d < b->ndim    ; d++) b_base += b->strides[d] * out_indices[d + a->ndim - 2];

    if(out->dtype == DTYPE_REAL){
      for(size_t k = 0; k < last_ndim; k++){
        sum  += a->data.re[a_base + k * a->strides[a->ndim - 1]] *
                b->data.re[b_base + k * b->strides[0]];
      }
    }else{
      for(size_t k = 0; k < last_ndim; k++){
        csum += tensor_get_cx(a, a_base + k * a->strides[a->ndim - 1]) *
                tensor_get_cx(b, b_base + k * b->strides[0]);
      }
    }

    if(out->dtype == DTYPE_REAL)    out->data.re[linear] = sum;
    if(out->dtype == DTYPE_COMPLEX) out->data.cx[linear] = csum;
  }


  return 0;
}





// --------------- Polynomials ----------------------

// Example: 3x2 - 5x + 1:
// coeffs     = [1, -5, 3]
// num_coeffs = 3
// deg        = 2
typedef struct {
	double* coeffs;
	size_t  num_coeffs;
	size_t  deg;
} Polynomial;

Polynomial alloc_poly(Arena *a, size_t deg){
	Polynomial p;
	p.num_coeffs = deg + 1;
	p.deg        = deg;
	p.coeffs     = arena_alloc(a, p.num_coeffs * sizeof(double));
	memset(p.coeffs, 0, p.num_coeffs * sizeof(double));
	return p;
}

int poly_repr(Arena *a, Polynomial *p, char* buf, char* variable){
	int printed = 0;
  if(variable == NULL){
    variable = "x";
  }
	// Highest power first: 3 x^2 - 5 x + 1
	for(size_t i = p->num_coeffs; i-- > 0;){
		double coef = p->coeffs[i];
		if(coef == 0) continue;

		// The sign goes in the separator, so we get "- 5 x" instead of "+ -5 x"
		if(printed == 0){
			if(coef < 0) printed += sprintf(buf + printed, "-");
		}else{
			printed += sprintf(buf + printed, coef < 0 ? " - " : " + ");
		}

		double mag = fabs(coef);
		char cbuf[32];
		format_number(cbuf, sizeof(cbuf), mag, DISPLAY_SIG);

		// A coefficient of 1 is implicit, except on the constant term
		if(i == 0 || mag != 1) printed += sprintf(buf + printed, "%s", cbuf);
		if(i > 0 && mag != 1)  printed += sprintf(buf + printed, " ");
		if(i > 1)              printed += sprintf(buf + printed, "%s^%zu", variable, i);
		else if(i == 1)        printed += sprintf(buf + printed, "%s", variable);
	}
	if(printed == 0) printed += sprintf(buf, "0");
	buf[printed] = '\0';
  return printed;
}

typedef struct {
	Polynomial num;
	Polynomial den;
	double     Ts;   // Sample time. Ts = 0 means CT, Ts > 0 means DT
} TransferFunction;

// Appends to buf without ever writing past size; *len stops at size - 1
void buf_append(char *buf, size_t size, size_t *len, const char *fmt, ...){
  if(*len >= size) return;
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf + *len, size - *len, fmt, args);
  va_end(args);
  if(n < 0) return;
  *len = MIN(*len + (size_t)n, size - 1);
}

int transfer_function_repr(Arena *a, TransferFunction *tf, char *buf, size_t size){
  // The TF knows its own domain, so it picks the variable: s for CT, z for DT
  char *variable = tf->Ts > 0 ? "z" : "s";

  char num_buf[1024];
  char den_buf[1024];
  int num_size = poly_repr(a, &tf->num, num_buf, variable);
  int den_size = poly_repr(a, &tf->den, den_buf, variable);

  size_t printed = 0;
  buf[0] = '\0';

  bool den_is_one = tf->den.deg == 0 && tf->den.coeffs[0] == 1;
  if(den_is_one){
    // Nothing to divide by: print the numerator on one line
    buf_append(buf, size, &printed, "%s", num_buf);
  }else{
    int max_size    = MAX(num_size, den_size);
    int padding_num = (max_size - num_size) / 2;
    int padding_den = (max_size - den_size) / 2;

    // %*s pads with that many spaces
    buf_append(buf, size, &printed, "%*s%s\n", padding_num, "", num_buf);
    for(int i = 0; i < max_size; i++) buf_append(buf, size, &printed, "-");
    buf_append(buf, size, &printed, "\n%*s%s", padding_den, "", den_buf);
  }

  if(tf->Ts > 0) buf_append(buf, size, &printed, "\nTs = %g", tf->Ts);

  return printed;
}


char *alloc_string(Arena *a, size_t len){
	char* new_string = arena_alloc(a, len + 1);
	memset(new_string, 0, len + 1);
	return new_string;
}


//============================== 
//
//    Tokenization
//
//============================== 

// X-Macros for defining token types.
// I wanted more granular control over how those
// tokens were printed, so instead of defining
// a single X macro, I added a V macro for tokens
// that carry value information

#define TOK_TYPE_LIST \
	V(TOK_KEYWORD)      \
	V(TOK_ID)           \
	V(TOK_NL)           \
	V(TOK_EOF)          \
	V(TOK_STRING)       \
	V(TOK_NUMBER)       \
	X(TOK_LPAREN)       \
	X(TOK_RPAREN)       \
	X(TOK_LBRACKET)     \
	X(TOK_RBRACKET)     \
	X(TOK_LCURLY)       \
	X(TOK_RCURLY)       \
	X(TOK_COMMA)        \
	X(TOK_DOT)          \
	X(TOK_EQ)           \
	X(TOK_DBL_EQ)       \
	X(TOK_LT)           \
	X(TOK_GT)           \
	X(TOK_LEQ)          \
	X(TOK_GEQ)          \
	V(TOK_PLUS)         \
	V(TOK_MINUS)        \
	V(TOK_MUL)          \
	V(TOK_DIV)          \
	V(TOK_CARET)        \
	V(TOK_AT)           \
	V(TOK_SEMI)         \
	X(TOK_TYPE_COUNT)

typedef enum {
#define X(name) name,
#define V(name) name,
	TOK_TYPE_LIST
#undef X
#undef V
} TokenType;

typedef struct {
	TokenType type;
	char      *value;
} Token;

void token_type_repr(TokenType type, char *buf){
	int repr_size = 0;
	switch(type){
#define X(name) case name: { repr_size = sprintf(buf, #name); break;}
#define V(name) case name: { repr_size = sprintf(buf, #name); break;}
		TOK_TYPE_LIST
#undef X
#undef V
	}
	buf[repr_size] = '\0';
}

void token_repr(Token *tok, char *buf){
	int repr_size = 0;
	switch(tok->type){
#define X(name) case name: { repr_size = sprintf(buf, #name); break;}
#define V(name) case name: { repr_size = sprintf(buf, "%-15s [%s]", #name, tok->value); break;}
		TOK_TYPE_LIST
#undef X
#undef V
	}
	buf[repr_size] = '\0';
}

void token_print(Token *tok){
	char buf[256];
	token_repr(tok, buf);
	printf("%s\n", buf);
}

typedef struct {
	const char   *source;
	size_t       size;
	size_t       offset;
} Tokenizer;

int tokenizer_eat_char(Tokenizer *tokenizer, char c){
  const char *pos = tokenizer->source + tokenizer->offset;
  if(pos[0] == c){
    tokenizer->offset++;
    return 0;
  }
  return 1;
}


size_t tokenizer_is_id(const char *pos){
	if (!isalpha((unsigned char)pos[0]) && pos[0] != '_')
		return 0;
	size_t i = 1;
	while(isalnum((unsigned char)pos[i]) || pos[i] == '_'){
		i += 1;
	}
	return i;
}

size_t tokenizer_is_number(const char *pos){
	if(!isdigit(pos[0])) return 0;
	size_t i = 1;
	while(isdigit(pos[i])){
		i += 1;
	}
	if(pos[i] == '.' && isdigit(pos[i + 1])){
		// scientific notation
		i += 1;
	}
	while(isdigit(pos[i])){
		i += 1;
	}
  if((pos[i] == 'e' || pos[i] == 'E') &&
     (isdigit(pos[i + 1]) ||
      ((pos[i + 1] == '+' || pos[i + 1] == '-') && isdigit(pos[i + 2])))){
          i += (pos[i + 1] == '+' || pos[i + 1] == '-') ? 2 : 1;
          while(isdigit(pos[i])) i += 1;
  }       
	return i;
}


int tokenizer_lookahead(const char *pos, const char *symbol){
	if(strncmp(pos + 1, symbol, 1) == 0) return 1;
	return 0;
}
int tokenizer_is_symbol(const char *pos, const char *symbol){
	if(strncmp(pos, symbol, 1) == 0) return strlen(symbol);
	return 0;
}

int tokenizer_is_str(const char *pos, const char *str, size_t n){
	if(strncmp(pos, str, n) == 0) return 1;
	return 0;
}

Token tokenizer_make_symbol(Arena *a, TokenType type, Tokenizer *tokenizer, size_t symbol_len){
	Token tok = {
		tok.type  = type,
		tok.value = NULL,
	};
	size_t num_chars = symbol_len;
	tokenizer->offset += num_chars;
	return tok;
}

int tokenizer_eat_string(Arena *a, Tokenizer *tokenizer, Token *out){
	tokenizer_eat_char(tokenizer, '"');
	size_t first = tokenizer->offset;

	while(tokenizer->offset < tokenizer->size &&
			tokenizer->source[tokenizer->offset] != '"'  &&
			tokenizer->source[tokenizer->offset] != '\n'){
		tokenizer->offset++;
	}

	if(tokenizer->offset >= tokenizer->size || tokenizer->source[tokenizer->offset] != '"'){
		fprintf(stderr, "Unterminated string at offset %zu\n", first - 1);
		return 1;
	}

	size_t num_chars = tokenizer->offset - first;
	tokenizer_eat_char(tokenizer, '"');

	out->type  = TOK_STRING;
	out->value = arena_alloc(a, num_chars + 1);
	memcpy(out->value, tokenizer->source + first, num_chars);
	out->value[num_chars] = '\0';
	return 0;
}

#define KEYWORDS_LIST  \
        X(let)         \
        X(fn)          \
        X(if)          \
        X(else)        \
        X(elif)        \
        X(return)      \
        X(for)         \
        X(in)          \
        V(KEYWORDS_COUNT)

typedef enum {
#define X(x) kw_##x,
#define V(x) x,
  KEYWORDS_LIST
#undef X
#undef V
} Keyword;

const char *KEYWORDS[KEYWORDS_COUNT] = {
#define X(x) #x,
#define V(x)
  KEYWORDS_LIST
#undef X
#undef V
};

size_t keywork_match(const char *pos){
  for(size_t i = 0; i < KEYWORDS_COUNT; i++){
    size_t len = strlen(KEYWORDS[i]);
    if(
        tokenizer_is_str(pos, KEYWORDS[i], len)
        && !isalnum((unsigned char)pos[len])
        && pos[len] != '_'
      )
    {
      return len;
    }
  }
  return 0;
}

Token tokenize_keyword(Arena *a, const char *pos, size_t len){
  char *value = arena_alloc(a, len + 1);
  strncpy(value, pos, len);
  value[len] = '\0';
  Token tok = {
    .type  = TOK_KEYWORD,
    .value = value,
  };
  return tok;
}



int tokenize(Arena *a, const char *line, size_t n, Token *tokens , size_t *num_tokens){
	Tokenizer tokenizer;
	tokenizer.source = line;
	tokenizer.offset = 0;
	tokenizer.size   = n;

	*num_tokens      = 0;
	int depth        = 0;

	while(tokenizer.offset < tokenizer.size){
		const char *pos = tokenizer.source + tokenizer.offset;

		if(pos[0] == '\n'){
			if(depth == 0){
				Token token = {
					.type = TOK_NL,
					.value = "\\n",
				};
				tokens[(*num_tokens)++] = token;
			}
			tokenizer.offset++;
			continue;
		}

		if(pos[0] == '/' && pos[1] == '/'){
      while(tokenizer.offset < tokenizer.size && tokenizer.source[tokenizer.offset] != '\n') {
        tokenizer.offset++;
      }
      continue;
		}

		if(isspace(pos[0])){
			tokenizer.offset++;
			continue;
		}

    // Check for strings
    
    if(pos[0] == '"'){
      Token tok;
			if(tokenizer_eat_string(a, &tokenizer, &tok) != 0) return 1;
      tokens[(*num_tokens)++] = tok;
      continue;
    }

		// Check for keywords
    size_t keyword_len = keywork_match(pos);
    if(keyword_len > 0){
      Token tok = tokenize_keyword(a, pos, keyword_len);
			tokens[*num_tokens] = tok;
			(*num_tokens)++;
			tokenizer.offset += keyword_len;
      continue;
    }

		// Check for identifiers
		int id_size = tokenizer_is_id(pos);
		if(id_size > 0){
			char *value = arena_alloc(a, id_size + 1);
			strncpy(value, pos, id_size);
			value[id_size] = '\0';
			Token tok = {
				tok.type  = TOK_ID,
				tok.value = value,
			};
			tokens[*num_tokens] = tok;
			(*num_tokens)++;
			tokenizer.offset += id_size;
			// token_print(&tok);
			continue;
		}

		// Check for identifiers
		int num_size = tokenizer_is_number(pos);
		if(num_size > 0){
			char *value = arena_alloc(a, num_size + 1);
			strncpy(value, pos, num_size);
			value[num_size] = '\0';
			Token tok = {
				tok.type  = TOK_NUMBER,
				tok.value = value,
			};
			tokens[*num_tokens] = tok;
			(*num_tokens)++;
			tokenizer.offset += num_size;
			// token_print(&tok);
			continue;
		}
		// Check for binary operators
			size_t symbol_len = 0;
			if((symbol_len = tokenizer_is_symbol(pos, "+"))) {
				Token tok = tokenizer_make_symbol(a, TOK_PLUS, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "-"))) {
				Token tok = tokenizer_make_symbol(a, TOK_MINUS, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "*"))) {
				Token tok = tokenizer_make_symbol(a, TOK_MUL, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "/"))) {
				Token tok = tokenizer_make_symbol(a, TOK_DIV, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "^"))) {
				Token tok = tokenizer_make_symbol(a, TOK_CARET, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};
			if((symbol_len = tokenizer_is_symbol(pos, "@"))) {
				Token tok = tokenizer_make_symbol(a, TOK_AT, &tokenizer, symbol_len);
				tokens[(*num_tokens)++] = tok;
				continue;
			};

		// Check for special symbols
		if(tokenizer_is_symbol(pos, "=") && tokenizer_lookahead(pos, "=")) {
			Token tok = tokenizer_make_symbol(a, TOK_DBL_EQ, &tokenizer, 2);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if(tokenizer_is_symbol(pos, "<") && tokenizer_lookahead(pos, "=")) {
			Token tok = tokenizer_make_symbol(a, TOK_LEQ, &tokenizer, 2);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if(tokenizer_is_symbol(pos, ">") && tokenizer_lookahead(pos, "=")) {
			Token tok = tokenizer_make_symbol(a, TOK_GEQ, &tokenizer, 2);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "<"))) {
			Token tok = tokenizer_make_symbol(a, TOK_LT, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ">"))) {
			Token tok = tokenizer_make_symbol(a, TOK_GT, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "="))) {
			Token tok = tokenizer_make_symbol(a, TOK_EQ, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "("))) {
			Token tok = tokenizer_make_symbol(a, TOK_LPAREN, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			depth++;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ")"))) {
			Token tok = tokenizer_make_symbol(a, TOK_RPAREN, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			depth--;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "["))) {
			Token tok = tokenizer_make_symbol(a, TOK_LBRACKET, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			depth++;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "]"))) {
			Token tok = tokenizer_make_symbol(a, TOK_RBRACKET, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			depth--;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "{"))) {
			Token tok = tokenizer_make_symbol(a, TOK_LCURLY, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "}"))) {
			Token tok = tokenizer_make_symbol(a, TOK_RCURLY, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ","))) {
			Token tok = tokenizer_make_symbol(a, TOK_COMMA, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, "."))) {
			Token tok = tokenizer_make_symbol(a, TOK_DOT, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};
		if((symbol_len = tokenizer_is_symbol(pos, ";"))) {
			Token tok = tokenizer_make_symbol(a, TOK_SEMI, &tokenizer, symbol_len);
			tokens[(*num_tokens)++] = tok;
			continue;
		};

		fprintf(stderr,"Unexpected character at offset %zu: %c\n", tokenizer.offset, pos[0]);
		return 1;

	}

	Token eof = { .type = TOK_EOF };
	tokens[(*num_tokens)++] = eof;

	return 0;

}

//============================== 
//
//    Parsing
//
//============================== 

// X-Macros are convenient here :)

#define AST_NODES_LIST        \
	      X(AST_PROGRAM)        \
	      X(AST_STATEMENT_LIST) \
	      X(AST_STATEMENT)      \
	      X(AST_LITERAL)        \
	      X(AST_ID)             \
	      X(AST_INDEXING)       \
	      X(AST_ARRAY)          \
	      X(AST_ROW)            \
	      X(AST_TUPLE)          \
	      X(AST_UNARY_OP)       \
	      X(AST_BINARY_OP)      \
	      X(AST_POSTFIX_OP)     \
	      X(AST_FN_CALL)        \
	      X(AST_ARGS_LIST)      \
	      X(AST_NODE_TYPE_COUNT)
				
typedef enum {
#define X(name) name,
	AST_NODES_LIST
#undef X
} ASTNodeType;


#define OP_TYPE_LIST         \
	      X(OP_PLUS)           \
	      X(OP_MINUS)          \
	      X(OP_MUL)            \
	      X(OP_DIV)            \
	      X(OP_POW)            \
	      X(OP_TMUL)           \
				X(OP_DBL_EQ)         \
				X(OP_GEQ)            \
				X(OP_LEQ)            \
				X(OP_LT)             \
				X(OP_GT)             \
	      X(OP_TYPE_LIST_COUNT)

typedef enum {
#define X(name) name,
	OP_TYPE_LIST
#undef X
} OpType;

void op_type_repr(OpType type, char *buf){
	int repr_size = 0;
	switch(type){
#define X(name) case name: { repr_size = sprintf(buf, #name); break;}
#define V(name) case name: { repr_size = sprintf(buf, #name); break;}
		OP_TYPE_LIST
#undef X
#undef V
	}
	buf[repr_size] = '\0';
}

OpType op_from_tok(TokenType type){
	switch (type) {
		case TOK_PLUS:    return OP_PLUS;
	  case TOK_MINUS:   return OP_MINUS;
	  case TOK_MUL:     return OP_MUL;
	  case TOK_DIV:     return OP_DIV;
	  case TOK_CARET:   return OP_POW;
	  case TOK_AT:      return OP_TMUL;
	  case TOK_DBL_EQ:  return OP_DBL_EQ;
	  case TOK_GEQ:     return OP_GEQ;
	  case TOK_LEQ:     return OP_LEQ;
	  case TOK_LT:      return OP_LT;
	  case TOK_GT:      return OP_GT;
		default:          UNREACHABLE("op_from_tok");
	}
}

// Expression value types

typedef enum {
	VALUE_ID,
	VALUE_NUMBER,
	VALUE_COMPLEX,
	VALUE_STRING,
	VALUE_POLYNOMIAL,
	VALUE_TRANSFER_FUNCTION,
	VALUE_NAN,

  // User-defined functions

	VALUE_FUNCTION,

	// Things to think about
	VALUE_TUPLE,
	VALUE_TENSOR,
	VALUE_TYPE_COUNT
} ValueType;

typedef enum {
	STATEMENT_DECL_ASSIGN,
	STATEMENT_ASSIGN,
	STATEMENT_FUNCTION_DEF,
	STATEMENT_IF,
	STATEMENT_FOR,
	STATEMENT_RETURN,
	STATEMENT_EXPR,
} StatementType;

// Forward declarations
typedef struct AST AST;

struct Value {
	ValueType valueType;
	union {
		Polynomial       poly;
		TransferFunction tf;
		double           number;
		double complex   complex_number;
		char             *str;
		char             *id;
		Tensor           tensor;
		Tuple            tuple;
    Function         fun;
	} value;
};


struct AST {
	ASTNodeType   type;
  StatementType statement_type;
	AST*          parent;
	AST*          next; // Optional, useful for linked lists

	union {
		struct {
			size_t num_statements;
			AST    *statement_list;
		} program;

		struct {
			char          *dst;
			AST           *expr;
		} statement;

		struct {
			size_t count;
			AST    *first;
		} statement_list;

		struct {
			AST    *id;
			AST    *expr;
			AST    *block;
		} for_stmt;

		struct {
			AST *condition;
			AST *if_block;
			AST *else_block;
      int else_is_elif;
		} if_stmt;

		struct {
			char   *id;
			AST    *params;
			AST    *block;
      size_t num_args;
		} fn_def;

		Value literal;

		struct   {
			size_t size;
			AST    *first;
		} array;

		struct   {
			size_t size;
			AST    *first;
		} row;

		struct   {
			size_t size;
			AST    *first;
		} tuple;

		struct   {
			char   *fn_name;
			size_t num_arguments;
			AST    *first_arg;
		} fn_call;

		struct   {
			char   *var_name;
			AST    *index_expr;
		} indexing;

		struct {
			OpType op;
			AST    *left;
			AST    *right;
		} binop;

		struct {
			OpType op;
			AST    *arg;
		} unop;

		struct {
			AST  *target;
			char *op;
		} postfix_op;
	} as;
};

void ast_node_repr(AST *ast, char *buf){
	int repr_size = 0;
	switch(ast->type){
#define X(name) case name:   { repr_size = sprintf(buf, #name); break;}
		AST_NODES_LIST
#undef X
	}
	buf[repr_size] = '\0';
}

void ast_node_print(AST *ast){
	char buf[256];
	ast_node_repr(ast, buf);
	printf("%s\n", buf);
}

typedef struct {
	Token  *tokens;
	size_t num_tokens;
	size_t next;
} Parser;

Token *parser_peek(Parser *parser){
	if(parser->next < parser->num_tokens) return &parser->tokens[parser->next];
	return &parser->tokens[parser->num_tokens - 1];
}

int parser_done(Parser *parser){
	return parser_peek(parser)->type == TOK_EOF;
}


AST *ast_alloc(Arena *pa, ASTNodeType type){
	AST *node = arena_alloc(pa, sizeof(AST));
	memset(node, 0, sizeof(AST));
	node->type = type;
	return node;
}

Token *parser_eat(TokenType type, Parser *parser){
	if(parser_done(parser)) return NULL;
	Token *next = parser_peek(parser);
	if(next->type == type){
		parser->next++;
		return next;
	}
	return NULL;
}


Token *parser_keyword_eat(const char* expected, Parser *parser){
  Token *kw = parser_eat(TOK_KEYWORD, parser);
  if(kw == NULL) return NULL;
  if(strcmp(kw->value, expected) == 0){
    return kw;
  }
	return NULL;
}

Token *parser_keyword_match(const char* expected, Token *kw){
  if(kw->type != TOK_KEYWORD) return NULL;
  if(strcmp(kw->value, expected) == 0){
    return kw;
  }
	return NULL;
}



Token *parser_lookahead(Parser *parser){
	if(parser->next + 1 >= parser->num_tokens) return NULL;
	Token *la = &parser->tokens[parser->next + 1];
	return la;
}

int parser_eat_stmt_end(Parser *parser){
	if(parser_done(parser))                             return 1;
	if(parser_eat(TOK_NL, parser))                      return 1;
	if(parser->tokens[parser->next].type == TOK_RCURLY) return 1;
	return 0;
}



AST *ast_make_string(Arena *pa, size_t len){
	AST *expr = ast_alloc(pa, AST_LITERAL);
	expr->statement_type       = STATEMENT_EXPR;
	expr->as.literal.value.str = arena_alloc(pa, len + 1);
	expr->as.literal.valueType = VALUE_STRING;
	return expr;
}

AST *ast_make_number(Arena *pa, double number){
	AST *expr = ast_alloc(pa, AST_LITERAL);
	expr->statement_type          = STATEMENT_EXPR;
	expr->as.literal.value.number = number;
	expr->as.literal.valueType    = VALUE_NUMBER;
	return expr;
}

AST *ast_make_id(Arena *pa, size_t len){
	AST *expr = ast_alloc(pa, AST_ID);
	expr->statement_type       = STATEMENT_EXPR;
	expr->as.literal.value.id  = arena_alloc(pa, len + 1);
	expr->as.literal.valueType = VALUE_ID;
	return expr;
}

AST *ast_make_indexing(Arena *pa, size_t len){
	AST *expr = ast_alloc(pa, AST_INDEXING);
	expr->statement_type         = STATEMENT_EXPR;
	expr->as.indexing.var_name   = arena_alloc(pa, len + 1);
	expr->as.indexing.index_expr = NULL;
	return expr;
}


AST *ast_make_array(Arena *pa){
	AST *array = ast_alloc(pa, AST_ARRAY);
	array->statement_type = STATEMENT_EXPR;
	array->as.array.first = NULL;
	array->as.array.size  = 0;
	return array;
}

AST *ast_make_row(Arena *pa){
	AST *row = ast_alloc(pa, AST_ROW);
	row->statement_type = STATEMENT_EXPR;
	row->as.row.first = NULL;
	row->as.row.size  = 0;
	return row;
}

AST *ast_make_tuple(Arena *pa){
	AST *tuple = ast_alloc(pa, AST_TUPLE);
	tuple->statement_type = STATEMENT_EXPR;
	tuple->as.tuple.first = NULL;
	tuple->as.tuple.size  = 0;
	return tuple;
}

AST *ast_make_fn_call(Arena *pa, const char *fn_name){
	AST *fn_call = ast_alloc(pa, AST_FN_CALL);
	fn_call->statement_type           = STATEMENT_EXPR;
	fn_call->as.fn_call.num_arguments = 0;
	fn_call->as.fn_call.fn_name       = arena_alloc(pa, strlen(fn_name) + 1);
	fn_call->as.fn_call.first_arg     = NULL;
	strcpy(fn_call->as.fn_call.fn_name, fn_name);
	return fn_call;
}

AST *ast_make_unop(Arena *pa, OpType op){
	AST *unop = ast_alloc(pa, AST_UNARY_OP); 
	unop->statement_type = STATEMENT_EXPR;
	unop->as.unop.op     = op;
	unop->as.unop.arg    = NULL;
	return unop;
}


AST *ast_make_binop(Arena *pa, OpType op){
	AST *binop = ast_alloc(pa, AST_BINARY_OP); 
	binop->statement_type = STATEMENT_EXPR;
	binop->as.binop.left  = NULL;
	binop->as.binop.right = NULL;
	binop->as.binop.op    = op;
	return binop;
}

AST *ast_make_postfix(Arena *pa,  char *op){
	AST *posfix_op                   = ast_alloc(pa, AST_POSTFIX_OP);
	posfix_op->statement_type        = STATEMENT_EXPR;
	posfix_op->as.postfix_op.target  = NULL;
	size_t len                       = strlen(op);
	posfix_op->as.postfix_op.op      = arena_alloc(pa, len + 1);
	strcpy(posfix_op->as.postfix_op.op, op);
	posfix_op->as.postfix_op.op[len] = '\0';
	return posfix_op;
}


AST *ast_make_statement(Arena *pa, StatementType type){
	AST *stmt            = ast_alloc(pa, AST_STATEMENT);
	stmt->statement_type = type;
	return stmt;
}

AST *ast_make_statement_list(Arena *pa){
	AST *stmt_list                     = ast_alloc(pa, AST_STATEMENT_LIST);
	stmt_list->as.statement_list.count = 0;
	return stmt_list;
}



AST *parse_expression(Arena *pa, AST *parent, Parser *parser); // Forward declaration
AST *parse_term(Arena *pa, AST *parent, Parser *parser);
AST *parse_unary(Arena *pa, AST *parent, Parser *parser);
AST *parse_expo(Arena *pa, AST *parent, Parser *parser);
AST *parse_postfix(Arena *pa, AST *parent, Parser *parser);
AST *parse_primary(Arena *pa, AST *parent, Parser *parser);
AST *parse_array(Arena *pa, AST *parent, Parser *parser);
AST *parse_statement_list(Arena *pa, AST *program_node, Parser *parser);

AST *parse_row_tail(Arena *pa, AST *parent, Parser *parser){
	size_t save = parser->next;

	if(parser_eat(TOK_COMMA, parser) == NULL) return NULL;
	// Trailing comma, as in [1, 2, 3,]: keep the comma consumed, end the row
	if(!parser_done(parser) && parser_peek(parser)->type == TOK_RBRACKET) return NULL;
	AST *expr = parse_expression(pa, parent, parser);
	if(!expr){
		parser->next = save;
		return NULL;
	}

	AST *tail = parse_row_tail(pa, parent, parser);
	if(tail) expr->next = tail;

	return expr;
}

AST *parse_row(Arena *pa, AST *parent, Parser *parser){
	AST *row    = ast_make_row(pa);

	AST *expr   = parse_expression(pa, parent, parser);
	if(!expr) return NULL;

	// parse_row_tail restores the position itself when it fails
	AST *tail = parse_row_tail(pa, parent, parser);

	row->as.row.first = expr;
	expr->next        = tail;

	return row;
}

AST *parse_array_tail(Arena *pa, AST *parent, Parser *parser){
	size_t save = parser->next;
	if(parser_eat(TOK_SEMI, parser) == NULL) return NULL;

	AST *row = parse_row(pa, parent, parser);
	if(!row){
		parser->next = save;
		return NULL;
	}

	save = parser->next;
	AST *tail = parse_array_tail(pa, parent, parser);
	if(!tail) parser->next = save;

	row->next = tail;
	return row;
}

AST *parse_array(Arena *pa, AST *parent, Parser *parser){
	if(parser_eat(TOK_LBRACKET, parser) == NULL) return NULL;
	AST *array = ast_make_array(pa);
	if(!parser_done(parser) && parser_peek(parser)->type == TOK_RBRACKET){
		if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
		return array;
	}

	AST *row = parse_row(pa, parent, parser);
	if(row == NULL) return NULL;

	AST *tail = parse_array_tail(pa, parent, parser);
	array->as.array.first = row;
	row->next             = tail;

	if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
	return array;
}
// AST *parse_array(Arena *pa, AST *parent, Parser *parser){
// 	if(parser_eat(TOK_LBRACKET, parser) == NULL) return NULL;
// 	AST *array = ast_make_array(pa);
// 	if(!parser_done(parser) && parser_peek(parser)->type == TOK_RBRACKET){
// 		if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
// 		return array;
// 	}

// 	AST *head = parse_expression(pa, parent, parser);
// 	if(!head) return NULL;
// 	if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);

// 	array->as.array.first = head;
// 	AST *tail             = head;

// 	size_t size = 1;

// 	while(!parser_done(parser) && parser_peek(parser)->type != TOK_RBRACKET){
// 		AST *expr   = parse_expression(pa, parent, parser);
// 		if(!expr) return NULL;
// 		if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);
// 		tail->next  = expr;
// 		tail        = expr;
// 		size       += 1;
// 	}

// 	if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
// 	array->as.array.size = size;
// 	return array;
// }

AST *parse_fn_call(Arena *pa, AST *parent, Parser *parser){
    Token *name = parser_eat(TOK_ID, parser);
    if(!name) return NULL;
    char *fn_name = name->value;
		if(parser_eat(TOK_LPAREN, parser) == NULL) return NULL;
		AST *fn_call = ast_make_fn_call(pa, fn_name);
		if(!parser_done(parser) && parser_peek(parser)->type == TOK_RPAREN){
			if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
			fn_call->as.fn_call.num_arguments = 0;
			return fn_call;
		}

		AST *head = parse_expression(pa, parent, parser);
		if(!head) return NULL;
		if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);
		AST *tail = NULL;
		if(head == NULL){
			if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
			fn_call->as.fn_call.num_arguments = 0;
			return fn_call;
		}
		fn_call->as.fn_call.first_arg = head;
		tail                          = head;
		size_t num_args               = 1;

		while(!parser_done(parser) && parser_peek(parser)->type != TOK_RPAREN){
			AST *expr  = parse_expression(pa, parent, parser);
			if(!expr) return NULL;
			if(parser_peek(parser)->type == TOK_COMMA) parser_eat(TOK_COMMA, parser);
			tail->next = expr;
			tail       = expr;
			num_args++;
		}

		if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
		fn_call->as.fn_call.num_arguments = num_args;
		return fn_call;
}

AST *parse_number(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
		if((current = parser_eat(TOK_NUMBER, parser)) == NULL) return NULL;
		AST *expr = ast_make_number(pa, atof(current->value));
		expr->parent = parent;
		return expr;
}

AST *parse_id(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
		if((current = parser_eat(TOK_ID, parser)) == NULL) return NULL;
    size_t len = strlen(current->value);
		AST *id    = ast_make_id(pa, len);
    strcpy(id->as.literal.value.id, current->value);
		id->parent = parent;
		return id;
}

AST *parse_indexing(Arena *pa, AST *parent, Parser *parser){
    Token *name = parser_eat(TOK_ID, parser);
    if(!name) return NULL;
    char *var_name = name->value;
		if(parser_eat(TOK_LBRACKET, parser) == NULL) return NULL;

		AST *indexing = ast_make_indexing(pa, strlen(var_name));
    strcpy(indexing->as.indexing.var_name, name->value);
		AST *expr  = parse_expression(pa, parent, parser);
		if(!expr) return NULL;
		indexing->as.indexing.index_expr = expr;

		if(parser_eat(TOK_RBRACKET, parser) == NULL) return NULL;
		return indexing;
}

AST *parse_string(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
		if((current = parser_eat(TOK_STRING, parser)) == NULL) return NULL;
    size_t len = strlen(current->value);
		AST *expr = ast_make_string(pa, len);
    strcpy(expr->as.literal.value.str, current->value);
		expr->parent = parent;
		return expr;
}


AST *parse_tuple_tail(Arena *pa, AST *parent, Parser *parser);
AST *parse_tuple_rest(Arena *pa, AST *parent, Parser *parser) {
    if(parser_eat(TOK_COMMA, parser) == NULL) return NULL;
    return parse_tuple_tail(pa, parent, parser);
}

AST *parse_tuple_tail(Arena *pa, AST *parent, Parser *parser) {
    size_t save = parser->next;
    AST *expr = NULL;
    if((expr = parse_expression(pa, parent, parser)) == NULL) {
        parser->next = save; // Restore cursor on epsilon!
        return NULL;
    }
    expr->next = parse_tuple_rest(pa, parent, parser);
    return expr;
}


AST *parse_tuple(Arena *pa, AST *parent, Parser *parser) {
    AST* head = NULL;
    if(parser_eat(TOK_LPAREN, parser) == NULL) return NULL;

    if((head = parse_expression(pa, parent, parser)) == NULL) return NULL;
    if(parser_eat(TOK_COMMA, parser) == NULL) return NULL;

    head->next = parse_tuple_tail(pa, parent, parser);

    if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
    AST *tuple = ast_make_tuple(pa);
    tuple->as.tuple.first = head;

    size_t size = 0;
    for(AST *node = head; node != NULL; node = node->next) { size++; }
    tuple->as.tuple.size = size;
    return tuple;
}

AST *parse_paren_expr(Arena *pa, AST *parent, Parser *parser) {
    Token *current = NULL;
    if(parser_eat(TOK_LPAREN, parser) == NULL) return NULL;
    AST *expr = parse_expression(pa, parent, parser);
    if(parser_eat(TOK_RPAREN, parser) == NULL) return NULL;
    return expr;
}

AST *parse_primary(Arena *pa, AST *parent, Parser *parser) {
	Token *current = parser_peek(parser);
  size_t save = parser->next;
  AST *primary = NULL;

  if(
      (parser->next = save, primary = parse_number(pa, parent, parser))     ||
      (parser->next = save, primary = parse_fn_call(pa, parent, parser))    ||
      (parser->next = save, primary = parse_indexing(pa, parent, parser))   ||
      (parser->next = save, primary = parse_id(pa, parent, parser))         ||
      (parser->next = save, primary = parse_string(pa, parent, parser))     ||
      (parser->next = save, primary = parse_array(pa, parent, parser))      ||
      (parser->next = save, primary = parse_tuple(pa, parent, parser))      ||
      (parser->next = save, primary = parse_paren_expr(pa, parent, parser))
    ){
    return primary;
  }
  return NULL;
}

AST *parse_term(Arena *pa, AST *parent, Parser *parser){
	AST *left = parse_unary(pa, parent, parser);
	if(!left) return NULL;
	while(!parser_done(parser) &&
			(parser_peek(parser)->type == TOK_MUL ||
			 parser_peek(parser)->type == TOK_AT  ||
			 parser_peek(parser)->type == TOK_DIV 
			 )
		)
	{
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_unary(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = left;
		binop->as.binop.right = right;
		left = binop;
	}
	return left;
}

AST *parse_unary(Arena *pa, AST *parent, Parser *parser){
  if(parser_peek(parser)->type == TOK_PLUS || parser_peek(parser)->type == TOK_MINUS){
    OpType op;

    if(parser_peek(parser)->type == TOK_PLUS)  op = OP_PLUS;
    else if(parser_peek(parser)->type == TOK_MINUS) op = OP_MINUS;
    else return NULL;

    parser->next++;

    AST *unop         = ast_make_unop(pa, op)  ;
    unop->as.unop.arg = parse_unary(pa, unop, parser);
		if(!unop->as.unop.arg) return NULL;

    return unop;
  }
  return parse_expo(pa, parent, parser);

}

AST *parse_expo(Arena *pa, AST *parent, Parser *parser){
	AST *postfix = parse_postfix(pa, parent, parser);
	if(!postfix) return NULL;
	if(!parser_done(parser) && parser_peek(parser)->type == TOK_CARET){
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_unary(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = postfix;
		binop->as.binop.right = right;
		return binop;
	}
	return postfix;
}

AST *parse_postfix_tail(Arena *pa, AST *parent, Parser *parser, AST *target){
	if(parser_done(parser) || parser_peek(parser)->type != TOK_DOT) return target;
	parser_eat(TOK_DOT, parser);
	AST *id = parse_id(pa, parent, parser);
	if(id == NULL) return NULL;
	AST *postfix = ast_make_postfix(pa, id->as.literal.value.id);
	postfix->as.postfix_op.target = target;
	return parse_postfix_tail(pa, parent, parser, postfix);
}

AST *parse_postfix(Arena *pa, AST *parent, Parser *parser){
	AST *primary = parse_primary(pa, parent, parser);
	if(!primary) return NULL;
	return parse_postfix_tail(pa, parent, parser, primary);
}

AST *parse_additive(Arena *pa, AST *parent, Parser *parser){
	AST *left = parse_term(pa, parent, parser);
	if(!left) return NULL;
	while(!parser_done(parser) && (
			parser_peek(parser)->type == TOK_PLUS ||
			parser_peek(parser)->type == TOK_MINUS)
		)
	{
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_term(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = left;
		binop->as.binop.right = right;
		left = binop;
	}
	return left;
}

AST *parse_expression(Arena *pa, AST *parent, Parser *parser){
	AST *left = parse_additive(pa, parent, parser);
	if(!left) return NULL;
	while(!parser_done(parser) && (
			parser_peek(parser)->type == TOK_DBL_EQ ||
			parser_peek(parser)->type == TOK_LT     ||
			parser_peek(parser)->type == TOK_GT     ||
			parser_peek(parser)->type == TOK_LEQ    ||
			parser_peek(parser)->type == TOK_GEQ)
		)
	{
		Token *op_tok         = parser_eat(parser_peek(parser)->type, parser);
		if(!op_tok) return NULL;
		AST   *binop          = ast_make_binop(pa, op_from_tok(op_tok->type));
		AST   *right          = parse_additive(pa, parent, parser);
		if(!right)  return NULL;
		binop->as.binop.left  = left;
		binop->as.binop.right = right;
		left = binop;
	}
	return left;
}


void parse_params(Arena *pa, AST *fn_def, Parser *parser){
	fn_def->as.fn_def.num_args = 0;
	if(parser_peek(parser)->type != TOK_ID) return;

	AST *tail = NULL;

	while(parser_peek(parser)->type == TOK_ID){
		Token *id_tok = parser_eat(TOK_ID, parser);
		if(id_tok == NULL) return;

		AST *node  = ast_make_id(pa, strlen(id_tok->value));
    strcpy(node->as.literal.value.id, id_tok->value);
		node->next = NULL;

		if(tail == NULL){
			fn_def->as.fn_def.params = node;
		}else {
			tail->next = node;
		}

		tail = node;
		fn_def->as.fn_def.num_args++;

		if(parser_peek(parser)->type != TOK_COMMA){
			return;
		} else parser_eat(TOK_COMMA, parser);
	}

}

AST *parse_block(Arena *pa, AST *parent, Parser *parser){
  if(parser_eat(TOK_LCURLY, parser) == NULL) return NULL;
  AST *list = parse_statement_list(pa, parent, parser);
	if(!list) return NULL;
  if(parser_eat(TOK_RCURLY, parser) == NULL) return NULL;
  return list;
}

AST *parse_assignment(Arena *pa, AST *parent, Parser *parser){
	size_t save    = parser->next;
  Token *current = NULL;
  AST   *stmt    = NULL;
  if((current = parser_peek(parser)) && 
      parser_keyword_eat("let", parser))
  {
    stmt = ast_make_statement(pa, STATEMENT_DECL_ASSIGN);
  }else{
		parser->next = save;
    stmt = ast_make_statement(pa, STATEMENT_ASSIGN);
  }

  Token *id = NULL;
  if((id = parser_eat(TOK_ID, parser)) == NULL) return NULL;

  stmt->as.statement.dst  = arena_alloc(pa, strlen(id->value) + 1);
  strcpy(stmt->as.statement.dst, id->value);

  if((id = parser_eat(TOK_EQ, parser)) == NULL) return NULL;

  AST *expr               = parse_expression(pa, stmt, parser);
	if(!expr) return NULL;
  stmt->as.statement.expr = expr;

  if(!parser_eat_stmt_end(parser)) return NULL;
  return stmt;
}

AST *parse_for_stmt(Arena *pa, AST *parent, Parser *parser){
  if(parser_keyword_eat("for", parser) == NULL) return NULL;

  AST *stmt   = ast_make_statement(pa, STATEMENT_FOR);

  AST *id     = parse_id(pa, parent, parser);
  if(!id) return NULL;

  if(parser_keyword_eat("in", parser) == NULL) return NULL;

  AST *expr   = parse_expression(pa, stmt, parser);
  if(!expr) return NULL;

  AST *block  = parse_block(pa, stmt, parser);
  if(!block)  return NULL;

  stmt->as.for_stmt.id    = id;
  stmt->as.for_stmt.expr  = expr;
  stmt->as.for_stmt.block = block;

  if(!parser_eat_stmt_end(parser)) return NULL;
  return stmt;
}

AST *parse_if_stmt(Arena *pa, AST *parent, Parser *parser){
    if(parser_keyword_eat("if", parser)) {
      AST *stmt                     = ast_make_statement(pa, STATEMENT_IF);
      AST *condition                = parse_expression(pa, stmt, parser);
      AST *if_block                 = parse_block(pa, stmt, parser);

			if(!condition) return NULL;
			if(!if_block)  return NULL;

      stmt->as.if_stmt.condition    = condition;
      stmt->as.if_stmt.if_block     = if_block;
      stmt->as.if_stmt.else_is_elif = 0;

      AST *current = stmt;
      Token *tok   = NULL;

      if((tok = parser_peek(parser)) && 
          parser_keyword_match("elif", tok))
      {
        parser_eat(TOK_KEYWORD, parser);

        AST *elif_stmt                   = ast_make_statement(pa, STATEMENT_IF);
        elif_stmt->parent                = current;

				AST *elif_condition              = parse_expression(pa, elif_stmt, parser);
				AST *elif_block                  = parse_block(pa, elif_stmt, parser);

				if(!elif_condition) return NULL;
				if(!elif_block)     return NULL;

        elif_stmt->as.if_stmt.condition  = elif_condition;
        elif_stmt->as.if_stmt.if_block   = elif_block;

        current->as.if_stmt.else_block   = elif_stmt;
        current->as.if_stmt.else_is_elif = 1;
        current                          = elif_stmt;

      }

			if((tok = parser_peek(parser)) && 
					parser_keyword_match("else", tok))
			{
        parser_eat(TOK_KEYWORD, parser);
				AST *else_block                  = parse_block(pa, stmt, parser);

				if(!else_block) return NULL;

        current->as.if_stmt.else_block   = else_block;
        current->as.if_stmt.else_is_elif = 0;
      }

			if(!parser_eat_stmt_end(parser)) return NULL;
      return stmt;
    }
    return NULL;
}

AST *parse_return_stmt(Arena *pa, AST *parent, Parser *parser){
    if(parser_keyword_eat("return", parser)) {
			size_t save             = parser->next;
      AST *stmt               = ast_make_statement(pa, STATEMENT_RETURN);
      AST *expr               = parse_expression(pa, stmt, parser);
			if(!expr) parser->next  = save;
			stmt->as.statement.expr = expr;
			if(!parser_eat_stmt_end(parser)) return NULL;
      return stmt;
    }
    return NULL;
}

AST *parse_fn_def(Arena *pa, AST *parent, Parser *parser){
  if(parser_keyword_eat("fn", parser) == NULL) return NULL;
  Token *id = NULL;

  AST *stmt = ast_make_statement(pa, STATEMENT_FUNCTION_DEF);
  if((id = parser_eat(TOK_ID, parser)) == NULL) return NULL;

  if(!parser_eat(TOK_LPAREN, parser)) return NULL;

  size_t len              = strlen(id->value);
  stmt->as.fn_def.id      = arena_alloc(pa, len + 1);
  strcpy(stmt->as.fn_def.id, id->value);

  stmt->as.fn_def.id[len] = '\0';
  parse_params(pa, stmt, parser);

  if(!parser_eat(TOK_RPAREN, parser)) return NULL;

	AST *block = parse_block(pa, stmt, parser);
	if(!block) return NULL;
  stmt->as.fn_def.block = block;

	if(!parser_eat_stmt_end(parser)) return NULL;
  return stmt;
}

AST *parse_expr_stmt(Arena *pa, AST *parent, Parser *parser){
  AST *expr = parse_expression(pa, parent, parser);
  if(expr == NULL) return NULL;
	if(!parser_eat_stmt_end(parser)) return NULL;
  return expr;
}

AST *parse_statement(Arena *pa, AST *parent, Parser *parser){
  size_t save = parser->next;
	Token *current = parser_peek(parser);
  AST *stmt = NULL;

  if(
      (parser->next = save, (stmt = parse_assignment(pa, parent, parser)))  ||
      (parser->next = save, (stmt = parse_return_stmt(pa, parent, parser))) ||
      (parser->next = save, (stmt = parse_for_stmt(pa, parent, parser)))    ||
      (parser->next = save, (stmt = parse_if_stmt(pa, parent, parser)))     ||
      (parser->next = save, (stmt = parse_fn_def(pa, parent, parser)))      ||
      (parser->next = save, (stmt = parse_expr_stmt(pa, parent, parser)))
    ) return stmt;

  return NULL;
}

AST *parse_statement_list(Arena *pa, AST *parent, Parser *parser){
	AST *stmt_list = ast_make_statement_list(pa);
	AST *tail = NULL;

  while(!parser_done(parser))
  {

    while(!parser_done(parser) && parser_peek(parser)->type == TOK_NL) {
      parser_eat(TOK_NL, parser);
    }

    if(parser_done(parser) || parser_peek(parser)->type == TOK_RCURLY) {
      break;
    }

    size_t before = parser->next;

    AST *stmt = parse_statement(pa, parent, parser);

    if (stmt == NULL || parser->next == before) {
      return NULL;
    }

    stmt->parent = parent;

    if (tail) tail->next = stmt;
    else stmt_list->as.statement_list.first = stmt;
		stmt_list->as.statement_list.count++;

    tail = stmt;
  }

	return stmt_list;
}



int parse_tokens(Arena *pa, AST *ast, Token *tokens, size_t num_tokens){
	ast->parent = NULL;
	ast->type = AST_PROGRAM;

	Parser parser;
	parser.num_tokens = num_tokens;
	parser.tokens     = tokens;
	parser.next       = 0;

	AST *list = parse_statement_list(pa, ast, &parser);
	ast->as.program.statement_list = list;

	if(!parser_done(&parser)) return 1;

	return list == NULL ? 1 : 0;
}

void fill_indent(size_t level, char *buf){
	size_t indent_size = 2;
	size_t total_spaces = indent_size * level;
	for(size_t i = 0; i < total_spaces; i++){
		buf[i] = ' ';
	}
	buf[total_spaces] = '\0';
}

void ast_print(AST *ast, int level){
	char buf[256];
	fill_indent(level, buf);
	switch(ast->type){
		case AST_PROGRAM:
			{
				printf("%sPROGRAM:\n", buf);
				AST *stmt = ast->as.program.statement_list;
				while(stmt != NULL){
					ast_print(stmt, level + 1);
					stmt = stmt->next;
				}
				break;
			}
		case AST_STATEMENT_LIST:
			{
				printf("%sSTMT_LIST:\n", buf);
        for(
            AST* node = ast->as.statement_list.first;
            node != NULL;
            node = node->next
          )
        {
					ast_print(node, level + 1);
        }
				break;
			}
		case AST_STATEMENT:
			{
				printf("%sSTMT:\n", buf);
				fill_indent(level + 1, buf);
				printf("%sSRC:  %s\n", buf, ast->as.statement.dst);
				printf("%sEXP:\n", buf);
				ast_print(ast->as.statement.expr, level + 2);
				break;
			}
		case AST_ARRAY:
			{
				printf("%sARRAY:\n", buf);
        for(
            AST* node = ast->as.array.first;
            node != NULL;
            node = node->next
          )
        {
					ast_print(node, level + 1);
        }
				break;
			}
		case AST_ROW:
			{
				printf("%sROW:\n", buf);
        for(
            AST* node = ast->as.row.first;
            node != NULL;
            node = node->next
          )
        {
					ast_print(node, level + 1);
        }
				break;
			}
		case AST_ID:
			{
        printf("%sID: %s\n", buf, ast->as.literal.value.id);
				break;
			}
		case AST_LITERAL:
			{
				if(ast->as.literal.valueType == VALUE_STRING){
					printf("%sLIT: %s\n", buf, ast->as.literal.value.str);
				} else if(ast->as.literal.valueType == VALUE_NUMBER){
					printf("%sLIT: %.2f\n", buf, ast->as.literal.value.number);
				}
				break;
			}
		case AST_TUPLE:
			{
				printf("%sTUPLE[%zu]:\n", buf, ast->as.tuple.size);
        for(
            AST* node = ast->as.tuple.first;
            node != NULL;
            node = node->next
          )
        {
					ast_print(node, level + 1);
        }
				break;
			}
		case AST_INDEXING:
			{
				printf("%sINDEXING[%s]:\n",
						buf,
						ast->as.indexing.var_name);
				ast_print(ast->as.indexing.index_expr, level + 1);
				break;
			}
		case AST_FN_CALL:
			{
				printf("%sFN[%s]:\n", buf, ast->as.fn_call.fn_name);
				AST *arg = ast->as.fn_call.first_arg;
				while(arg != NULL){
					ast_print(arg, level + 1);
					arg = arg->next;
				}
				break;
			}
		case AST_BINARY_OP:
			{
				char opBuf[128];
				op_type_repr(ast->as.binop.op, opBuf);
				printf("%sBINOP[%s]:\n", buf, opBuf);
				fill_indent(level + 1, buf);
				printf("%sLEFT:\n", buf);
				ast_print(ast->as.binop.left, level + 2);
				fill_indent(level + 1, buf);
				printf("%sRIGHT:\n", buf);
				ast_print(ast->as.binop.right, level + 2);
				break;
			}
		case AST_POSTFIX_OP:
			{
				char opBuf[128];
				printf("%sPOSTFIX[%s]:\n", buf, ast->as.postfix_op.op);
				fill_indent(level + 1, buf);
				printf("%sTARGET:\n", buf);
				ast_print(ast->as.postfix_op.target, level + 2);
				break;
			}
	}
}

//============================== 
//
//    Environment
//
//============================== 

typedef struct {
	char  *name;
	Value *value;
} Variable;

typedef struct {
	size_t failed_asserts;
	size_t runtime_errors;
} RunStats;

typedef struct Environment Environment;
struct Environment {
  Arena       *persistent;
  Arena       *frame;
	Variable    *vars;
	size_t      num_vars;
	size_t      vars_capacity;
	Environment *parent;
	RunStats    *run_stats;
	char        *exe_path;
};

// -------- Runtime errors ------

void runtime_error(Environment *e, const char *fmt, ...){
	va_list args;
	va_start(args,fmt);
	fprintf(stderr, "Error: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	e->run_stats->runtime_errors++;
}

void runtime_warning(const char *fmt, ...){
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "Warning: ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

void initialize_native_fns(Environment *e);
void env_init(Environment *e, Arena *pa){
  e->persistent    = pa;
	e->vars          = arena_alloc(pa, ENV_INITIAL_VARS * sizeof(Variable));
  e->vars_capacity = ENV_INITIAL_VARS;
	e->num_vars      = 0;
	e->parent        = NULL;
}


Environment env_create(Arena *pa, Arena *fa, RunStats *run_stats, char* exe_path){
	Environment e;
	env_init(&e, pa);
  e.frame     = fa;
	e.run_stats = run_stats;
	e.exe_path  = exe_path;
  initialize_native_fns(&e);
	return e;
}

Environment * env_root(Environment *e){
  while(e->parent) e = e->parent;
  return e;
}

Arena *env_storage(Environment *e) {
  return e->parent ? e->frame : e->persistent;
}

int env_variable_idx(Environment *e, const char* name){
	for(int i = 0; i < e->num_vars; i++){
		if(strcmp(name, e->vars[i].name) == 0) return i;
	}
	return -1;
}


Variable *env_get_variable_from_idx(Environment *e, int idx){
	if(idx < 0) return NULL;
	if(idx >= e->num_vars) return NULL;
	return &e->vars[idx];
}

Variable *env_find_local(Environment *e, const char* name){
	if(e == NULL) return NULL;
	int i = env_variable_idx(e, name);
	if(i >= 0) return env_get_variable_from_idx(e, i);
	return NULL;

}
Variable *env_find_variable(Environment *e, const char* name){
	if(e == NULL) return NULL;

	for(
			Environment *env  = e;
			env              != NULL;
			env               = env->parent
	){
		int i = env_variable_idx( env, name);
		if(i >= 0) return env_get_variable_from_idx(env, i);
	}

	return NULL;

}

int env_reserve_vars(Environment *e, size_t needed){
  if(needed <= e->vars_capacity) return 1;
  size_t new_capacity = e->vars_capacity * 2;
  while(new_capacity < needed) new_capacity *= 2;

  Variable *new_vars = arena_alloc(env_storage(e), new_capacity * sizeof(Variable));
  if(new_vars == NULL) return 0;

  memcpy(new_vars, e->vars, e->num_vars * sizeof(Variable));
  e->vars          = new_vars;
  e->vars_capacity = new_capacity;
  return 1;
}



void env_alloc_variable(Environment *e, const char* name, Value *value){
  if(!env_reserve_vars(e, e->num_vars + 1)){
    runtime_error(e, "Out of memory allocating variable %s", name);
    return; 
  }
  Arena *storage  = env_storage(e);

	Variable *var   = &e->vars[e->num_vars];
	size_t name_len = strlen(name);

  var->name       = arena_alloc(storage, name_len + 1);
	strcpy(var->name, name);

  var->value      = arena_alloc(storage, sizeof(Value));
	*var->value     = *value;

	e->num_vars++;

	// TODO: Implement reallocation
}

void env_set_variable(Environment *e, const char* name, Value *value){
	int idx =  env_variable_idx(e, name);
	if(idx < 0){
		env_alloc_variable(e, name, value);
		return;
	}

	Variable *var = &e->vars[idx];
	*var->value   = *value;
}

void  env_list_variables(Environment  *e){
	for(size_t i = 0; i < e->num_vars; i++){
		printf("%s\n", e->vars[i].name);
	}
}


//============================== 
//
//    Interpreting
//
//============================== 


Value make_num(double num){
	Value v = {
		.valueType    = VALUE_NUMBER,
		.value.number = num
	};
	return v;
}
Value make_complex(double complex num){
	Value v = {
		.valueType            = VALUE_COMPLEX,
		.value.complex_number = num
	};
	return v;
}

Value make_tensor(Tensor tens){
	Value t = {
		.valueType    = VALUE_TENSOR,
		.value.tensor = tens
	};
	return t;
}

Value make_function(Function fun){
	Value v = {
		.valueType = VALUE_FUNCTION,
		.value.fun = fun
	};
	return v;
}

Value value_clone(Arena *a, Value *v);


Tensor clone_tensor(Arena *a, Tensor *old){
  Tensor t = {0};
	if(tensor_init_like(a, &t, old, old->dtype) != 0){
		t.data.re = NULL;
		t.data.cx = NULL;
		t.count   = 0;
		t.ndim    = 0;
		return t;
	}
	if(old->dtype == DTYPE_REAL){
		memcpy(t.data.re, old->data.re, old->count * sizeof(double));
	}else{
		memcpy(t.data.cx, old->data.cx, old->count * sizeof(double complex));
	}
  return t;
}


Tuple clone_tuple(Arena *a, Tuple *src){
  Tuple dst;

  dst.size  = src->size;
  dst.items = arena_alloc(a, src->size * sizeof(Value));

  for(size_t i = 0; i < src->size; i++){
    dst.items[i] = value_clone(a, &src->items[i]);
  }
  return dst;
}

Polynomial clone_poly(Arena *a, Polynomial *p){
  Polynomial poly;
  poly.num_coeffs = p->num_coeffs;
  poly.deg        = p->deg;
  poly.coeffs     = arena_alloc(a, poly.num_coeffs * sizeof(double));
  memcpy(poly.coeffs, p->coeffs, poly.num_coeffs * sizeof(double));
  return poly;
}

TransferFunction clone_tf(Arena *a, TransferFunction *tf){
	Polynomial num = clone_poly(a, &tf->num);
	Polynomial den = clone_poly(a, &tf->den);
	TransferFunction tf_new = {
		.num = num,
		.den = den,
		.Ts  = tf->Ts
	};
  return tf_new;
}

char *clone_string(Arena *a, char *old){
  size_t len = strlen(old);
  char *new = arena_alloc(a, len + 1);
  strcpy(new, old);
  return new;
}

Value value_clone(Arena *a, Value *v){
  Value copy = *v;
  switch(v->valueType){
    case VALUE_COMPLEX: 
      {
        copy.value.complex_number = v->value.complex_number;
        break;
      }
    case VALUE_NUMBER: 
      {
        copy.value.number = v->value.number;
        break;
      }
    case VALUE_ID: 
      {
        copy.value.id = clone_string(a, v->value.id);
        break;
      }
    case VALUE_STRING: 
      {
        copy.value.str = clone_string(a, v->value.str);
        break;
      }
    case VALUE_TENSOR: 
      {
        copy.value.tensor = clone_tensor(a, &v->value.tensor);
        break;
      }
    case VALUE_TUPLE: 
      {
        copy.value.tuple = clone_tuple(a, &v->value.tuple);
        break;
      }
    case VALUE_POLYNOMIAL: 
      {
        copy.value.poly = clone_poly(a, &v->value.poly);
        break;
      }
    case VALUE_TRANSFER_FUNCTION: 
      {
				copy.value.tf = clone_tf(a, &v->value.tf);
        break;
      }
    default: break;
  }
  return copy;
}

Polynomial poly_new(Arena *a, double *src_coeffs, size_t num_coeffs){
	size_t deg = num_coeffs - 1;
	double *coeffs = arena_alloc(a, num_coeffs * sizeof(double));
  memcpy(coeffs, src_coeffs, num_coeffs * sizeof(double));
	Polynomial p = {
		.coeffs     = coeffs,
		.deg        = deg,
		.num_coeffs = num_coeffs
	};
	return p;
}

Value make_poly(Polynomial poly){
	Value v = {
		.valueType  = VALUE_POLYNOMIAL,
		.value.poly = poly
	};
	return v;
}

Value make_tuple(Tuple tuple){
	Value v = {
		.valueType   = VALUE_TUPLE,
		.value.tuple = tuple
	};
	return v;
}


Value make_transfer_function(TransferFunction tf){
	Value v = {
		.valueType = VALUE_TRANSFER_FUNCTION,
		.value.tf  = tf
	};
	return v;
}

Value make_string(char *str){
	Value v = {
		.valueType = VALUE_STRING,
		.value.str = str
	};
	return v;
}



Value make_nan(){
	Value v = {
		.valueType    = VALUE_NAN,
		.value.number = 0
	};
	return v;
}

//============================
//
//   Transformations
//
//============================

Polynomial poly_from_num(Arena *a, double n){
	Polynomial poly = alloc_poly(a, 0);
	poly.coeffs[0] = n;
	return poly;
}

Tensor scale_tensor(Arena *a, Tensor *t, double x){
	Tensor tens_new = clone_tensor(a, t);
	if(tens_new.dtype == DTYPE_REAL){
		for(size_t i = 0; i < tens_new.count; i++) tens_new.data.re[i] *= x;
	}else{
		for(size_t i = 0; i < tens_new.count; i++) tens_new.data.cx[i] *= x;
	}
	return tens_new;
}

Polynomial scale_poly(Arena *a, Polynomial *p, double x){
	Polynomial poly = alloc_poly(a, p->deg);
	for(size_t i = 0; i < poly.num_coeffs; i++){
		poly.coeffs[i] = p->coeffs[i] * x;
	}
	return poly;
}


TransferFunction tf_invert(Arena *a, TransferFunction *tf){
	TransferFunction tf_new = {
		.num = clone_poly(a, &tf->den),
		.den = clone_poly(a, &tf->num),
		.Ts  = tf->Ts
	};
	return tf_new;
}

TransferFunction scale_tf(Arena *a, TransferFunction *tf, double x){
	TransferFunction new_tf = clone_tf(a, tf);
	new_tf.num = scale_poly(a, &new_tf.num, x);
	return new_tf;
}

TransferFunction tf_from_poly(Arena *a, Polynomial *p){
	Polynomial num = alloc_poly(a, p->deg);
	memcpy(num.coeffs, p->coeffs, p->num_coeffs * sizeof(double));
	Polynomial den = alloc_poly(a, 0);
	den.coeffs[0] = 1;
	TransferFunction tf = {
		.num = num,
		.den = den,
	};
	return tf;
}

TransferFunction tf_from_num(Arena *a, double n){
	Polynomial num_poly = poly_from_num(a, n);
	return tf_from_poly(a, &num_poly);
}

char *cat_strings(Arena *a, char *str1, char *str2){
  size_t len1    = strlen(str1);
  size_t len2    = strlen(str2);
  size_t new_len = len1 + len2;
  char *new_str  = alloc_string(a, new_len);

  for(size_t i = 0; i < len1; i++){
    new_str[i] = str1[i];
  }
  for(size_t i = len1; i < new_len; i++){
    new_str[i] = str2[i - len1];
  }
  new_str[new_len] = '\0';

  return new_str;
}

char *string_from_num(Arena *a, double num){
  char *num_str = arena_alloc(a, 32);
  format_number(num_str, 32, num, DISPLAY_SIG);
  return num_str;
}

//============================
//
//   Algorithms
//
//============================

int modulo(int a, int b){
	int r = a % b;
	return r < 0 ? r + b : r;
}

int poly_is_zero(Polynomial *p){
  for(size_t i = 0; i < p->num_coeffs; i++) if(p->coeffs[i] != 0) return 0;
  return 1;
}


double cancel_add(double a, double b){
	double s = a + b;
	if(fabs(s) <= POLY_CANCEL_TOL * (fabs(a) + fabs(b))) return 0.0;
	return s;
}

void poly_update_degree(Polynomial *p){
  int i = p->num_coeffs - 1;
  while(p->coeffs[i] == 0.0 && i > 0){
		i--;
	}
  p->num_coeffs = i + 1;
  p->deg        = i;
}

Polynomial poly_monic(Arena *a, Polynomial p){
  return scale_poly(a, &p, 1.0/p.coeffs[p.num_coeffs - 1]);
}

int poly_divmod(Arena *arena, Polynomial a, Polynomial b, Polynomial *q_out, Polynomial *r_out){
	poly_update_degree(&a);
	poly_update_degree(&b);

	if(poly_is_zero(&b)){
		return 1;
	}

	if(a.deg < b.deg || poly_is_zero(&a)){
		if(q_out) *q_out = poly_from_num(arena, 0.0);
		if(r_out) *r_out = clone_poly(arena, &a);
		return 0;
	}

	Polynomial q      = alloc_poly(arena, a.deg - b.deg);
	Polynomial r      = clone_poly(arena, &a);
	double lead_coeff = b.coeffs[b.num_coeffs - 1];

	while(r.deg >= b.deg && !poly_is_zero(&r)){
		size_t q_deg    = r.deg - b.deg;
		double coef     = r.coeffs[r.deg] / lead_coeff;
		q.coeffs[q_deg] = coef;

		for(int j = 0; j < b.num_coeffs; j++){
			r.coeffs[j + q_deg] = cancel_add(r.coeffs[j + q_deg], -coef * b.coeffs[j]);
		}

		// To avoid floating point issues in the subtraction:
		r.coeffs[r.deg] = 0.0;
		poly_update_degree(&r);
	}

	if(q_out) *q_out = q;
	if(r_out) *r_out = r;

	return 0;
}

double complex  poly_horner_cx(Polynomial p, double complex x){

  // P(x) = a0 + a1*x + a2*x^2 + ... + an*x^n
  // P(x) = a0 + x * (a1 + x * (a2 + x * (a3) ))

  if(p.deg == 0) return p.coeffs[0];

  double complex val = p.coeffs[p.deg];
  for (int i = (int)p.deg - 1; i >= 0; i--) {
    val = val * x + p.coeffs[i];
  }

  return val;

}

double poly_horner(Polynomial p, double x){
	return creal(poly_horner_cx(p, x));
}


void poly_horner_with_derivative(
    Polynomial p,
    double complex x,
    double complex *val_out,
    double complex *der_out
    )
{

  // P(x)  = a0 + a1*x + a2*x^2 + ... + an*x^n
  // P(x)  = a0 + x * (a1 + x * (a2 + x * (a3) ))
  // P'(x) = a1 + 2a2*x + 3a3*x^2 + 4a4x^3 + ... + n * an*x^n - 1
  // P'(x) = a1 + x * (2a2 + x * (3a3 + x * (4a4)))

  if(p.deg == 0) { 
    if(val_out) *val_out = p.coeffs[0];
    if(der_out) *der_out = 0;
    return;
  }

  double complex val = p.coeffs[p.deg];
  double complex der = 0.0;

  for (int i = (int)p.deg - 1; i >= 0; i--) {
    der = der * x + val;
    val = val * x + p.coeffs[i];
  }

  if(val_out) *val_out = val;
  if(der_out) *der_out = der;

}


void poly_aberth_solve(Arena *ta, Polynomial poly, double complex *roots) {
    double *coeffs = poly.coeffs;
    size_t deg = poly.deg;

    if (deg == 0 || coeffs == NULL || roots == NULL ||
        coeffs[deg] == 0.0) {
        return;
    }

    // Cauchy radius
    double radius = 0.0;

    for (size_t i = 0; i < deg; i++) {
        double ratio = fabs(coeffs[i] / coeffs[deg]);
        if (ratio > radius) radius = ratio;
    }

    // Initial estimates on a circle
    double M = radius + 1.0;
    double pi = acos(-1.0);
		double offset = 0.1;

    for (size_t i = 0; i < deg; i++) {
        double theta = 2.0 * pi * i / deg + offset;
        roots[i] = M * cexp(I * theta);
    }

    // Scratch memory
    size_t scratch = arena_save(ta);
    double complex *temp = arena_alloc(
        ta, deg * sizeof(double complex)
    );

    // Aberth-Ehrlich iterations
    bool converged = false;

		int iter = 0;
    for (iter = 0; iter < ABERTH_MAX_ITER; iter++) {
        double max_step = 0.0;

        for (size_t i = 0; i < deg; i++) {
            double complex zi = roots[i];
            double complex pz, der;
            double complex sum = 0.0;

            poly_horner_with_derivative(poly, zi, &pz, &der);

            if (pz == 0.0) {
                temp[i] = 0.0;
                continue;
            }

            // Aberth correction
            for (size_t j = 0; j < deg; j++) {
                if (i == j) continue;

                double complex diff = zi - roots[j];

                if (diff == 0.0) {
                    goto cleanup;
                }

                sum += 1.0 / diff;
            }

            double complex denom = der - pz * sum;

            if (denom == 0.0) {
                goto cleanup;
            }

            double complex delta = pz / denom;

            if (!isfinite(creal(delta)) ||
                !isfinite(cimag(delta))) {
                goto cleanup;
            }

            temp[i] = -delta;

            double step = cabs(delta) / (1.0 + cabs(zi));
            if (step > max_step) max_step = step;
        }

        // Simultaneous update
        for (size_t i = 0; i < deg; i++) {
            roots[i] += temp[i];
        }

        // Convergence
        if (max_step < ABERTH_TOL) {
            converged = true;
            break;
        }
    }

cleanup:
		if (!converged) {
			runtime_warning("Aberth solver did not converge (degree %zu, %d iterations)", deg, iter);
		}

    arena_restore(ta, scratch);
}


Polynomial poly_scaled_to_one(Arena *arena, Polynomial *p){
	double max = 0.0;
	for(size_t i = 0; i < p->num_coeffs; i++) max = MAX(max, fabs(p->coeffs[i]));
	if(max == 0.0) return clone_poly(arena, p);
	return scale_poly(arena, p, 1.0 / max);
}


double poly_max_abs(const Polynomial *p){
	double m = 0;
	for(size_t i = 0; i < p->num_coeffs; i++) m = fmax(m, fabs(p->coeffs[i]));
	return m;
}

Polynomial poly_gcd(Arena *arena, Polynomial a, Polynomial b){
	// Euclid'a gcd algorithm for polynomials
	a = poly_scaled_to_one(arena, &a);
	b = poly_scaled_to_one(arena, &b);
	poly_update_degree(&a);
	poly_update_degree(&b);
	Polynomial q;
	Polynomial r;
	double scale = MAX(poly_max_abs(&a), poly_max_abs(&b));   // 1 after the scaling above
	while (!poly_is_zero(&b)) {
		poly_divmod(arena, a, b, &q, &r);
		a = b;
		if(poly_max_abs(&r) <= GCD_TOL * scale) break;
		b = r;
	}
	return poly_monic(arena, a);
}

TransferFunction tf_simplify(Arena *a, TransferFunction *tf){
	Polynomial g = poly_gcd(a, tf->num, tf->den);

	Polynomial reduced_num = tf->num;
	Polynomial reduced_den = tf->den;
	poly_divmod(a, tf->num, g, &reduced_num, NULL);
	poly_divmod(a, tf->den, g, &reduced_den, NULL);

	TransferFunction new_tf = {
		.num = clone_poly(a, &reduced_num),
		.den = clone_poly(a, &reduced_den),
		.Ts  = tf->Ts
	};

	return new_tf;
}

TransferFunction tf_normalize(Arena *a, TransferFunction *tf){
	double lead = tf->den.coeffs[tf->den.num_coeffs - 1];

	TransferFunction new_tf = {
		.num = scale_poly(a, &tf->num, 1.0 / lead),
		.den = scale_poly(a, &tf->den, 1.0 / lead),
		.Ts  = tf->Ts
	};

	return new_tf;
}


double complex *tf_poles(Arena *ta, TransferFunction tf){
	Polynomial den = tf.den;
	double complex *roots = arena_alloc(ta, den.deg * sizeof(double complex));
	poly_aberth_solve(ta, den, roots);
	return roots;
}

//============================== 
//
//    ODE
//
//============================== 

typedef void (*ODEFn)(double t, double *x, double *dxdt, size_t n, void* data);

void rk4_step(ODEFn f, double t, double *y, double h, size_t n, double *work, void *data){
	double *k   = work;
	double *tmp = work + n;
	double *acc = work + 2*n;

	f(t, y, k, n, data);                                  // k1
	for(size_t i = 0; i < n; i++){ acc[i] = k[i];      tmp[i] = y[i] + h/2*k[i]; }
	f(t + h/2, tmp, k, n, data);                          // k2
	for(size_t i = 0; i < n; i++){ acc[i] += 2*k[i];   tmp[i] = y[i] + h/2*k[i]; }
	f(t + h/2, tmp, k, n, data);                          // k3
	for(size_t i = 0; i < n; i++){ acc[i] += 2*k[i];   tmp[i] = y[i] + h*k[i]; }
	f(t + h, tmp, k, n, data);                            // k4
	for(size_t i = 0; i < n; i++){ y[i] += h/6 * (acc[i] + k[i]); }
}

// x' = Ax + Bu
// y  = Cx + Du
// For a SISO system, which is what is
// currently supported, u is simply a
// number and B = [0,0,...,1]T
// Likewise, C is a row vector and D is 
// also a number.

typedef struct {
	size_t n;
	double *a; // Denominator coefficients [a0,...,an-1]. Assumes that it is normalized.
	double *c; 
	double d;
	double h;  // Integrator step
} LtiSystem;

// h = 0.05 / fastest pole magnitude. 0.5 already keeps RK4 stable, but
// gives errors around 1e-4; 0.05 brings them down to around 1e-8.
double lti_step_size(Arena *a, TransferFunction monic){
	size_t n = monic.den.deg;
	double fastest = 0;
	SCRATCH_SCOPE(a){
		double complex *p = tf_poles(a, monic);
		for(size_t i = 0; i < n; i++) if(cabs(p[i]) > fastest) fastest = cabs(p[i]);
	}       
	return fastest > 0 ? 0.05 / fastest : 0.01;
}

int lti_from_tf(Arena *a, Environment *e, TransferFunction tf, LtiSystem *sys){
	if(tf.num.deg > tf.den.deg){
		runtime_error(e, "Transfer function is improper");
		return 0;
	}
	TransferFunction m = tf_normalize(a, &tf);
	size_t n = m.den.deg;

	sys->n = n;
	sys->a = m.den.coeffs;
	sys->d = n <= m.num.deg ? m.num.coeffs[n] : 0;
	sys->c = arena_alloc(a, n * sizeof(double));

	if(!sys->c) return 0; // Allocation failure

	for(size_t i = 0; i < n; i++){
		double b  = i <= m.num.deg ? m.num.coeffs[i] : 0;
		sys->c[i] = b - sys->a[i] * sys->d;
	}
	sys->h = lti_step_size(a, m);
	return 1;
}

double lti_output(const LtiSystem *sys, const double *x, double u){
	double y = sys->d * u;
	for(size_t i = 0; i < sys->n; i++) y += sys->c[i] * x[i];
	return y;
}

typedef struct {
	const double *a; // normalized den coeffs a[0..n-1] (a[n] == 1 is implied)
	double        u;  
} LtiCtx;
          
void lti_derivative(double t, double *x, double *dxdt, size_t n, void *data){
	// Last row of the companion matrix (a[n] == 1, see LtiCtx)

	LtiCtx *c = data;
	double last = c->u;
	for(size_t i = 0; i < n; i++) last -= c->a[i] * x[i];
	for(size_t i = 0; i + 1 < n; i++) dxdt[i] = x[i + 1];
	dxdt[n - 1] = last;
}     

int lti_simulate(Arena *a, const LtiSystem *sys, double *x, double u,
		const double *ts, size_t m, double *y){
	double *work = arena_alloc(a, (3 * sys->n) * sizeof(double));
	if(!work) return 0;
	LtiCtx ctx = { .a = sys->a, .u = u };

	if(m == 0) return 1;
	// The input starts at t = 0, and the system is at rest before that.
	// Integrating from 0 (not from ts[0]) keeps y(t) independent of which times are requested
	double t = 0;
	for(size_t k = 0; k < m; k++){
		if(ts[k] < 0){
			y[k] = 0;
			continue;
		}
		double span = ts[k] - t;
		if(sys->n > 0 && span > 0){
			size_t steps = (size_t)ceil(span / sys->h);
			double h = span / steps;
			for(size_t s = 0; s < steps; s++)
				rk4_step(lti_derivative, t + s * h, x, h, sys->n, work, &ctx);
		}
		t = ts[k];
		y[k] = lti_output(sys, x, u);
	}
	return 1;
}

double *step_response(Arena *a, Environment *e, TransferFunction tf, const double *ts, size_t m){
	LtiSystem sys;          
	if(!lti_from_tf(a, e, tf, &sys)) return NULL;

	double *x = arena_alloc(a, sys.n * sizeof(double));   // at rest
	double *y = arena_alloc(a, m * sizeof(double));
	if(!x || !y) return NULL;

	if(!lti_simulate(a, &sys, x, 1.0, ts, m, y)) return NULL;
	return y; 
}       

double *impulse_response(Arena *a, Environment *e, TransferFunction tf, const double *ts, size_t
		m){
	LtiSystem sys;
	if(!lti_from_tf(a, e, tf, &sys)) return NULL;
	if(sys.d != 0){
		runtime_error(e, "impulse: system is not strictly proper");
		return NULL;
	}

	double *x = arena_alloc(a, sys.n * sizeof(double));
	double *y = arena_alloc(a, m * sizeof(double));
	if(!x || !y) return NULL;
	x[sys.n - 1] = 1;                    // the impulse kicks the state by B

	if(!lti_simulate(a, &sys, x, 0.0, ts, m, y)) return NULL;
	return y;
}

// ----------------- DT systems -----------------------

double *dt_response(Arena *a, Environment *e, TransferFunction tf, const double *u, size_t
		N){
	// Assume our transfer function is of the form:
	//
	//         B0 + B1*z + B2*z^2 + ... + Bn*z^n
	// TF =   ------------------------------------
	//         A0 + A1*z + A2*z^2 + ... + An*z^n
	//
	// Normalizing for the An term:
	//
	//         b0 + b1*z + b2*z^2 + ... + bn*z^n
	// TF =   ------------------------------------
	//         a0 + a1*z + a2*z^2 + ... + 1*z^n
	//
	// Dividing the numerator and the denominator by z^n
	//
	//         b0*z^(-n) + b1*z^(-n + 1) + ... + bn
	// TF =   ------------------------------------
	//         a0*z^(-n) + a1*z^(-n + 1) + ... + 1
	//
	// Converting it to a FD equation:
	//
	// b0*u[k - n] + b1*u[k - n + 1] + ... + bn*u[k]
	//                    =
	// a0*y[k - n] + a1*y[k - n + 1] + ... + y[k]
	//
	// Or, isolating y[k]:
	//
	// y[k] = b0 * u[k - n] + ... + bn-1*u[k - 1] + bn*u[k]
	//      -(a0 * y[k - n] + ... + an-1*y[k - 1]          )

	TransferFunction norm = tf_normalize(a, &tf);
	size_t deg            = norm.den.deg;

	// y[k] would need u[k + 1], an input from the future
	if(norm.num.deg > deg){
		runtime_error(e, "discrete system is not causal (numerator degree %zu > denominator degree %zu)",
				norm.num.deg, deg);
		return NULL;
	}

	double *y = arena_alloc(a, N * sizeof(double));
	if(!y) return NULL;

	// Starts at k = 0: a biproper system responds to u[0] right away
	for(size_t k = 0; k < N; k++){
		double acc = 0;
		for(size_t i = 0; i <= deg && i <= k; i++){
			size_t c = deg - i;   // Coefficient for delay i
			if(c < norm.num.num_coeffs) acc += norm.num.coeffs[c] * u[k - i]; // Missing = padded with 0
			if(i > 0)                   acc -= norm.den.coeffs[c] * y[k - i];
		}
		y[k] = acc;
	}

	return y;
}

// Sample index for time t. The small nudge keeps 0.3 / 0.1 = 2.9999.. at 3
static size_t dt_sample_index(double t, double Ts){
	return (size_t)floor(t / Ts + 1e-9);
}

// Impulse or step response of a DT system, read at the requested times.
double *dt_response_at(Arena *a, Environment *e, TransferFunction tf, const double *ts, size_t n, bool is_step){
	double *out = arena_alloc(a, n * sizeof(double));
	if(!out) return NULL;
	if(n == 0) return out;

	// Before t = 0 the system is at rest, so out stays 0 there
	if(ts[n - 1] < 0) return out;

	size_t Ns = dt_sample_index(ts[n - 1], tf.Ts) + 1;
	double *u = arena_alloc(a, Ns * sizeof(double));
	if(!u) return NULL;
	u[0] = 1.0;
	if(is_step) for(size_t k = 1; k < Ns; k++) u[k] = 1.0;

	double *y = dt_response(a, e, tf, u, Ns);
	if(!y) return NULL;

	for(size_t j = 0; j < n; j++){
		if(ts[j] >= 0) out[j] = y[dt_sample_index(ts[j], tf.Ts)];
	}
	return out;
}
//=============================================
//
//   Dispatcher types and native functions
//
//=============================================

typedef Value (*NativeValueDispatch)(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent);
typedef Value (*UnaryValueDispatch)(Arena *a, Environment *e, Value *caller, Value *value);

//============================
//
//   Helpers
//
//============================

bool get_opt_string(Environment *e, const char *fn, size_t argc, Value *argv,
                  size_t i, const char *name, const char *def, const char **out){
	if(i >= argc) { *out = def; return true; }
	if(argv[i].valueType != VALUE_STRING){
		runtime_error(e, "%s: %s must be a string", fn, name);
		return false;
	}
	*out = argv[i].value.str;
	return true;
}

bool get_opt_number(Environment *e, const char *fn, size_t argc, Value *argv,
                  size_t i, const char *name, double def, double *out){
	if(i >= argc) { *out = def; return true; }
	if(argv[i].valueType != VALUE_NUMBER){
		runtime_error(e, "%s: %s must be a number", fn, name);
		return false;
	}
	*out = argv[i].value.number;
	return true;
}

bool get_opt_choice(Environment *e, const char *fn, size_t argc, Value *argv,
                  size_t i, const char *name, const char *const *choices, size_t def_idx,
									size_t *out){
	const char *given;
	if(!get_opt_string(e, fn, argc, argv, i, name, NULL, &given)) return false;
	if(given == NULL) { *out = def_idx; return true; } 

	for(size_t k = 0; choices[k] != NULL; k++){
		if(strcmp(given, choices[k]) == 0){ *out = k; return true; }
	}

	// If not found, list the possible choices in the error msg

	char list[256];
	size_t len = 0;
	list[0] = '\0';

	for(size_t k = 0; choices[k] != NULL; k++){
		buf_append(list, sizeof(list), &len, "%s\"%s\"", k ? ", " : "", choices[k]);
	}

	runtime_error(e, "%s: %s must be one of %s (got \"%s\")", fn, name, list, given);
	return false;
}


//============================
//
//   Unary Operations
//
//============================

typedef Value (*UnaryFn)(Arena *a, Value *arg);

Value unop_with_nan(Arena *a, Value *arg){
	return make_nan();
}

Value unop_num_plus(Arena *a, Value *arg){
	return make_num(arg->value.number);
}
Value unop_complex_plus(Arena *a, Value *arg){
	return make_complex(arg->value.complex_number);
}
Value unop_tens_plus(Arena *a, Value *arg){
	return make_tensor(arg->value.tensor);
}
Value unop_poly_plus(Arena *a, Value *arg){
	return make_poly(arg->value.poly);
}
Value unop_tf_plus(Arena *a, Value *arg){
	return make_transfer_function(arg->value.tf);
}

Value unop_num_minus(Arena *a, Value *arg){
	return make_num(-1.0 * arg->value.number);
}
Value unop_complex_minus(Arena *a, Value *arg){
	return make_complex(-1.0 * arg->value.complex_number);
}
Value unop_tens_minus(Arena *a, Value *arg){
  Tensor tens = scale_tensor(a, &arg->value.tensor, -1.0);
	return make_tensor(tens);
}
Value unop_poly_minus(Arena *a, Value *arg){
	return make_poly(scale_poly(a, &arg->value.poly, -1.0));
}
Value unop_tf_minus(Arena *a, Value *arg){
	return make_transfer_function(scale_tf(a, &arg->value.tf, -1.0));
}



static const UnaryFn unop_dispatch[OP_TYPE_LIST_COUNT][VALUE_TYPE_COUNT] = {
	[OP_PLUS] = {
		[VALUE_NUMBER]            = unop_num_plus,
		[VALUE_COMPLEX]           = unop_complex_plus,
		[VALUE_TENSOR]            = unop_tens_plus,
		[VALUE_POLYNOMIAL]        = unop_poly_plus,
    [VALUE_TRANSFER_FUNCTION] = unop_tf_plus,
		[VALUE_NAN]               = unop_with_nan,
    [VALUE_STRING]            = unop_with_nan,
	},

	[OP_MINUS] = {
		[VALUE_NUMBER]            = unop_num_minus,
		[VALUE_COMPLEX]           = unop_complex_minus,
		[VALUE_TENSOR]            = unop_tens_minus,
		[VALUE_POLYNOMIAL]        = unop_poly_minus,
    [VALUE_TRANSFER_FUNCTION] = unop_tf_minus,
		[VALUE_NAN]               = unop_with_nan,
    [VALUE_STRING]            = unop_with_nan,
	},

};

//============================
//
//   Binary Operations
//
//============================


typedef Value (*BinaryFn)(Arena *a, Environment *e, Value *left, Value *right);


Value op_with_nan(Arena *a, Environment *e, Value *left, Value *right){
	return make_nan();
}


double complex as_complex(Value *v){
	return v->valueType == VALUE_COMPLEX ? v->value.complex_number : v->value.number;
}

Value add_scalar_scalar(Arena *a, Environment *e, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number + right->value.number);
	return make_complex(as_complex(left) + as_complex(right));
}
Value sub_scalar_scalar(Arena *a, Environment *e, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number - right->value.number);
	return make_complex(as_complex(left) - as_complex(right));
}
Value mul_scalar_scalar(Arena *a, Environment *e, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number * right->value.number);
	return make_complex(as_complex(left) * as_complex(right));
}
Value div_scalar_scalar(Arena *a, Environment *e, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(left->value.number / right->value.number);
	return make_complex(as_complex(left) / as_complex(right));
}
Value pow_scalar_scalar(Arena *a, Environment *e, Value *left, Value *right){
	if(left->valueType == VALUE_NUMBER && right->valueType == VALUE_NUMBER)
		return make_num(pow(left->value.number , right->value.number));
	return make_complex(cpow(as_complex(left),  as_complex(right)));
}

// Real-specific

Value eq_scalar_scalar(Arena *a, Environment *e, Value *left, Value *right){
	return make_num(as_complex(left) == as_complex(right));
}
Value lt_num_num(Arena *a, Environment *e, Value *left, Value *right){
	return make_num(left->value.number < right->value.number);
}
Value gt_num_num(Arena *a, Environment *e, Value *left, Value *right){
	return make_num(left->value.number > right->value.number);
}
Value leq_num_num(Arena *a, Environment *e, Value *left, Value *right){
	return make_num(left->value.number <= right->value.number);
}
Value geq_num_num(Arena *a, Environment *e, Value *left, Value *right){
	return make_num(left->value.number >= right->value.number);
}

// ------------- Polynomials --------------------

Value add_num_poly(Arena *a, Environment *e, Value *left, Value *right){
		double num       =  left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;
		Polynomial  poly =  left->valueType == VALUE_POLYNOMIAL ? left->value.poly : right->value.poly;

		Polynomial pnew  =  clone_poly(a, &poly);

		pnew.coeffs[0]   += num;
		return make_poly(pnew);
}

Value add_poly_poly(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p1    = left->value.poly;
		Polynomial p2    = right->value.poly;
		size_t max_size  = MAX(p1.num_coeffs, p2.num_coeffs);

		Polynomial pnew = alloc_poly(a, max_size - 1);

		for(size_t i = 0; i < max_size; i++){
			double v1 = 0;
			double v2 = 0;
			if(i < p1.num_coeffs) v1 = p1.coeffs[i];
			if(i < p2.num_coeffs) v2 = p2.coeffs[i];
			pnew.coeffs[i] = cancel_add(v1, v2);
		}

		poly_update_degree(&pnew);
		return make_poly(pnew);
}

Value sub_num_poly(Arena *a, Environment *e, Value *left, Value *right){
		double num          = left->value.number;
		Polynomial  poly    = right->value.poly;

    Polynomial new_poly = scale_poly(a, &poly, -1.0);

		new_poly.coeffs[0] = num + new_poly.coeffs[0];
		return make_poly(new_poly);
}

Value sub_poly_num(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial  poly    = left->value.poly;
		double num          = right->value.number;

    Polynomial new_poly = clone_poly(a, &poly);

		new_poly.coeffs[0] -= num;
		return make_poly(new_poly);
}



Value sub_poly_poly(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p1    = left->value.poly;
		Polynomial p2    = right->value.poly;
		size_t max_size  = MAX(p1.num_coeffs, p2.num_coeffs);

		Polynomial pnew = alloc_poly(a, max_size - 1);

		for(size_t i = 0; i < max_size; i++){
			double v1 = 0;
			double v2 = 0;
			if(i < p1.num_coeffs) v1 = p1.coeffs[i];
			if(i < p2.num_coeffs) v2 = p2.coeffs[i];
			pnew.coeffs[i] = cancel_add(v1, -v2);
		}

		poly_update_degree(&pnew);
		return make_poly(pnew);
}

Value mul_poly_poly(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p1   = left->value.poly;
		Polynomial p2   = right->value.poly;

    size_t new_deg  = p1.deg + p2.deg;
		Polynomial pnew = alloc_poly(a, new_deg);

		for(size_t i = 0; i < p1.num_coeffs; i++){
      for(size_t j = 0; j < p2.num_coeffs; j++){
        pnew.coeffs[i + j] += p1.coeffs[i] * p2.coeffs[j];
      }
		}
    
		return make_poly(pnew);
}

Value mul_num_poly(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p = left->valueType == VALUE_POLYNOMIAL ? left->value.poly : right->value.poly;
		double num   = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;

		Polynomial pnew = scale_poly(a, &p, num);
		return make_poly(pnew);
}


Value div_poly_poly(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p1 = left->value.poly;
		Polynomial p2 = right->value.poly;

		TransferFunction tf  = {
			.num = clone_poly(a, &p1),
			.den = clone_poly(a, &p2),
			.Ts  = 0
		};
    
		return make_transfer_function(tf_simplify(a, &tf));
}


Value ordered_div_num_poly(Arena *a, Environment *e, Value *left, Value *right){
		double num   = left->value.number;
    Polynomial p = right->value.poly; 

		Polynomial pnew = poly_from_num(a, num);

    TransferFunction tf = {
      .num = pnew,
      .den = p
    };

		return make_transfer_function(tf_simplify(a, &tf));
}

Value ordered_div_poly_num(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p = left->valueType == VALUE_POLYNOMIAL ? left->value.poly : right->value.poly;
		double num   = left->valueType == VALUE_NUMBER ? left->value.number : right->value.number;

    Value poly_value = make_poly(p);
    Value inv_num    = make_num(1.0/num);

    return mul_num_poly(a, e, &inv_num, &poly_value);
}


// ------------- Transfer Functions --------------------

bool tf_same_domain(Environment *e, const char *op, TransferFunction *a, TransferFunction *b){
	if(a->Ts == b->Ts) return true;
	runtime_error(e, "%s: can't combine systems with different sample times (%g and %g)",
			          op,  a->Ts, b->Ts);
	return false;
}

Value add_tf_tf(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;
		if(!tf_same_domain(e, "+", &tf1, &tf2)) return make_nan();

    // A     C
    // -  +  -
    // B     D

    Value A = make_poly(tf1.num);
    Value B = make_poly(tf1.den);
    Value C = make_poly(tf2.num);
    Value D = make_poly(tf2.den);

    Value AD = mul_poly_poly(a, e, &A, &D);
    Value BC = mul_poly_poly(a, e, &B, &C);
    Value BD = mul_poly_poly(a, e, &B, &D);

    Value AD_BC = add_poly_poly(a, e, &AD, &BC);

    TransferFunction tf = {
      .num = AD_BC.value.poly,
      .den = BD.value.poly,
			.Ts  = tf1.Ts	,
    };

    return make_transfer_function(tf_simplify(a, &tf));
}

Value sub_tf_tf(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;
		if(!tf_same_domain(e, "-", &tf1, &tf2)) return make_nan();

    // A     C
    // -  -  -
    // B     D

    Value A = make_poly(tf1.num);
    Value B = make_poly(tf1.den);
    Value C = make_poly(tf2.num);
    Value D = make_poly(tf2.den);

    Value AD = mul_poly_poly(a, e, &A, &D);
    Value BC = mul_poly_poly(a, e, &B, &C);
    Value BD = mul_poly_poly(a, e, &B, &D);

    Value AD_BC = sub_poly_poly(a, e, &AD, &BC);

    TransferFunction tf = {
      .num = AD_BC.value.poly,
      .den = BD.value.poly,
			.Ts  = tf1.Ts,
    };

    return make_transfer_function(tf_simplify(a, &tf));
}

Value mul_tf_tf(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;
		if(!tf_same_domain(e, "*", &tf1, &tf2)) return make_nan();

    // A     C
    // -  *  -
    // B     D

    Value A = make_poly(tf1.num);
    Value B = make_poly(tf1.den);
    Value C = make_poly(tf2.num);
    Value D = make_poly(tf2.den);

    Value AC = mul_poly_poly(a, e, &A, &C);
    Value BD = mul_poly_poly(a, e, &B, &D);

    TransferFunction tf = {
      .num = AC.value.poly,
      .den = BD.value.poly,
			.Ts  = tf1.Ts,
    };

    return make_transfer_function(tf_simplify(a, &tf));
}

Value mul_tf_poly(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    Polynomial poly     = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.poly : left->value.poly;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		poly_tf.Ts               = tf.Ts;
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return mul_tf_tf(a, e, &tf_val, &poly_tf_val);
}

Value add_tf_poly(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    Polynomial poly     = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.poly : left->value.poly;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		poly_tf.Ts               = tf.Ts;
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return add_tf_tf(a, e, &tf_val, &poly_tf_val);
}

Value sub_tf_poly(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->value.tf;
    Polynomial poly     = right->value.poly;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		poly_tf.Ts               = tf.Ts;
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return sub_tf_tf(a, e, &tf_val, &poly_tf_val);
}

Value sub_poly_tf(Arena *a, Environment *e, Value *left, Value *right){
    Polynomial poly     = left->value.poly;
    TransferFunction tf = right->value.tf;

		TransferFunction poly_tf = tf_from_poly(a, &poly);
		poly_tf.Ts               = tf.Ts;
		Value tf_val             = make_transfer_function(tf);
		Value poly_tf_val        = make_transfer_function(poly_tf);

    return sub_tf_tf(a, e, &poly_tf_val, &tf_val);
}

Value add_num_tf(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    double num          = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.number : left->value.number;

		TransferFunction num_tf = tf_from_num(a, num);
		num_tf.Ts               = tf.Ts;
		Value tf_val            = make_transfer_function(tf);
		Value num_tf_val        = make_transfer_function(num_tf);

    return add_tf_tf(a, e, &tf_val, &num_tf_val);
}


Value sub_tf_num(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->value.tf;
    double num          = right->value.number;

		TransferFunction num_tf = tf_from_num(a, num);
		num_tf.Ts               = tf.Ts;
		Value tf_val            = make_transfer_function(tf);
		Value num_tf_val        = make_transfer_function(num_tf);

    return sub_tf_tf(a, e, &tf_val, &num_tf_val);
}
Value sub_num_tf(Arena *a, Environment *e, Value *left, Value *right){
    double num          = left->value.number;
    TransferFunction tf = right->value.tf;

		TransferFunction num_tf = tf_from_num(a, num);
		num_tf.Ts               = tf.Ts;
		Value tf_val            = make_transfer_function(tf);
		Value num_tf_val        = make_transfer_function(num_tf);

    return sub_tf_tf(a, e, &num_tf_val, &tf_val);
}
Value mul_num_tf(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->valueType == VALUE_TRANSFER_FUNCTION ? left->value.tf : right->value.tf;
    double num          = left->valueType == VALUE_TRANSFER_FUNCTION ? right->value.number : left->value.number;

		TransferFunction new_tf = scale_tf(a, &tf, num);
    return make_transfer_function(new_tf);
}
Value div_num_tf(Arena *a, Environment *e, Value *left, Value *right){
    double num          = left->value.number;
    TransferFunction tf = right->value.tf;

		TransferFunction inv_tf = tf_invert(a, &tf);
		TransferFunction new_tf = scale_tf(a, &inv_tf, num);

    return make_transfer_function(new_tf);
}

Value div_tf_num(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf = left->value.tf;
    double num          = right->value.number;

		TransferFunction new_tf = scale_tf(a, &tf, 1.0/num);

    return make_transfer_function(new_tf);
}

Value div_tf_tf(Arena *a, Environment *e, Value *left, Value *right){
    TransferFunction tf1 = left->value.tf;
    TransferFunction tf2 = right->value.tf;
		if(!tf_same_domain(e, "/", &tf1, &tf2)) return make_nan();

		TransferFunction inv_tf2 = tf_invert(a, &tf2);

		Value tf1_val     = make_transfer_function(tf1);
		Value inv_tf2_val = make_transfer_function(inv_tf2);

    return mul_tf_tf(a, e, &tf1_val, &inv_tf2_val);
}

Value div_poly_tf(Arena *a, Environment *e, Value *left, Value *right){
		Polynomial p        = left->value.poly;
		TransferFunction tf = right->value.tf;

		TransferFunction tf_inv  = tf_invert(a, &tf);
		TransferFunction tf_poly = tf_from_poly(a, &p);
		tf_poly.Ts               = tf.Ts;
		
		Value tf_inv_val  = make_transfer_function(tf_inv);
		Value tf_poly_val = make_transfer_function(tf_poly);

		return mul_tf_tf(a, e, &tf_inv_val, &tf_poly_val);
}

Value div_tf_poly(Arena *a, Environment *e, Value *left, Value *right){
		TransferFunction tf = left->value.tf;
		Polynomial p        = right->value.poly;

		TransferFunction tf_poly     = tf_from_poly(a, &p);
		tf_poly.Ts                   = tf.Ts;
		TransferFunction tf_poly_inv = tf_invert(a, &tf_poly);

		Value tf_val                 = make_transfer_function(tf);
		Value tf_poly_inv_val        = make_transfer_function(tf_poly_inv);
		return mul_tf_tf(a, e, &tf_val, &tf_poly_inv_val);
		
}

// ----------- Strings ------------------

Value cat_num_str(Arena *a, Environment *e, Value *left, Value *right){
  double num    = left->value.number;
  char *str     = right->value.str;
  char *num_str = string_from_num(a, num);
  return make_string(cat_strings(a, num_str, str));
}

Value cat_str_num(Arena *a, Environment *e, Value *left, Value *right){
  char *str  = left->value.str;
  double num = right->value.number;
  char *num_str = string_from_num(a, num);
  return make_string(cat_strings(a, str, num_str));
}

Value cat_str_str(Arena *a, Environment *e, Value *left, Value *right){
  char *str1 = left->value.str;
  char *str2 = right->value.str;
  return make_string(cat_strings(a, str1, str2));
}
Value eq_str_str(Arena *a, Environment *e, Value *left, Value *right){
		return make_num(strcmp(left->value.str, right->value.str) == 0);
}

//------------ Tensors --------------

Value mul_tensor_tensor(Arena *a, Environment *e, Value *left, Value *right){
	Tensor x = left->value.tensor;
	Tensor y = right->value.tensor;

	if(x.sizes[x.ndim - 1] != y.sizes[0]){
		runtime_error(e, "@: inner dimensions don't match (%zu and %zu)",
				x.sizes[x.ndim - 1], y.sizes[0]);
		return make_nan();
	}

	// vector @ vector contracts to 0 dimensions: return a scalar
	if(x.ndim == 1 && y.ndim == 1){
		double complex dot;
		if(tensor_dot(a, &x, &y, &dot) != 0){
			runtime_error(e, "@: could not compute dot product");
			return make_nan();
		}
		if(x.dtype == DTYPE_REAL && y.dtype == DTYPE_REAL) return make_num(creal(dot));
		return make_complex(dot);
	}

	Tensor out;
	if(tensor_contract(a, &x, &y, &out) != 0){
		runtime_error(e, "@: result would have more than %d dimensions, or not enough memory", MAX_DIMS);
		return make_nan();
	}
	return make_tensor(out);
}

//------------ Broadcasting --------------

#define BROADCAST_OP(name, sym, re_expr, cx_expr) \
    static double         re_##name(double x, double y) { return (re_expr); } \
    static double complex cx_##name(double complex x, double complex y) { return (cx_expr); } \
  Value broadcast_##name(Arena *a, Environment *e, Value *l, Value *r) { \
    return broadcast(a, e, sym, l, r, re_##name, cx_##name); \
  }

typedef double         (*ScalarOpRe)(double, double);
typedef double complex (*ScalarOpCx)(double complex, double complex);

typedef Tensor Operand;

int as_operand(Value *v, Operand *out){
	*out = (Operand){0};
  switch (v->valueType) {
    case VALUE_NUMBER: 
      {
				out->dtype   = DTYPE_REAL;
				out->data.re = &v->value.number;
				out->ndim    = 0;
				out->count   = 1;
        return 1;
      }
    case VALUE_COMPLEX: 
      {
				out->dtype   = DTYPE_COMPLEX;
				out->data.cx = &v->value.complex_number;
				out->ndim    = 0;
				out->count   = 1;
        return 1;
      }
    case VALUE_TENSOR: 
      {
				*out = v->value.tensor;
        return 1;
      }
    default: return 0;
  }
  return 0;
}

// Broadcasting here follows the Numpy convention
// A:           (4,3,3)
// B:           (3,1)
// stride(A):   (9,3,1)
// stride(B):   (1,1)
// ------ Right-align and pad with ones ----
// A:           (4,3,3)
// B:           (1,3,1)
// ------ Right-align and pad with ones ----
// Out:         (4,3,3)
// ------ Pick strides based on Out ----
// stride(A):   (9,3,1)
// stride(B):   (0,1,0)

int broadcast_shape(Operand *x, Operand *y, size_t *nd_out, size_t *sizes_out){
	size_t nd = x->ndim > y->ndim ? x->ndim : y->ndim;
	for(size_t i = 0; i < nd; i++)	{
		size_t xs = i < x->ndim ? x->sizes[x->ndim - 1 - i] : 1;
		size_t ys = i < y->ndim ? y->sizes[y->ndim - 1 - i] : 1;
		if(xs != ys && xs != 1 && ys != 1) return 1; // Incompatible
		sizes_out[nd - 1 - i] = (xs == 1) ? ys : xs;
	}
	*nd_out = nd;
	return 0;
}

void broadcast_view(Operand *v, size_t nd, const size_t *sizes){
	size_t pad = nd - v->ndim;
	size_t strides[MAX_DIMS];
	for(size_t d = 0; d < nd; d++){
		if(d < pad){
			strides[d] = 0;
		}else{
			size_t src = d - pad;
			// If the original size was 1, we stretch it (0), else, we stride as usual
			strides[d] = (v->sizes[src] == 1) ? 0 : v->strides[src];
		}
	}
	v->ndim = nd;
	for(size_t d = 0; d < nd; d++){
		v->strides[d] = strides[d];
		v->sizes[d]   = sizes[d];
	}
}


Value broadcast(Arena *a, Environment *e, const char *sym, Value *l, Value *r, ScalarOpRe reop, ScalarOpCx cxop){
  Operand x, y;
  if(!as_operand(l, &x) || !as_operand(r, &y)){
    runtime_error(e, "%s: unsupported operand types", sym);
    return make_nan();
  }
	size_t nd_out;
	size_t sizes_out[MAX_DIMS];
	if(broadcast_shape(&x, &y, &nd_out, sizes_out) != 0){
    runtime_error(e, "%s: incompatible broadcast shapes", sym);
    return make_nan();
	}

	broadcast_view(&x, nd_out, sizes_out);
	broadcast_view(&y, nd_out, sizes_out);

	DType dtype = DTYPE_REAL;
	if(x.dtype == DTYPE_COMPLEX || y.dtype == DTYPE_COMPLEX) dtype = DTYPE_COMPLEX;

	Tensor out;
	if(tensor_init(a, &out, nd_out, sizes_out, dtype) != 0){
    runtime_error(e, "%s: could not allocate result", sym);
    return make_nan();
	}

	size_t indices[MAX_DIMS];
	for(size_t i = 0; i < out.count; i++){
		tensor_unravel_index(&out, indices, i);
		if(dtype == DTYPE_REAL){
			out.data.re[i] = reop(
				x.data.re[tensor_get_offset(&x, indices)],
				y.data.re[tensor_get_offset(&y, indices)]
			);
		}else{
			out.data.cx[i] = cxop(
				tensor_get_cx(&x, tensor_get_offset(&x, indices)),
				tensor_get_cx(&y, tensor_get_offset(&y, indices))
			);
		}
	}
	
	return make_tensor(out);
}

BROADCAST_OP(add, "+", x + y   ,  x + y)
BROADCAST_OP(sub, "-", x - y   ,  x - y)
BROADCAST_OP(mul, "*", x * y   ,  x * y)
BROADCAST_OP(div, "/", x / y   ,  x / y)
BROADCAST_OP(pow, "^", pow(x,y),  cpow(x, y))


static const BinaryFn binop_dispatch[OP_TYPE_LIST_COUNT][VALUE_TYPE_COUNT][VALUE_TYPE_COUNT] = {
	[OP_PLUS] = {
		[VALUE_NUMBER]              = {
			[VALUE_NUMBER]            = add_scalar_scalar,
			[VALUE_COMPLEX]           = add_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_add,
			[VALUE_POLYNOMIAL]        = add_num_poly,
			[VALUE_TRANSFER_FUNCTION] = add_num_tf,
			[VALUE_STRING]            = cat_num_str,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX]              = {
			[VALUE_NUMBER]            = add_scalar_scalar,
			[VALUE_COMPLEX]           = add_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_add,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_STRING]            = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TENSOR] = {
			[VALUE_NUMBER]            = broadcast_add,
			[VALUE_COMPLEX]           = broadcast_add,
			[VALUE_TENSOR]            = broadcast_add,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = add_num_poly,
			[VALUE_TENSOR]            = op_with_nan,
			[VALUE_POLYNOMIAL]        = add_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = add_tf_poly,
			[VALUE_NAN]               = op_with_nan,
		},
    [VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = add_num_tf,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = add_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = add_tf_tf,
			[VALUE_NAN]               = op_with_nan,
    },
		[VALUE_STRING] = {
			[VALUE_NUMBER]            = cat_str_num,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_STRING]            = cat_str_str,    
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_TENSOR]            = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_MINUS] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]            = sub_scalar_scalar,
			[VALUE_COMPLEX]           = sub_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_sub,
			[VALUE_POLYNOMIAL]        = sub_num_poly,
			[VALUE_TRANSFER_FUNCTION] = sub_num_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX]              = {
			[VALUE_NUMBER]            = sub_scalar_scalar,
			[VALUE_COMPLEX]           = sub_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_sub,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_STRING]            = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TENSOR] = {
			[VALUE_NUMBER]            = broadcast_sub,
			[VALUE_COMPLEX]           = broadcast_sub,
			[VALUE_TENSOR]             = broadcast_sub,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = sub_poly_num,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = sub_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = sub_poly_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = sub_tf_num,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = sub_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = sub_tf_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_MUL] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]            = mul_scalar_scalar,
			[VALUE_COMPLEX]           = mul_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_mul,
			[VALUE_POLYNOMIAL]        = mul_num_poly,
			[VALUE_TRANSFER_FUNCTION] = mul_num_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]            = mul_scalar_scalar,
			[VALUE_COMPLEX]           = mul_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_mul,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TENSOR] = {
			[VALUE_NUMBER]            = broadcast_mul,
			[VALUE_COMPLEX]           = broadcast_mul,
			[VALUE_TENSOR]            = broadcast_mul,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = mul_num_poly,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = mul_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = mul_tf_poly,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = mul_num_tf,
			[VALUE_TENSOR]             = op_with_nan,
			[VALUE_POLYNOMIAL]        = mul_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = mul_tf_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_TENSOR]            = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	[OP_DIV] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]            = div_scalar_scalar,
			[VALUE_COMPLEX]           = div_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_div,
      [VALUE_POLYNOMIAL]        = ordered_div_num_poly,
			[VALUE_TRANSFER_FUNCTION] = div_num_tf,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]            = div_scalar_scalar,
			[VALUE_COMPLEX]           = div_scalar_scalar,
			[VALUE_TENSOR]            = broadcast_div,
      [VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_TENSOR] = {
			[VALUE_NUMBER]            = broadcast_div,
			[VALUE_COMPLEX]           = broadcast_div,
			[VALUE_TENSOR]            = broadcast_div,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		},
		[VALUE_POLYNOMIAL] = {
			[VALUE_NUMBER]            = ordered_div_poly_num,
			[VALUE_TENSOR]            = op_with_nan,
      [VALUE_POLYNOMIAL]        = div_poly_poly,
			[VALUE_TRANSFER_FUNCTION] = div_poly_tf,
			[VALUE_NAN]               = op_with_nan,
    },
		[VALUE_TRANSFER_FUNCTION] = {
			[VALUE_NUMBER]            = div_tf_num,
			[VALUE_TENSOR]            = op_with_nan,
      [VALUE_POLYNOMIAL]        = div_tf_poly,
			[VALUE_TRANSFER_FUNCTION] = div_tf_tf,
			[VALUE_NAN]               = op_with_nan,
    },
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_COMPLEX]           = op_with_nan,
			[VALUE_TENSOR]            = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},
	[OP_TMUL] = {
		[VALUE_TENSOR] = {
			[VALUE_TENSOR] = mul_tensor_tensor,
		}
	},
	[OP_POW] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]  = pow_scalar_scalar,
			[VALUE_COMPLEX] = pow_scalar_scalar,
			[VALUE_TENSOR]  = broadcast_pow,
			[VALUE_NAN]     = op_with_nan,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]  = pow_scalar_scalar,
			[VALUE_COMPLEX] = pow_scalar_scalar,
			[VALUE_TENSOR]  = broadcast_pow,
			[VALUE_NAN]     = op_with_nan,
		},
		[VALUE_TENSOR] = {
			[VALUE_NUMBER]  = broadcast_pow,
			[VALUE_COMPLEX] = broadcast_pow,
			[VALUE_TENSOR]  = broadcast_pow,
			[VALUE_NAN]     = op_with_nan,
		},
		[VALUE_NAN] = {
			[VALUE_NUMBER]            = op_with_nan,
			[VALUE_TENSOR]            = op_with_nan,
			[VALUE_POLYNOMIAL]        = op_with_nan,
			[VALUE_TRANSFER_FUNCTION] = op_with_nan,
			[VALUE_NAN]               = op_with_nan,
		} 
	},

	// Comparisons

	[OP_DBL_EQ] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER]  = eq_scalar_scalar,
			[VALUE_COMPLEX] = eq_scalar_scalar,
		},
		[VALUE_COMPLEX] = {
			[VALUE_NUMBER]  = eq_scalar_scalar,
			[VALUE_COMPLEX] = eq_scalar_scalar,
		},
		[VALUE_STRING] = {
			[VALUE_STRING] = eq_str_str,
		}
	},

	[OP_LT] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = lt_num_num,
		}
	},

	[OP_GT] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = gt_num_num,
		}
	},

	[OP_GEQ] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = geq_num_num,
		}
	},

	[OP_LEQ] = {
		[VALUE_NUMBER] = {
			[VALUE_NUMBER] = leq_num_num,
		}
	},

};


//====================================
//
//   Native functions
//
//====================================

//------------ System ------------

Value native_assert(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  *silent    = 1;
	Value cond = argv[0];
	Value msg  = argv[1];
	if(msg.valueType != VALUE_STRING){
		runtime_error(e, "Second argument must be a string");
		(e->run_stats->failed_asserts)++;
		return make_nan();
	}
	if(!cond.value.number){
		fprintf(stderr, "FAIL: %s: %s\n", script, msg.value.str);
		(e->run_stats->failed_asserts)++;
	}
  return make_num(cond.value.number);
}

Value native_ls(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  env_list_variables(e);
  *silent = 1;
  return make_num(0);
}

void print_value(Arena *ta, Environment *e, Value *value);
static void print_value_bare(Arena *ta, Environment *e, Value *value);
Value native_print(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
    print_value_bare(a, e, &val);
    *silent = 1;
    return make_num(0);
}

int import_script(Arena *ta, Environment *e, char* importer, char *path);
Value native_import(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
  if(val.valueType != VALUE_STRING){
    char buf[1024];
		runtime_error(e, "Importing a module expects a string.");
    *silent = 1;
    return make_num(0);
  }
  if(import_script(a, e, script, val.value.str) != 0){
		runtime_error(e, "Error importing script %s", val.value.str);
	}
  *silent = 1;
  return make_num(0);
}



// Views a Value as a 1-D-like real tensor. Returns 0 if it isn't one.
// This includes shapes that are 1-D-like, such as:
// (1,n), (n,1), (1,1,n), etc.
int as_real_vector(Arena *a, Value *v, double **data, size_t *n){
	if(v->valueType != VALUE_TENSOR)          return 0;
	Tensor *t = &v->value.tensor;
	if(t->dtype != DTYPE_REAL) return 0;

	size_t dim           = 0;
	size_t non_singleton = 0;
	for(size_t i = 0; i < t->ndim; i++){
		if(t->sizes[i] != 1) { dim = i; non_singleton++; }
	}
	if(non_singleton > 1) return 0; // This is a matrix

	*n            = t->count;
	size_t stride = t->strides[dim];
	// If the elements are adjacent or there aren't more than one,
	// no need to copy them!
	if(stride == 1 || t->count <= 1) { *data = t->data.re; return 1; }

	double *buf = arena_alloc(a, t->count * sizeof(double));
	if(!buf) return 0;
	// This is because if the tensor has a unique singleton dimension, say:
	// a: (1,1,n,1,1)
	// Then we need to map each index k to (0,0,k,0,0)
	// In other words, the offset will be 0 + 0 + k * stride + 0 + 0
	for(size_t k = 0; k < t->count; k++) buf[k] = t->data.re[k * stride];
	*data = buf;
	return 1;
}

// Wraps an existing buffer of n doubles as a 1-D real tensor (no copy).
Value make_real_vector(double *data, size_t n){
	Tensor t = {0};
	t.dtype      = DTYPE_REAL;
	t.data.re    = data;
	t.ndim       = 1;
	t.count      = n;
	t.sizes[0]   = n;
	t.strides[0] = 1;
	return make_tensor(t);
}

typedef double         (*RealFn)       (double);
typedef double complex (*ComplexFn)    (double complex);
typedef double         (*RealComplexFn)(double complex);


Value lift_unary(Arena *a, Environment *e, const char *name, Value v, RealFn rf, ComplexFn cf, RealComplexFn rcf){
  switch(v.valueType){
    case VALUE_NUMBER: return make_num(rf(v.value.number));
    case VALUE_COMPLEX:
       {
         if(cf)  return make_complex(cf(v.value.complex_number));
         if(rcf) return make_num(rcf(v.value.complex_number));
         break;
       }
    case VALUE_TENSOR:
       {
          // Elementwise: the result keeps the input's sizes and strides, so
          // looping over the flat buffer is fine even for transposed views.
          Tensor *src = &v.value.tensor;
          DType out_dtype;
          if(src->dtype == DTYPE_REAL) out_dtype = DTYPE_REAL;
          else if(cf)                  out_dtype = DTYPE_COMPLEX;
          else if(rcf)                 out_dtype = DTYPE_REAL;
          else break;

          Tensor out = *src;
          out.dtype  = out_dtype;
          size_t elsize = out_dtype == DTYPE_REAL ? sizeof(double) : sizeof(double complex);
          void *buf = arena_alloc(a, src->count * elsize);
          if(buf == NULL){
            runtime_error(e, "%s: out of memory for %zu elements", name, src->count);
            return make_nan();
          }
          if(out_dtype == DTYPE_REAL) out.data.re = buf;
          else                        out.data.cx = buf;

          for(size_t i = 0; i < src->count; i++){
            if(src->dtype == DTYPE_REAL) out.data.re[i] = rf(src->data.re[i]);
            else if(cf)                  out.data.cx[i] = cf(src->data.cx[i]);
            else                         out.data.re[i] = rcf(src->data.cx[i]);
          }
          return make_tensor(out);
       }
    default: break;
  }
  runtime_error(e, "%s: unsupported argument type", name);
  return make_nan();
}


//------------ Math --------------

// These functions are here simply for compatibility purposes
double real_identity(double x)                 { return x; }
double real_zero(double x)                     { return 0.0; }
double real_arg(double x)                      { return atan2(0.0, x); }
double real_sqr(double x)                      { return x * x; }
double complex complex_sqr(double complex x)   { return x * x; }
double complex complex_log10(double complex x) { return clog(x) / clog(10); }

#define UNARY_NATIVE_MATHS_LIST                     \
      M(abs,   fabs,          NULL,          cabs)  \
      M(real,  real_identity, NULL,          creal) \
      M(imag,  real_zero,     NULL,          cimag) \
      M(arg,   real_arg,      NULL,          carg)  \
      M(log,   log,           clog,          NULL)  \
      M(log10, log10,         complex_log10, NULL)  \
      M(exp,   exp,           cexp,          NULL)  \
      M(sin,   sin,           csin,          NULL)  \
      M(cos,   cos,           ccos,          NULL)  \
      M(tan,   tan,           ctan,          NULL)  \
      M(asin,  asin,          casin,         NULL)  \
      M(acos,  acos,          cacos,         NULL)  \
      M(atan,  atan,          catan,         NULL)  \
      M(sinh,  sinh,          csinh,         NULL)  \
      M(cosh,  cosh,          ccosh,         NULL)  \
      M(tanh,  tanh,          ctanh,         NULL)  \
      M(asinh, asinh,         casinh,        NULL)  \
      M(acosh, acosh,         cacosh,        NULL)  \
      M(atanh, atanh,         catanh,        NULL)  \
      M(floor, floor,         NULL,          NULL)  \
      M(ceil,  ceil,          NULL,          NULL)  \
      M(round, round,         NULL,          NULL)  \
      M(sqr,   real_sqr,      complex_sqr,   NULL)  \
      M(sqrt,  sqrt,          csqrt,         NULL)

#define M(name, rf, cf, rcf) \
  Value native_##name(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){  \
   return lift_unary(a, e, #name, argv[0], rf, cf, rcf); \
  } 
  UNARY_NATIVE_MATHS_LIST
#undef M

Value native_range(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){    
	Value from_val = argv[0];
	Value to_val   = argv[1];
	if(from_val.valueType != VALUE_NUMBER || to_val.valueType != VALUE_NUMBER){
		runtime_error(e, "Expected 2 real numbers. Example: range(1,5) = [1,2,3,4]");
		return make_nan();
	}
	int from = floor(from_val.value.number);
	int to   = ceil(to_val.value.number);
	int n    = to - from;
	int nele = abs(n);

	Tensor t;
	size_t sizes[MAX_DIMS] = {0};
	sizes[0] = nele;
	if(tensor_init(a, &t, 1, sizes, DTYPE_REAL) != 0){
		runtime_error(e, "range: not enough memory for %d elements", nele);
		return make_nan();
	}

	int step = (from < to) - (from > to);

	for(int i = 0; i < nele; i++) {
		t.data.re[i] = from + (i * step);
	}

	return make_tensor(t);
}

Value native_linspace(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){    
	Value from_val  = argv[0];
	Value to_val    = argv[1];
	if(
      from_val.valueType  != VALUE_NUMBER   ||
      to_val.valueType    != VALUE_NUMBER
      ){
		runtime_error(e, "linspace: from and to must be numbers");
		return make_nan();
	}

	double steps_val;
	if(!get_opt_number(e, "linspace", argc, argv, 2, "num_points", 50, &steps_val)) return make_nan();

  if(!isfinite(steps_val) || steps_val != floor(steps_val)) {
		runtime_error(e, "linspace: number of points must be a whole number (got %g)", steps_val);
		return make_nan();
  }
  if(!(steps_val <= 1e7)){
		runtime_error(e, "linspace: number of points is too high");
		return make_nan();
  }
  if(steps_val < 0) {
		runtime_error(e, "linspace: number of points can't be negative");
		return make_nan();
  }

	double from  = from_val.value.number;
	double to    = to_val.value.number;
	int n_points = ceil(steps_val);
	double dx    = (to - from) / (n_points - 1.0);

	Tensor t;
	size_t sizes[MAX_DIMS];
	sizes[0] = n_points;
	if(tensor_init(a, &t, 1, sizes, DTYPE_REAL) != 0){
    runtime_error(e, "linspace: not enough memory for %d points", n_points); 
    return make_nan(); 
	}

	if(n_points == 0) return make_tensor(t);
	if(n_points == 1) {
		t.data.re[0] = from;
		return make_tensor(t);
	}

	for(int i = 0; i < n_points; i++) {
		t.data.re[i] = from + (i * dx);
	}
  t.data.re[n_points - 1] = to;

	return make_tensor(t);
}

Value native_len(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
  if(
      val.valueType != VALUE_TENSOR &&
      val.valueType != VALUE_TUPLE
    )
  {
    runtime_error(e, "len: expected an array or a tuple");
    return make_nan();
  }

  if(val.valueType == VALUE_TENSOR) return make_num(val.value.tensor.count);
  if(val.valueType == VALUE_TUPLE) return make_num(val.value.tuple.size);
  return make_num(0);
}

Value native_push(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value tens = argv[0];
  Value val  = argv[1];
  if(
      tens.valueType != VALUE_TENSOR ||
      (val.valueType != VALUE_NUMBER && val.valueType != VALUE_COMPLEX)
    )
  {
    runtime_error(e, "push: expected an array and a number");
    return make_nan();
  }
	if(tens.value.tensor.ndim != 1){
    runtime_error(e, "push: expected a 1D array");
    return make_nan();
	}

	Tensor new_t;
	Tensor old_t           = tens.value.tensor;
	size_t sizes[MAX_DIMS] = {0};
	size_t len             = old_t.count + 1;
	sizes[0]               = len;

	DType dtype = (val.valueType == VALUE_COMPLEX || old_t.dtype == DTYPE_COMPLEX)
									? DTYPE_COMPLEX 
									: DTYPE_REAL;

	if(tensor_init(a, &new_t, old_t.ndim, sizes, dtype) != 0){
    runtime_error(e, "push: not enough memory");
    return make_nan();
	}

	for(size_t i = 0; i < old_t.count; i++){
		if(dtype == DTYPE_REAL){
			new_t.data.re[i] = old_t.data.re[i];
		}else{
			new_t.data.cx[i] = tensor_get_cx(&old_t, i);
		}
	}
	if(dtype == DTYPE_REAL){
		new_t.data.re[old_t.count] = val.value.number;
	}else{
		new_t.data.cx[old_t.count] = val.valueType == VALUE_COMPLEX
		                             ? val.value.complex_number
		                             : val.value.number;
	}

  return make_tensor(new_t);
}

// ------------- Plotting ------------

Value native_plot(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
	*silent = 1;
	Value x_val = argv[0];
	Value y_val = argv[1];
	if(x_val.valueType != VALUE_TENSOR || y_val.valueType != VALUE_TENSOR){
		runtime_error(e, "plot requires two arrays");
		return make_nan();
	}
	if(x_val.value.tensor.dtype != DTYPE_REAL || y_val.value.tensor.dtype != DTYPE_REAL){
		runtime_error(e, "plot: complex values are not supported");
		return make_nan();
	}

	size_t style_idx;
	if(!get_opt_choice(e, "plot", argc, argv, 2, "style", PLOT_STYLES, PLOT_LINES, &style_idx) ) return make_nan();

	double *xs, *ys;
	size_t  nx,  ny;
	if(!as_real_vector(a, &x_val, &xs, &nx)){
		runtime_error(e, "plot: expected both arguments to be 1D arrays");
		return make_nan();
	}
	if(!as_real_vector(a, &y_val, &ys, &ny)){
		runtime_error(e, "plot: expected both arguments to be 1D arrays");
		return make_nan();
	}

	if(nx != ny){
		runtime_error(e, "plot: arrays have different lengths (%zu and %zu)",
				x_val.value.tensor.count, y_val.value.tensor.count);
		return make_nan();
	}

	PlotSeries series = {
		.x_data     = xs,
		.y_data     = ys,
		.num_points = nx,
		.style      = (PlotStyle)style_idx,
	};
	PlotData data = { .series = &series, .num_series = 1 };

	PlotLayout layout;
	if(!plot_layout(&data, &layout)){
		runtime_error(e, "plot: no finite points to plot");
		return make_nan();
	}

	plot_braille(a, stdout, &data, &layout, 60, 15);
	return make_num(0);
}

// ------------- Arrays and Tensors ------------


typedef double (*ReducerFn)(double acc, double x);
double reducer_max(double acc, double x) { return x > acc ? x : acc; }
double reducer_min(double acc, double x) { return x < acc ? x : acc; }
double reducer_sum(double acc, double x) { return x + acc; }
double reducer_prod(double acc, double x) { return x * acc; }

Value reduce(Environment *e, const char* name, ReducerFn reducer, Value val, double init, bool emtpy_ok){
  if(val.valueType == VALUE_NUMBER) return make_num(val.value.number);
  if(val.valueType != VALUE_TENSOR || val.value.tensor.dtype != DTYPE_REAL){
    runtime_error(e, "%s expects a real array", name);
    return make_nan();
  }
  // Order doesn't matter for these reducers, so the flat buffer works for any shape
  size_t size = val.value.tensor.count;
  if(size == 0 && !emtpy_ok){
    runtime_error(e, "%s: array can't be empty.", name);
    return make_nan();
  }
  double acc = init;
  for(size_t i = 0; i < size; i++){
    double v = val.value.tensor.data.re[i];
    if(isnan(v)) return make_num(NAN);
    acc = reducer(acc, v);
  }
  return make_num(acc);
}

#define NATIVE_REDUCER_LIST                  \
  R(max,  reducer_max,  -INFINITY, false)    \
  R(min,  reducer_min,  INFINITY,  false)    \
  R(sum,  reducer_sum,  0.0,       true)     \
  R(prod, reducer_prod, 1.0,       false)

#define R(name, reducer_fn, init, empty_ok) \
Value native_##name(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent) \
{  \
    return reduce(e, #name, reducer_fn, argv[0], init, empty_ok); \
}
  NATIVE_REDUCER_LIST
#undef R

Value native_mean(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
  Value total = reduce(e, "mean", reducer_sum, argv[0], 0.0, false);
  if(total.valueType != VALUE_NUMBER || argv[0].valueType != VALUE_TENSOR) return total;
  return make_num(total.value.number / argv[0].value.tensor.count);
}       

Value native_eye(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
  Value n_val = argv[0];
	if(n_val.valueType != VALUE_NUMBER){
		runtime_error(e, "eye: argument must be a number");
		return make_nan();
	}
	size_t n = floor(n_val.value.number);
	size_t sizes[MAX_DIMS] = {0};
	sizes[0] = n;
	sizes[1] = n;

	Tensor t;
	if(tensor_init(a, &t, 2, sizes, DTYPE_REAL) != 0) {
		runtime_error(e, "eye: not enough memory to allocate matrix");
		return make_nan();
	}

	size_t indices[MAX_DIMS] = {0};
	for(size_t i = 0; i < n; i++){
		indices[0] = i;
		indices[1] = i;
		t.data.re[tensor_get_offset(&t, indices)] = 1.0;
	}

  return make_tensor(t);
}

Value native_zeros(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
  Value sizes_val = argv[0];
	if(sizes_val.valueType != VALUE_TENSOR){
		runtime_error(e, "zeros: argument must be an array");
		return make_nan();
	}
	Tensor sizes_tensor = sizes_val.value.tensor;
	if(sizes_tensor.ndim != 1){
		runtime_error(e, "zeros: expected a 1-dimensional array");
		return make_nan();
	}
	if(sizes_tensor.count == 0 || sizes_tensor.count > MAX_DIMS || sizes_tensor.dtype != DTYPE_REAL){
		runtime_error(e, "zeros: expected between 1 and %d real sizes", MAX_DIMS);
		return make_nan();
	}
	size_t sizes[MAX_DIMS] = {0};
	for(size_t i = 0; i < sizes_tensor.count; i++) sizes[i] = floor(sizes_tensor.data.re[i]);

	Tensor t;
	if(tensor_init(a, &t, sizes_tensor.count, sizes, DTYPE_REAL) != 0){
		runtime_error(e, "zeros: not enough memory to allocate matrix");
		return make_nan();
	}
	for(size_t i = 0; i < t.count; i++) t.data.re[i] = 0.0;
  return make_tensor(t);
}

Value native_ones(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
  Value sizes_val = argv[0];
	if(sizes_val.valueType != VALUE_TENSOR){
		runtime_error(e, "ones: argument must be an array");
		return make_nan();
	}
	Tensor sizes_tensor = sizes_val.value.tensor;
	if(sizes_tensor.ndim != 1){
		runtime_error(e, "ones: expected a 1-dimensional array");
		return make_nan();
	}
	if(sizes_tensor.count == 0 || sizes_tensor.count > MAX_DIMS || sizes_tensor.dtype != DTYPE_REAL){
		runtime_error(e, "ones: expected between 1 and %d real sizes", MAX_DIMS);
		return make_nan();
	}
	size_t sizes[MAX_DIMS] = {0};
	for(size_t i = 0; i < sizes_tensor.count; i++) sizes[i] = floor(sizes_tensor.data.re[i]);

	Tensor t;
	if(tensor_init(a, &t, sizes_tensor.count, sizes, DTYPE_REAL) != 0){
		runtime_error(e, "ones: not enough memory to allocate matrix");
		return make_nan();
	}
	for(size_t i = 0; i < t.count; i++) t.data.re[i] = 1.0;
  return make_tensor(t);
}

// ------------- Polynomials ------------

Value native_poly(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value val = argv[0];
	double *coeffs;
	size_t num_coeffs;
	if(!as_real_vector(a, &val, &coeffs, &num_coeffs)){
		runtime_error(e, "poly: Expected a 1-D-like array of coefficients");
		return make_nan();
	}
	int err;
	Polynomial p = poly_new(a, coeffs, num_coeffs);
	poly_update_degree(&p);
	return make_poly(p);
}
Value native_polymod(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  Value second = argv[1];
  if(first.valueType == VALUE_POLYNOMIAL && second.valueType == VALUE_POLYNOMIAL){
    Polynomial rem;
    if(poly_divmod(a, first.value.poly, second.value.poly, NULL, &rem) != 0) {
			runtime_error(e, "polymod: division by the zero polynomial");
			return make_nan();
		}
    return make_poly(rem);
  }
	runtime_error(e, "Unsupported operation, expected a polynomial!");
  return make_nan();
}
Value native_polydiv(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  Value second = argv[1];
	if(first.valueType == VALUE_POLYNOMIAL && second.valueType == VALUE_POLYNOMIAL){
		Polynomial div;
		if(poly_divmod(a, first.value.poly, second.value.poly, &div, NULL) != 0){ 
			runtime_error(e, "polydiv: division by the zero polynomial");
			return make_nan();
		}
		return make_poly(div);
	}
  runtime_error(e, "Unsupported operation, expected a polynomial!");
  return make_nan();
}
Value native_polygcd(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  Value second = argv[1];
  if(first.valueType == VALUE_POLYNOMIAL && second.valueType == VALUE_POLYNOMIAL){
    Polynomial gcd = poly_gcd(a, first.value.poly, second.value.poly);
    return make_poly(gcd);
  }
  runtime_error(e, "Unsupported operation, expected a polynomial!");
  return make_nan();
}

Value native_poles(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value first = argv[0];
  if(first.valueType == VALUE_TRANSFER_FUNCTION){
		TransferFunction tf = first.value.tf;
		double complex *roots = tf_poles(a, tf);

		Value *values = arena_alloc(a, tf.den.deg * sizeof(Value));
		for(size_t i = 0; i < tf.den.deg; i++){ values[i] = make_complex(roots[i]); }

		Tuple t = {
			.size = tf.den.deg,
			.items = values,
		};
		return make_tuple(t);
  }
	runtime_error(e, "Unsupported operation, expected a transfer function.");
  return make_nan();
}

// Checks the (G, t) arguments shared by impulse() and step().
// t must be an array of non-decreasing times.
static int check_response_args(Arena *a, Environment *e, const char *name, Value *argv){
	double *ts;
	size_t  n;
	if(argv[0].valueType != VALUE_TRANSFER_FUNCTION || !as_real_vector(a, &argv[1], &ts, &n)){
		runtime_error(e, "%s expects a transfer function and an array of times. Example: %s(1/(s+1), linspace(0, 10, 200))", name, name);
		return 0;
	}
	for(size_t i = 1; i < n; i++){
		if(!(ts[i] >= ts[i - 1])){
			runtime_error(e, "%s: times must be non-decreasing (t[%zu] = %g comes after %g)", name, i, ts[i], ts[i - 1]);
			return 0;
		}
	}
	return 1;
}

Value native_impulse(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
	if(!check_response_args(a, e, "impulse", argv)) return make_nan();

	double *ts;
	size_t n;

	if(!as_real_vector(a, &argv[1], &ts, &n)){
		runtime_error(e, "impulse: expected a real vector");
		return make_nan();
	}

	TransferFunction tf = argv[0].value.tf;

	// DT response
	if(tf.Ts > 0){
		double *y = dt_response_at(a, e, tf, ts, n, false);
		if(!y) return make_nan();
		return make_real_vector(y, n);
	}

	double *y = impulse_response(a, e, tf, ts, n);
	if(!y) return make_nan();
	return make_real_vector(y, n);
}

Value native_step(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){
	if(!check_response_args(a, e, "step", argv)) return make_nan();
	double *ts;
	size_t n;

	if(!as_real_vector(a, &argv[1], &ts, &n)){
		runtime_error(e, "step: expected a real vector");
		return make_nan();
	}

	TransferFunction tf = argv[0].value.tf;

	// DT response
	if(tf.Ts > 0){
		double *y = dt_response_at(a, e, tf, ts, n, true);
		if(!y) return make_nan();
		return make_real_vector(y, n);
	}


	double *y = step_response(a, e, tf, ts, n);
	if(!y) return make_nan();
	return make_real_vector(y, n);
}

Value native_zvar(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
  Value ts_val = argv[0];
	if(ts_val.valueType != VALUE_NUMBER){
		runtime_error(e, "zvar: expected a sampling time");
		return make_nan();
	}

	double Ts = ts_val.value.number;

	if(Ts <= 0){
		runtime_error(e, "zvar: sampling time must be > 0");
		return make_nan();
	}

	double num_coeffs[]     = {0.0, 1.0};
	size_t num_coeffs_count = 2;
	double den_coeffs[]     = {1.0};
	size_t den_coeffs_count = 1;
	Polynomial pnum         = poly_new(a, num_coeffs, num_coeffs_count);
	Polynomial pden         = poly_new(a, den_coeffs, den_coeffs_count);
	TransferFunction ztf = {
		.num = pnum,
		.den = pden,
		.Ts =  ts_val.value.number
	};
	return make_transfer_function(ztf);
}

Value native_complex(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent){ 
	Value re = argv[0];
	Value im = argv[1];
	if(re.valueType != VALUE_NUMBER || im.valueType != VALUE_NUMBER){
		runtime_error(e, "Expected 2 real numbers. Example: complex(1,2) = 1 + 2*j");
		return make_nan();
	}
	return make_complex(re.value.number + I * im.value.number);
}


typedef struct {
  const char          *name;
  NativeValueDispatch fn;
  size_t              argc_min;
  size_t              argc_max;
} NativeFn;


#define NATIVE_LIST             \
        N(assert,   2)          \
        N(ls,       0)          \
        N(print,    1)          \
        N(import,   1)          \
        N(complex,  2)          \
        UNARY_NATIVE_MATHS_LIST \
        NATIVE_REDUCER_LIST     \
        N(range,    2)          \
        NV(linspace, 2, 3)      \
        N(len,      1)          \
        N(push,     2)          \
        N(mean,     1)          \
        N(eye,      1)          \
        N(zeros,    1)          \
        N(ones,     1)          \
        NV(plot,     2, 3)      \
        N(poly,     1)          \
        N(polymod,  2)          \
        N(polydiv,  2)          \
        N(polygcd,  2)          \
        N(poles,    1)          \
        N(impulse,  2)          \
        N(step,     2)          \
        N(zvar,     1)
#define N(name, argc) { #name, native_##name, argc, argc },
#define NV(name, argc_min, argc_max) { #name, native_##name, argc_min, argc_max },
#define M(name, rf, cf, rcf) N(name, 1)
#define R(name, reducer_fn, init, empty_ok) N(name, 1)
static const NativeFn natives[] = { NATIVE_LIST };
#undef M
#undef N
#undef R


//==============================
//
//   Postfix operations
//
//==============================

typedef Value (*PostfixFn)(Arena *a, Environment *e, Value *target);
typedef struct {
	PostfixFn  fn;
	const char *name;
} PostfixFnInfo;

Value postfix_shape(Arena *a, Environment *e, Value *target){
	if(target->valueType != VALUE_TENSOR){
		runtime_error(e, "shape: expected a tensor");
		return make_nan();
	}
	size_t ndim   = target->value.tensor.ndim;
	double *sizes = arena_alloc(a, ndim * sizeof(double));
	for(size_t i  = 0; i < ndim; i++) sizes[i] = target->value.tensor.sizes[i];
	Value shape   = make_real_vector(sizes, ndim);
	return shape;
}

Value postfix_T(Arena *a, Environment *e, Value *target){
	if(target->valueType != VALUE_TENSOR){
		runtime_error(e, "T: expected a tensor");
		return make_nan();
	}
	size_t ndim   = target->value.tensor.ndim;
	if(ndim == 1){
		Tensor col     = target->value.tensor;
		col.ndim       = 2;
		col.sizes[1]   = 1;
		col.strides[1] = 0;
		return make_tensor(col);
	}
	if(ndim != 2){
		runtime_error(e, "T: expected 2 dimensions");
		return make_nan();
	}
	Tensor new_t = clone_tensor(a, &target->value.tensor);
	tensor_swap_axes(&new_t, 0, 1);
	return make_tensor(new_t);
}



#define POSTFIX_FN_LIST \
	N(shape)              \
	N(T)

#define N(x) { postfix_##x, #x },
const PostfixFnInfo postfix_fn_list[] = {
		POSTFIX_FN_LIST
};
#undef N


//==============================
//
//   Expression  evaluation
//
//==============================


Value resolve_variable(Arena *a, Environment *e, Value *literal){
	if(literal->valueType == VALUE_STRING){
		return *literal;
	}
	Variable *var = env_find_variable(e, literal->value.str);
	if(!var) {
		runtime_error(e, "Undefined variable %s", literal->value.str);
		return make_nan();
	}
	return *var->value;
}

// Evaluation

typedef enum {
	EXEC_OK,
	EXEC_ERROR,
	EXEC_RETURN,
} ExecStatus;

ExecStatus eval_stmt(Arena* ta, AST *ast, Environment *e, char *script_path, AST *stmt, Value *result_out);
Value eval_expr(Arena *a, Environment *e, char *script_path, AST *expr, int *silent);
ExecStatus eval_block( Arena *a, AST *ast, Environment *e, char *script_path, AST *first, Value *result_out);

Value eval_unop(Arena *a, Environment *e, AST *op, Value *arg_given){
	Value *arg  = arg_given;

	if(arg->valueType == VALUE_ID){
		*arg = resolve_variable(a, e, arg);
	}

	if( op->as.binop.op >= OP_TYPE_LIST_COUNT ||
			arg->valueType >= VALUE_TYPE_COUNT
			)
	{
		return make_nan();
	}

	UnaryFn fn = unop_dispatch[op->as.binop.op][arg->valueType];

	if(!fn){
		runtime_error(e, "Unsupported operation!");
		return make_nan();
	}

	return fn(a, arg);


	runtime_error(e, "Unrecognized unary operator");
	exit(1);

}

Value eval_binop(Arena *a, Environment *e, AST *op, Value *raw_left, Value *raw_right){

	Value *left  = raw_left;
	Value *right = raw_right;

	// Resolve variables
	// TODO: Check if it begins  in quotation  marks?  For  now, assume
	//       all strings  are  variable  names
	if(left->valueType == VALUE_ID){
		*left = resolve_variable(a, e, left);
	}
	if(right->valueType == VALUE_ID){
		*right = resolve_variable(a, e, right);
	}

	if( op->as.binop.op >= OP_TYPE_LIST_COUNT ||
			left->valueType >= VALUE_TYPE_COUNT  ||
			right->valueType >= VALUE_TYPE_COUNT
			)
	{
		return make_nan();
	}

	BinaryFn fn = binop_dispatch[op->as.binop.op][left->valueType][right->valueType];

	if(!fn){
		runtime_error(e, "Unsupported binary operation!");
		return make_nan();
	}

	return fn(a, e, left, right);

	runtime_error(e, "Expected 2 numbers, got something else!");
	exit(1);

}


Environment *push_env(Environment *e){
  Arena *fa = e->frame;

	Environment *new_env = arena_alloc(fa, sizeof(Environment));

  new_env->persistent    = e->persistent;
	new_env->parent        = env_root(e);
  new_env->frame         = e->frame;
  new_env->num_vars      = 0;
  new_env->vars          = arena_alloc(fa, ENV_INITIAL_VARS * sizeof(Variable));
  new_env->vars_capacity = ENV_INITIAL_VARS;
	new_env->run_stats     = e->run_stats;
	new_env->exe_path      = e->exe_path;

	return new_env;
}

Value eval_function(Arena *ta, Environment *e, AST *fn_call, char *script_path, int *silent){
	int argc      = fn_call->as.fn_call.num_arguments;
	char *fn_name = fn_call->as.fn_call.fn_name;

	// Evaluating arguments in the base environment

	Value *evaluated = arena_alloc(ta, argc * sizeof(Value));
	if(argc > 0){
		size_t i = 0;
		for(
			AST *arg  = fn_call->as.fn_call.first_arg;
			arg      != NULL;
			arg       = arg->next
		){
			Value val    = eval_expr(ta, e, script_path, arg, silent);
			evaluated[i] = val;
			i++;
		}
	}

	// Get the function variable from the base environment

	Variable *fun_var = env_find_variable(e, fn_name);
	if(!fun_var) {
		runtime_error(e, "Undefined function %s", fn_call->as.fn_call.fn_name);
		return make_nan();
	}
	Value *fun = fun_var->value;

	// Sanity checks

	if (fun->valueType != VALUE_FUNCTION) {
		runtime_error(e, "%s is not a function", fn_name);
		return make_nan();
	}

  if(fun->value.fun.kind == FN_NATIVE){
    size_t argc_min = fun->value.fun.native_data.argc_min;
    size_t argc_max = fun->value.fun.native_data.argc_max;
		if((size_t)argc < argc_min || (size_t)argc > argc_max){
			if(argc_max == ARGS_VARIADIC){
				runtime_error(e, "%s expects at least %zu arguments, got %d", fn_name, argc_min, argc);
			}else if(argc_min == argc_max){
				runtime_error(e, "%s expects %zu arguments, got %d", fn_name, argc_min, argc);
			}else{
				runtime_error(e, "%s expects between %zu and %zu arguments, got %d", fn_name, argc_min, argc_max, argc);
			}
			return make_nan();
		}
  }else{
    size_t num_args = fun->value.fun.definition->as.fn_def.num_args;
		if((size_t)argc != num_args){
			runtime_error(e, "%s expects %zu arguments, got %d", fn_name, num_args, argc);
			return make_nan();
		}
  }

  if(fun->value.fun.kind == FN_NATIVE){
    int idx = fun->value.fun.native_data.native_index;
    if(idx < 0 || idx >= LEN(natives)){
			runtime_error(e, "%s not registered", fn_name);
      return make_nan();
    }
    NativeValueDispatch dispatch = natives[idx].fn;
    Value result = dispatch(ta, e, script_path, argc, evaluated, silent);
    return result;
  }


	// Push a new environment for the function scope

  size_t scratch = arena_save(e->frame);
	Environment *new_env = push_env(e);

	// Set its argument values to those evaluated before

	size_t i = 0;
	for(
			AST *arg      = fn_call->as.fn_call.first_arg,
			    *arg_def  = fun->value.fun.definition->as.fn_def.params;
			arg          != NULL;
			arg           = arg->next,
			arg_def       = arg_def->next
		 ){
		env_set_variable(new_env, arg_def->as.literal.value.id, &evaluated[i]);
		i++;
	}

	Value result;
  AST *block     = fun->value.fun.definition->as.fn_def.block;
  ExecStatus out = eval_block(ta, fn_call, new_env, script_path, block, &result);

  if(out == EXEC_ERROR){
    arena_restore(e->frame, scratch);
    return make_nan();
  }

  Value escaped = value_clone(ta, &result);
  arena_restore(e->frame, scratch);
	return escaped;
}

//============================== 
//
//    Printing
//
//============================== 

int format_complex(char *buf, size_t n, double complex z, int sig, bool compact){
  char re_buf[32], im_buf[32];
  double im = cimag(z);
  format_number(re_buf, sizeof(re_buf), creal(z), sig);
  format_number(im_buf, sizeof(im_buf), fabs(im), sig);
  const char *sep = compact ? (im < 0 ? "-" : "+") : (im < 0 ? " - " : " + ");
  return snprintf(buf, n, "%s%s%si", re_buf, sep, im_buf);
}       

int format_tensor_element(char *buf, size_t n, const Tensor *t, size_t off){
  if(t->dtype == DTYPE_REAL) return format_number(buf, n, t->data.re[off], DISPLAY_SIG);
  return format_complex(buf, n, t->data.cx[off], DISPLAY_SIG, true);
}

int tensor_max_width(Tensor *t){
  size_t idx[MAX_DIMS];
  char   buf[80];
  int    width = 0;
  for(size_t p = 0; p < t->count; p++){ 
    tensor_unravel_index(t, idx, p);
    int len = format_tensor_element(buf, sizeof(buf), t, tensor_get_offset(t, idx));
    if(len > width) width = len;
  }
  return width;
}

void write_tensor_element(FILE *out, const Tensor *t, size_t off, int width){
  char buf[80];
  format_tensor_element(buf, sizeof(buf), t, off);
  fprintf(out, "%*s", width, buf);
}

void write_tensor_rec(FILE *out, Tensor *t, size_t dim, size_t offset, int width){
  fputc('[', out);
  for(size_t i = 0; i < t->sizes[dim]; i++){
    size_t off = offset + i * t->strides[dim];
    if(dim == t->ndim - 1) write_tensor_element(out, t, off, width);
    else                   write_tensor_rec(out, t, dim + 1, off, width);

    if(i + 1 == t->sizes[dim]) continue;                       // nothing after the last 
    if(dim == t->ndim - 1){ fputs(", ", out); continue; }      // between numbers in a row

    fputc(',', out);
    for(size_t k = 0; k < t->ndim - dim - 1; k++) fputc('\n', out);  // newline, plus a 
    for(size_t k = 0; k <= dim; k++) fputc(' ', out);               // line up under the 
  }
  fputc(']', out);
}
          
void write_tensor(FILE *out, Tensor *t){
  write_tensor_rec(out, t, 0, 0, tensor_max_width(t));
}

// Prints a single value followed by one newline. Used directly for
// values nested inside other values (tuple items), so they don't pick up
// the blank lines that print_value puts around top-level results.
static void print_value_bare(Arena *ta, Environment *e, Value *value){
	switch(value->valueType){
		case VALUE_ID:
			{
				Value resolved = resolve_variable(ta, e, value);
				if(resolved.valueType == VALUE_NAN){
					printf("%s\n", value->value.id);
				}else{
					print_value_bare(ta, e, &resolved);
				}
				break;
			}
		case VALUE_NAN:
			{
				printf("nan\n");
				break;
			}
		case VALUE_NUMBER:
			{
        char buf[32];
        format_number(buf, 32, value->value.number, DISPLAY_SIG);
				printf("%s\n", buf);
				break;
			}
  case VALUE_COMPLEX: 
      {
        double complex z = value->value.complex_number;
        char buf[80];
        if(fabs(cimag(z)) > 1e-12 * MAX(1.0, fabs(creal(z)))) format_complex(buf, sizeof(buf), z,
            DISPLAY_SIG, false);
        else                                                  format_number(buf, sizeof(buf),
            creal(z), DISPLAY_SIG);
        printf("%s\n", buf);
        break;
      }
		case VALUE_STRING:
			{
        printf("%s\n", value->value.str);
				break;
			}
		case VALUE_TENSOR:
			{
        write_tensor(stdout, &value->value.tensor);
        putchar('\n');
				break;
			}
		case VALUE_TUPLE:
			{
				printf("(\n");
				for(size_t i = 0; i < value->value.tuple.size; i++){
          print_value_bare(ta, e, &value->value.tuple.items[i]);
				}
				printf(")\n");
				break;
			}
		case VALUE_POLYNOMIAL:
			{
        char buf[1024];
        poly_repr(ta, &value->value.poly, buf, "x");
        printf("%s\n", buf);
				break;
			}
		case VALUE_FUNCTION:
			{
        char buf[1024];
        if(value->value.fun.kind == FN_NATIVE){
          function_repr(
              ta, 
              value->value.fun.native_data.argc_min,
              value->value.fun.native_data.argc_max,
              natives[value->value.fun.native_data.native_index].name,
              buf
          );
        }else{
          function_repr(
              ta, 
              value->value.fun.definition->as.fn_def.num_args,
              value->value.fun.definition->as.fn_def.num_args,
              value->value.fun.definition->as.fn_def.id,
              buf
          );
        }
        printf("%s%s\n", value->value.fun.kind == FN_NATIVE ? "native " : "", buf);
				break;
			}
		case VALUE_TRANSFER_FUNCTION:
			{
        char buf[1024];
        transfer_function_repr(ta, &value->value.tf, buf, sizeof(buf));
        printf("%s\n", buf);
				break;
			}
		default:
			{
				break;
			}
	}
}

// Top-level printing (print() and REPL results): the value surrounded by
// blank lines
void print_value(Arena *ta, Environment *e, Value *value){
  printf("\n");
  print_value_bare(ta, e, value);
  printf("\n");
}

int run_program(Arena *ta, Environment *e, FILE *f, char *script_path);

char *path_join_dir(Arena *a, const char* file, const char *rel){
	// Example:
	// a/b/script.akt + x.akt -> a/b/x.akt
	// script.tx -> current directory

	const char *last_slash = file ? strrchr(file, '/') : NULL;
	size_t dir_len         = last_slash ? (size_t)(last_slash - file) : 1;
	const char *dir        = last_slash ? file : ".";

	size_t len = dir_len + 1 + strlen(rel);
	char *out  = arena_alloc(a, len + 1);
	snprintf(out, len + 1, "%.*s/%s", (int)dir_len, dir, rel);
	return out;

}

char *resolve_path(Arena *a, char* script_path, char *exe_path, char *path){
  char *candidate = NULL;

  // base.akt            -> executable_path/libs/base.akt
  // ./folder/script.akt -> caller_path/folder/script.akt
  // Absolute paths are kept

	// Absolute
  if(path[0] == '/'){ return clone_string(a, path); }

  if(path[0] == '~' && path[1] == '/'){
    const char *home = getenv("HOME");
    if(!home) return NULL;

    // Example:
    // path     = ~/scripts/a.akt              (15 chars)
    // home     = /home/user                   (10 chars)
    // combined = /home/user/scripts/a.akt     (24 chars) -> len1 + len2 - 1

    size_t len       = strlen(home) + strlen(path) - 1;
    candidate        = arena_alloc(a, len + 1);
    sprintf(candidate, "%s%s", home, path + 1);
    candidate[len]   = '\0';
    return candidate;
  }

	if(
			strncmp(path, "./", 2)  == 0 ||
			strncmp(path, "../", 3) == 0
		){ return path_join_dir(a, script_path, path); }

	char *lib = path_join_dir(a, exe_path, "libs/x"); // Placeholder so that we can reuse this fn
	lib[strlen(lib) - 1] = '\0';
	return path_join_dir(a, lib, path);
}

int import_script(Arena *ta, Environment *e, char* importer, char *path){ 
  char *resolved_path = resolve_path(ta, importer, e->exe_path, path);
  FILE *f             = fopen(resolved_path, "r");
  if(f == NULL){
    perror(path);
    return 1;
  }
  int code = run_program(ta, e, f, resolved_path);
  fclose(f);
  return code;
}


Value unary_callable_poly_num(Arena *a, Environment *e, Value *caller, Value *arg){
  return make_num(poly_horner(caller->value.poly, arg->value.number));
}
Value unary_callable_poly_complex(Arena *a, Environment *e, Value *caller, Value *arg){
	double complex x = as_complex(arg);
  return make_complex(poly_horner_cx(caller->value.poly, x));
}
Value unary_callable_poly_tensor(Arena *a, Environment *e, Value *caller, Value *arg){
	Tensor t    = arg->value.tensor;
	DType dtype = t.dtype;
	
	Tensor tout;
	if(tensor_init_like(a, &tout, &t, t.dtype) != 0){
		runtime_error(e, "Failed to initialize tensor");
		return make_nan();
	}

	for(size_t i = 0; i < tout.count; i++){
		if(dtype == DTYPE_REAL){
			tout.data.re[i] = poly_horner(caller->value.poly, t.data.re[i]);
		}else{
			tout.data.cx[i] = poly_horner_cx(caller->value.poly, t.data.cx[i]);
		}
	}

	return make_tensor(tout);
}

Value unary_callable_tf_num(Arena *a, Environment *e, Value *caller, Value *arg){
  double num_val = poly_horner(caller->value.tf.num, arg->value.number);
  double den_val = poly_horner(caller->value.tf.den, arg->value.number);
  return make_num(num_val/den_val);
}

Value unary_callable_tf_complex(Arena *a, Environment *e, Value *caller, Value *arg){
	double complex x = as_complex(arg);
  double complex num_val = poly_horner_cx(caller->value.tf.num, x);
  double complex den_val = poly_horner_cx(caller->value.tf.den, x);
  return make_complex(num_val/den_val);
}

Value unary_callable_tf_tensor(Arena *a, Environment *e, Value *caller, Value *arg){
	Tensor t    = arg->value.tensor;
	DType dtype = t.dtype;
	
	Tensor tout;
	if(tensor_init_like(a, &tout, &t, t.dtype) != 0){
		runtime_error(e, "Failed to initialize tensor");
		return  make_nan();
	}

	for(size_t i = 0; i < tout.count; i++){
		if(dtype == DTYPE_REAL){
			double x           = t.data.re[i];
			double num         = poly_horner(caller->value.tf.num, x);
			double den         = poly_horner(caller->value.tf.den, x);
			tout.data.re[i]    = num / den;
		} else {
			double complex x   = t.data.cx[i];
			double complex num = poly_horner_cx(caller->value.tf.num, x);
			double complex den = poly_horner_cx(caller->value.tf.den, x);
			tout.data.cx[i]    = num / den;
		}
	}

	return make_tensor(tout);
}

UnaryValueDispatch callable_values_dispatch[VALUE_TYPE_COUNT][VALUE_TYPE_COUNT] = {
  [VALUE_POLYNOMIAL] = {
    [VALUE_NUMBER]  = unary_callable_poly_num,
    [VALUE_COMPLEX] = unary_callable_poly_complex,
    [VALUE_TENSOR]  = unary_callable_poly_tensor,
  },
  [VALUE_TRANSFER_FUNCTION] = {
    [VALUE_NUMBER]  = unary_callable_tf_num,
    [VALUE_COMPLEX] = unary_callable_tf_complex,
    [VALUE_TENSOR]  = unary_callable_tf_tensor,
  },
};



void initialize_native_fns(Environment *e){
  for(size_t i = 0; i < LEN(natives); i++){
    NativeFunctionData data = {
      .argc_min     = natives[i].argc_min,
      .argc_max     = natives[i].argc_max,
      .native_index = i
    };
    Function native_fn = {
      .kind        = FN_NATIVE,
      .native_data = data
    };
    Value fn = make_function(native_fn);
    env_set_variable(e, natives[i].name, &fn);
  }
}

Value eval_postfix(Arena *ta, Environment *e, char *script_path, AST *postfix, int *silent){
	AST *target = postfix->as.postfix_op.target;
	char *name  = postfix->as.postfix_op.op;

	Value val = eval_expr(ta, e, script_path, target, silent);
	if(val.valueType == VALUE_NAN) return val;

	for(size_t i = 0; i < LEN(postfix_fn_list); i++){
		if(strcmp(name, postfix_fn_list[i].name) == 0){
			Value out = postfix_fn_list[i].fn(ta, e, &val);
			return out;
		}
	}

	runtime_error(e, "Undefined postfix operator %s", name);
	return make_nan();
}

Value eval_func_call(Arena *ta, Environment *e, char *script_path, AST *fn, int *silent){

  char *name  = fn->as.fn_call.fn_name;
  size_t argc = fn->as.fn_call.num_arguments;

	// Check if it is a user-defined function

	Variable *var = env_find_variable(e, name);
	if(var && var->value->valueType == VALUE_FUNCTION){
		return eval_function(ta, e, fn, script_path, silent);
	}


  if(argc == 1){
		Value val = eval_expr(ta, e, script_path, fn->as.fn_call.first_arg, silent);

    // ---------- Check if there's a suitable variable with name fn_name -----------

    Variable *var = env_find_variable(e,  fn->as.fn_call.fn_name);
    if(var != NULL){
      if(var->value->valueType >= VALUE_TYPE_COUNT ||
         val.valueType >= VALUE_TYPE_COUNT)
      {
				runtime_error(e, "Unsupported operation!");
        return make_nan();
      }

      UnaryValueDispatch dispatch = 
        callable_values_dispatch[var->value->valueType][val.valueType];


      if(!dispatch){
				runtime_error(e, "Unsupported operation!");
        return make_nan();
      }

      return dispatch(ta, e, var->value, &val);
       
    }

  }

	runtime_error(e, "Undefined function %s", name);
	return make_nan();

}

Value eval_indexing(Arena *ta, Environment *e, char *script_path, AST *indexing, int *silent){

  char *name = indexing->as.indexing.var_name;
	Value val = eval_expr(ta, e, script_path, indexing->as.indexing.index_expr, silent);
	if(val.valueType != VALUE_NUMBER){
		runtime_error(e, "Indices must be numbers.%s", name);
		return make_nan();
	}
  int idx = (int)val.value.number;

	// Check if it is an indexable variable

	Variable *var = env_find_variable(e, name);
	if(!var){
		runtime_error(e, "Undefined variable %s", name);
		return make_nan();
	}

	if(
			!(
					var->value->valueType == VALUE_TENSOR ||
					var->value->valueType == VALUE_TUPLE 
			)
		)
	{
		runtime_error(e, "Can't index from variable %s", name);
		return make_nan();
	}

	if(var->value->valueType == VALUE_TENSOR){
		Tensor *t = &var->value->value.tensor;
		if(t->ndim != 1){
			runtime_error(e, "Only 1-D tensors can be indexed for now: %s", name);
			return make_nan();
		}
		int size = (int)t->count;
		if(idx >= size || size == 0){
			runtime_error(e, "Index out of bounds: %s", name);
			return make_nan();
		}
		size_t off = (size_t)modulo(idx, size) * t->strides[0];
		if(t->dtype == DTYPE_COMPLEX) return make_complex(t->data.cx[off]);
		return make_num(t->data.re[off]);
	}

	if(var->value->valueType == VALUE_TUPLE){
		int size = (int)var->value->value.tuple.size;
		if(idx >= size){
			runtime_error(e, "Index out of bounds: %s", name);
			return make_nan();
		}
		return var->value->value.tuple.items[modulo(idx, size)];
	}

	return make_nan();

}

Value eval_array(Arena *ta, Environment *e, char *script_path, AST *array, int *silent){
	size_t num_rows = 0;
	for(AST *row = array->as.array.first; row != NULL; row = row->next) num_rows++;

	if(num_rows == 0){
		// Empty literal []: a 1-D tensor with no elements
		size_t sizes[MAX_DIMS] = {0};
		Tensor t;
		if(tensor_init(ta, &t, 1, sizes, DTYPE_REAL) != 0) return make_nan();
		return make_tensor(t);
	}

	bool dtype_set = false;
	DType dtype    = DTYPE_REAL;

	if(num_rows == 1){
		// This is a vector
		size_t num_els = 0;
		AST *row       = array->as.array.first;
		for(AST *expr  = row->as.row.first; expr != NULL; expr = expr->next) num_els++;

		size_t sizes[MAX_DIMS] = {0};
		sizes[0]               = num_els;

		Tensor t;
		tensor_init(ta, &t, 1, sizes, dtype);

		Value *values = arena_alloc(ta, num_els * sizeof(Value));
		size_t idx = 0;
		for(AST *expr  = row->as.row.first; expr != NULL; expr = expr->next) {
			values[idx] = eval_expr(ta, e, script_path, expr, silent);
			if(values[idx].valueType != VALUE_NUMBER && values[idx].valueType != VALUE_COMPLEX){
				runtime_error(e, "Arrays can only be built of numeric values");
				return make_nan();
			}
			if(values[idx].valueType == VALUE_COMPLEX) dtype = DTYPE_COMPLEX;
			idx++;
		}
		size_t indices[MAX_DIMS] = {0};
		for(size_t i = 0; i < num_els; i++){
			indices[0] = i;
			size_t offset = tensor_get_offset(&t, indices);
			if(dtype == DTYPE_REAL) t.data.re[offset] = values[i].value.number;
			else                    t.data.cx[offset] = values[i].value.complex_number;
		}
		t.dtype = dtype;

		return make_tensor(t);
	}

	// This is a matrix
	// Important: we assume that the number of columns is given by the number
	// of elements in the first row

	size_t num_cols = 0;
	AST *first_row  = array->as.array.first;
	for(AST *expr   = first_row->as.row.first; expr != NULL; expr = expr->next) num_cols++;

	size_t sizes[MAX_DIMS] = {0};
	sizes[0]               = num_rows;
	sizes[1]               = num_cols;

	Tensor t;
	tensor_init(ta, &t, 2, sizes, dtype);

	Value *values = arena_alloc(ta, t.count * sizeof(Value));

	size_t i = 0;
	size_t j = 0;
	size_t indices[MAX_DIMS] = {0};
	for(AST *row = array->as.array.first; row != NULL; row = row->next){
		j = 0;
		for(AST *expr = row->as.row.first; expr != NULL; expr = expr->next){
			indices[0] = i;
			indices[1] = j;
			size_t idx = tensor_get_offset(&t, indices);
			values[idx] = eval_expr(ta, e, script_path, expr, silent);
			if(values[idx].valueType != VALUE_NUMBER && values[idx].valueType != VALUE_COMPLEX){
				runtime_error(e, "Arrays can only be built of numeric values");
				return make_nan();
			}
			if(values[idx].valueType == VALUE_COMPLEX) dtype = DTYPE_COMPLEX;
			j++;
		}
		i++;
	}

	for(size_t idx = 0; idx < t.count; idx++){
		tensor_unravel_index(&t, indices, idx);
		if(dtype == DTYPE_REAL) t.data.re[idx] = values[idx].value.number;
		else                    t.data.cx[idx] = values[idx].value.complex_number;
	}
	t.dtype = dtype;

	return make_tensor(t);
}


Value eval_expr(Arena *ta, Environment *e, char *script_path, AST *expr, int *silent){
	Value value = {0};

	switch(expr->type){
		case AST_ID:
			{
				value = resolve_variable(ta, e, &expr->as.literal);
				break;
			}
		case AST_LITERAL:
			{

				Value literal = expr->as.literal;
				if(literal.valueType == VALUE_STRING){
					literal = resolve_variable(ta, e, &expr->as.literal);
				}
				value = literal;
				break;
			}
		case AST_ARRAY:
			{
				value = eval_array(ta, e, script_path, expr, silent);
				// value.valueType        = VALUE_ARRAY;
				// value.value.array.size = expr->as.array.size;
				// value.value.array.nums = arena_alloc(ta, expr->as.array.size * sizeof(double));
				// AST *item              = expr->as.array.first;
				// size_t i               = 0;
				// while(item != NULL){
				// 	Value item_value = eval_expr(ta, e, script_path, item, silent);
				// 	// TODO: check if it really is a number
				// 	value.value.array.nums[i++] = item_value.value.number;
				// 	item                        = item->next;
				// }
				break;
			}
		case AST_TUPLE:
			{
				value.valueType        = VALUE_TUPLE;
				value.value.tuple.size = expr->as.tuple.size;
				value.value.tuple.items = arena_alloc(ta, expr->as.tuple.size * sizeof(Value));
        size_t i = 0;
        for(AST *item = expr->as.tuple.first; item != NULL; item = item->next){
					Value item_value = eval_expr(ta, e, script_path, item, silent);
					value.value.tuple.items[i] = item_value;
          i++;
        }
				break;
			}
		case AST_BINARY_OP:
			{
				Value left  = eval_expr(ta,  e, script_path, expr->as.binop.left, silent);
				Value right = eval_expr(ta,  e, script_path, expr->as.binop.right, silent);
				value       = eval_binop(ta, e, expr, &left, &right);
				break;
			}
		case AST_UNARY_OP:
			{
				Value arg = eval_expr(ta,  e, script_path, expr->as.unop.arg, silent);
				value     = eval_unop(ta, e, expr, &arg);
				break;
			}
		case AST_FN_CALL:
			{
				value = eval_func_call(ta, e, script_path, expr, silent);
				break;
			}
		case AST_POSTFIX_OP:
			{
				value = eval_postfix(ta, e, script_path, expr, silent);
				break;
			}
		case AST_INDEXING:
			{
				value = eval_indexing(ta, e, script_path, expr, silent);
				break;
			}
		default:
			{
				fprintf(stderr, "Unreachable [eval_expr]\n");
				exit(1);
			}
	}

	return value;
}

ExecStatus eval_block(
    Arena *ta,
    AST *ast,
    Environment *e,
    char *script_path,
    AST *block,
    Value *result_out
) {
    Value result = make_num(0);

    for (AST *stmt = block->as.statement_list.first; stmt != NULL; stmt = stmt->next) {

        if (stmt->statement_type == STATEMENT_EXPR) {
            int silent = 1;
            result = eval_expr( ta, e, script_path, stmt, &silent);
        } else {
            int err = eval_stmt(ta, ast, e, script_path, stmt, result_out);
            if (err != 0) return err;

            result = make_num(0);
        }
    }

    if (result_out)
        *result_out = result;

    return 0;
}

ExecStatus eval_stmt(
		Arena* ta,
		AST *ast,
		Environment *e,
		char *script_path,
		AST *stmt,
		Value *result_out
		){
  int silent = 0;

	if(stmt->statement_type == STATEMENT_EXPR){
    SCRATCH_SCOPE(ta){
      Value result = eval_expr(ta, e, script_path, stmt, &silent);
      if(!silent) print_value(ta, e, &result);
    }
		return 0;
	}

	if(stmt->statement_type == STATEMENT_DECL_ASSIGN){
		char *dst = stmt->as.statement.dst;
		// Check if it hasn't been defined before
    if(env_find_local(e, dst) != NULL){
			runtime_error(e, "Variable %s has already been defined", dst);
			return 1;
    }

		//  TODO: Check if it's not null
    SCRATCH_SCOPE(ta){
      Value value = eval_expr(ta, e, script_path, stmt->as.statement.expr, &silent);
      Value saved = value_clone(env_storage(e), &value);
      env_set_variable(e, dst, &saved);
    }
		return 0;
	}

	if(stmt->statement_type == STATEMENT_ASSIGN){
		char *dst = stmt->as.statement.dst;
		// Check if it has been defined before
    if(env_find_local(e, dst) == NULL){
			runtime_error(e, "Variable %s has not been defined", dst);
			return 1;
    }

		//  TODO: Check if it's not null
    SCRATCH_SCOPE(ta){
      Value value = eval_expr(ta, e, script_path, stmt->as.statement.expr, &silent);
      Value saved = value_clone(env_storage(e), &value);
      env_set_variable(e, dst, &saved);
    }
		return 0;
	}

	if(stmt->statement_type == STATEMENT_FUNCTION_DEF){
		char *id = stmt->as.fn_def.id;

		// Check if it has been defined before
		if(env_find_variable(e, id) != NULL){
			runtime_error(e, "There's already a variable with the name: %s", id);
			return 1;
		}

    Function fun = {
      .kind       = FN_USER,
      .definition = stmt
    };
    Value value   = make_function(fun);
    env_set_variable(e, id, &value);

		return 0;
	}

  if(stmt->statement_type == STATEMENT_IF){
    Value cond = eval_expr(
        ta, e, script_path,
        stmt->as.if_stmt.condition, &silent
        );

    if (cond.valueType != VALUE_NUMBER) {
			runtime_error(e,"Expected numeric condition");
      return 1;
    }

    int cond_true = cond.value.number != 0.0;

    AST *branch = cond_true
      ? stmt->as.if_stmt.if_block
      : stmt->as.if_stmt.else_block;

    if (branch == NULL)
      return 0;

    if (!cond_true && stmt->as.if_stmt.else_is_elif) {
      return eval_stmt(ta, ast, e, script_path, branch, result_out);
    }

    return eval_block(ta, ast, e, script_path, branch, result_out);
  }

  if(stmt->statement_type == STATEMENT_FOR){

    Value iterable = eval_expr(ta, e, script_path, stmt->as.for_stmt.expr, &silent);
    if(iterable.valueType != VALUE_TENSOR || iterable.value.tensor.ndim != 1){
      runtime_error(e, "for: expected a 1-D array");
      return EXEC_ERROR;
    } 
    Tensor *it = &iterable.value.tensor;

    char *id = stmt->as.for_stmt.id->as.literal.value.id;
    for(size_t i = 0; i < it->count; i++){
      size_t scratch = arena_save(ta);   // iterable lives below this mark, so it survives

      size_t off = i * it->strides[0];
      Value item = it->dtype == DTYPE_COMPLEX ? make_complex(it->data.cx[off])
                                              : make_num(it->data.re[off]);
      env_set_variable(e, id, &item);

      Value body_result;
      ExecStatus status = eval_block(ta, ast, e, script_path, stmt->as.for_stmt.block, &body_result);
      if(status == EXEC_RETURN){
        if(result_out) *result_out = body_result;   // don't restore: it may point into ta
        return EXEC_RETURN;
      } 
      if(status != EXEC_OK) return status;

      arena_restore(ta, scratch);
    }   
    return EXEC_OK;
  }

  if(stmt->statement_type == STATEMENT_RETURN){
		if(!result_out) return EXEC_RETURN;
		if(!stmt->as.statement.expr){ *result_out = make_num(0); return EXEC_RETURN; }
		*result_out = eval_expr(ta, e, script_path, stmt->as.statement.expr, &silent);
		return EXEC_RETURN;
  }

	return 0;
}

int run_ast(Arena* ta, AST *ast, Environment *e, char *script_path){

	AST *stmt = ast->as.program.statement_list->as.statement_list.first;

	while(stmt != NULL){
		ExecStatus status = eval_stmt(ta, ast, e, script_path, stmt, NULL);
		if(status == EXEC_RETURN){
			printf("Return outside function body\n");
		}
		if(status != EXEC_OK) return status;
		stmt = stmt->next;
	}

	return 0;
}


int parse_source(Environment *e,  const char *source, size_t source_len, AST *ast){
	// Null-terminated

  // --------- Commands that require parsing -----------

	Token  *tokens    = arena_alloc(e->persistent, (source_len + 1) * sizeof(Token));
	size_t num_tokens = 0;
	int    err        = 0;
	err = tokenize(e->persistent, source, source_len, tokens, &num_tokens);
	if(err != 0){
		fprintf(stderr, "Tokenization error\n");
		return err;
	}

  // for(size_t i = 0; i < num_tokens; i++){
  //   token_print(&tokens[i]);
  // }

	return parse_tokens(e->persistent, ast, tokens, num_tokens);
}

//============================== 
//
//    Helpers
//
//============================== 

size_t trim_end(char *str){
	// Ex:       this is a string      \0
	// Produces: this is a string\0
	// Assumes string to be null-terminated
	// Mutates the string in-place

	size_t size = strlen(str);
	for(int i = size - 1; i >= 0; i--){
		if(isspace(str[i])){
			str[i] = '\0';
		}else{
			break;
		}
	}

	return strlen(str);
}

//============================== 
//
//    Main
//
//============================== 

void print_usage(){
	printf("Usage: aktua [FILE]\n");
	printf("    Runs an Aktua script located at FILE\n");
	printf("    If a FILE argument isn't supplied, a repl is launched\n");
}

void cleanup(Arena *ta, Arena *a){
	arena_destroy(ta);
	arena_destroy(a);
}

int launch_repl(Arena *ta, Environment *e, char *script_path){
  char *buffer      = NULL; // Null-terminated
  size_t size       = 0;
  ssize_t nread     = 0;
  ssize_t last_good = 0;
  int parse_code    = 0;
  int stop          = 0;

  while(1){

    printf("aktua> ");
    nread = getline(&buffer, &size, stdin);
    if(nread == -1){
      printf("\nNo lines were read. Exiting...\n");
      break;
    }

    // Trimming the endline characters
    last_good         = strcspn(buffer, "\r\n");
    buffer[last_good] = '\0';

    trim_end(buffer);

    // --------- Basic commands -----------

    if (strcmp(buffer, "") == 0)
      continue;

    if (strcmp(buffer, "q") == 0) {
      printf("\nExiting...\n");
      break;
    }

    // Tokenization and parsing use the permanent arena
    AST *ast   = arena_alloc(e->persistent, sizeof(AST));
    parse_code = parse_source(e,  buffer, strlen(buffer), ast);

    // Evaluation uses temporary memory
    SCRATCH_SCOPE(ta){
      if(parse_code == 0) run_ast(ta, ast, e, script_path);
    }
    if(parse_code != 0){
      printf("Syntax error\n");
      continue;
    }

  }
  free(buffer);
  return parse_code;
}

char *read_entire_file(size_t *out_len, FILE *f){
  if(fseek(f, 0, SEEK_END) != 0){
    perror("Error seeking to end of file");
    return NULL;
  }

  long file_size = ftell(f);
  if(file_size < 0){
    perror("Error telling file size");
    return NULL;
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    perror("Error seeking to start of file");
    return NULL;
  }

  char *buffer = malloc(file_size + 1);
  if(!buffer){
    perror("Memory allocation failed");
    return NULL;
  }

  size_t bytes_read = fread(buffer, 1, file_size, f);
  if (bytes_read < (size_t)file_size && ferror(f)) {
    perror("Error reading file");
    free(buffer);
    return NULL;
  }
  buffer[bytes_read] = '\0';

  if(out_len != NULL){
    *out_len = bytes_read;
  }

  return buffer;

}

int run_program(Arena *ta, Environment *e, FILE *f, char *script_path){
	size_t len         = 0;
	int parse_code     = 0;

  char *buffer = read_entire_file(&len, f);

  // Tokenization and parsing use the permanent arena
  AST *ast     = arena_alloc(e->persistent, sizeof(AST));
  parse_code   = parse_source(e,  buffer, len, ast);

  // ast_print(ast, 0);

  // Evaluation uses temporary memory
  SCRATCH_SCOPE(ta){
    if(parse_code == 0) run_ast(ta, ast, e, script_path);
  }

  free(buffer);
  if(parse_code != 0){
    printf("Exiting with code %i...\n", parse_code);
  }
	return parse_code;
}


int main(int argc, char **argv){
	size_t size        = 0;
	ssize_t nread      = 0;
	ssize_t last_good  = 0;
	int parse_code     = 0;
	RunStats run_stats = {0};

	Arena ta      = arena_create(32 * 1024 * 1024); // 32 MB for now
	Arena pa      = arena_create(32 * 1024 * 1024); // 32 MB for now
	Arena fa      = arena_create(1 * 1024 * 1024);  // Frame arena
	Environment e = env_create(&pa, &fa, &run_stats, argv[0]);

	if(argc < 2){
		// REPL
		int code = launch_repl(&ta, &e, argv[0]);
		cleanup(&ta, &pa);
		return code;
	}else{

		if(strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0){
			print_usage();
			cleanup(&ta, &pa);
			exit(0);
		}

		if(strcmp(argv[1], "--builtins") == 0){
      for(size_t i = 0; i < LEN(natives); i++) printf("%s\n", natives[i].name);
			cleanup(&ta, &pa);
			exit(0);
		}


		// Launch a program

		FILE *f = fopen(argv[1], "r");

		if(f != NULL){
			int code = run_program(&ta, &e, f, argv[1]);
			if(run_stats.failed_asserts > 0 || run_stats.runtime_errors > 0){ 
				fprintf(stderr, "%zu assertion(s) failed, %zu runtime error(s)\n",
						run_stats.failed_asserts, run_stats.runtime_errors);
				return 1; 
			}
			fclose(f);
			return code;
		}else{
			fprintf(stderr, "Could not load file at %s\n", argv[1]);
			return 1;
		}

	}

	return 0;
}
