#ifndef AKTUA_CORE_H
#define AKTUA_CORE_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <complex.h>

#define LEN(x) (sizeof((x))/sizeof((x)[0]))
#define UNREACHABLE(x) do { fprintf(stderr, "Unreachable: " x); exit(1); } while(0);
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#define MIN(x,y) ((x) < (y) ? (x) : (y))

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
void *arena_alloc(Arena *a, size_t size);
size_t arena_save(Arena *a);
void   arena_restore(Arena *a, size_t offset);

//============================== 
//
//    Value types
//
//============================== 

typedef enum {
	VALUE_ID,
	VALUE_NUMBER,
	VALUE_BOOLEAN,
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

typedef struct {
	double* coeffs;
	size_t  num_coeffs;
	size_t  deg;
} Polynomial;

typedef struct {
	Polynomial num;
	Polynomial den;
	double     Ts;   // Sample time. Ts = 0 means CT, Ts > 0 means DT
} TransferFunction;

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

typedef struct Value Value;
typedef struct KwArg KwArg;
typedef struct {
	Value   *items;
	size_t   size;
} Tuple;


typedef enum {
  FN_NATIVE,
  FN_USER
} FunctionKind;

typedef struct NativeFn NativeFn;
typedef struct {
  const NativeFn    *native;
  size_t             argc_min;
  size_t             argc_max;
} NativeFunctionData;

//=============================================
//
//   Dispatcher types and native functions
//
//=============================================

#define NATIVE_PARAMS Arena *a, Environment *e, char *script, size_t argc, Value *argv, size_t kwargc, KwArg *kwargs, int *silent

typedef struct  Environment  Environment;
typedef Value (*NativeValueDispatch)(NATIVE_PARAMS);
typedef Value (*UnaryValueDispatch)(Arena *a, Environment *e, Value *caller, Value *value);
struct NativeFn {
  const char          *name;
  NativeValueDispatch fn;
  size_t              argc_min;
  size_t              argc_max;
	const char *const  *kwargs;
};


typedef struct {
  FunctionKind kind; 
  union {
    struct AST*        definition;
    NativeFunctionData native_data;
  };
} Function;

struct Value {
	ValueType valueType;
	union {
		Polynomial       poly;
		TransferFunction tf;
		bool             boolean;
		double           number;
		double complex   complex_number;
		char             *str;
		char             *id;
		Tensor           tensor;
		Tuple            tuple;
    Function         fun;
	} value;
};

struct KwArg {
	const char *name;
	Value      value;
};

Value call_function(Arena *ta, Environment *e, char *script_path, const char *name,
										Value *fun, size_t argc, Value *args, size_t kwargc,
										KwArg *kwargs, int *silent);

const char *fn_name(Value *fn);

// ----------- Factories -----------------

Value make_num(double num);
Value make_nan();
Value make_complex(double complex num);
Value make_tensor(Tensor tens);
Value make_function(Function fun);
Value value_clone(Arena *a, Value *v);

inline static char *cstr_clone(Arena *a, const char *cstr){
	size_t len  = strlen(cstr);
	char *clone = arena_alloc(a, len + 1);
	memcpy(clone, cstr, len);
	clone[len]  = '\0';
	return clone;
}

// ----------- Tensor helpers -----------------

int as_real_vector(Arena *a, Value *v, double **data, size_t *n);
Value make_real_vector(double *data, size_t n);
Value make_real_matrix(double *data, size_t nrow, size_t ncol);
Value make_tuple(Tuple tuple);

//============================== 
//
//    Runtime
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
	size_t      scope_start;
	Environment *parent;
	RunStats    *run_stats;
	char        *exe_path;
};

#ifdef __MINGW_PRINTF_FORMAT
#define AKTUA_PRINTF __MINGW_PRINTF_FORMAT
#else
#define AKTUA_PRINTF printf
#endif

void runtime_error(Environment *e, const char *fmt, ...)
	__attribute__((format(AKTUA_PRINTF, 2, 3)));

//============================== 
//
//    Encoding
//
//============================== 

static inline size_t utf8_width(const char *s){
	// Explanation: UTF-8 chars may take more than 1 byte.
	// More details in lineedit.c:utf8_is_continuation
	size_t n = 0;	
	for(;*s;s++) if(((unsigned char)*s & 0xC0) != 0x80) n++;
	return n;
}

//============================== 
//
//    Modules
//
//============================== 

typedef struct {
	const char       *name;
	const NativeFn   *fns;
	size_t            count;
} NativeModule;


extern const NativeModule hydraulics_module;
extern const NativeModule orbital_module;
extern const NativeModule ode_module;

#endif // !AKTUA_CORE_H
