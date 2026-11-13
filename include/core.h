#ifndef AKTUA_CORE_H
#define AKTUA_CORE_H

#include <stdio.h>
#include <stdbool.h>
#include <complex.h>

#define LEN(x) sizeof((x))/sizeof((x)[0])

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
typedef struct {
	Value*  items;
	size_t size;
} Tuple;

typedef enum {
  FN_NATIVE,
  FN_USER
} FunctionKind;

typedef struct NativeFn NativeFn;
typedef struct {
  const NativeFn   *native;
  size_t            argc_min;
  size_t            argc_max;
} NativeFunctionData;

//=============================================
//
//   Dispatcher types and native functions
//
//=============================================

typedef struct  Environment  Environment;
typedef Value (*NativeValueDispatch)(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int *silent);
typedef Value (*UnaryValueDispatch)(Arena *a, Environment *e, Value *caller, Value *value);
struct NativeFn {
  const char          *name;
  NativeValueDispatch fn;
  size_t              argc_min;
  size_t              argc_max;
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

// ----------- Factories -----------------

Value make_num(double num);
Value make_nan();
Value make_complex(double complex num);
Value make_tensor(Tensor tens);
Value make_function(Function fun);
Value value_clone(Arena *a, Value *v);


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
	Environment *parent;
	RunStats    *run_stats;
	char        *exe_path;
};


void runtime_error(Environment *e, const char *fmt, ...);


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

#endif // !AKTUA_CORE_H
