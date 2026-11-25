//===============================================
//
//  Module: Ordinary Differential Equations
//
//===============================================

#include "core.h"

#include <stdio.h>
#include <string.h>

// ---------- Kernels ------------------

typedef int(*OdeFn)(double t, double *y, size_t ylen, double *yout, void *data);
typedef int(*OdeKernel)(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data);

// The caller should preallocate ts and yout
// tmp is organized as: [y, y'] -> must be allocated
// for 2*ylen doubles
int euler(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data)
{
	double *state = tmp;
	double *dydt  = tmp + ylen;

	// Copy initial values
	memcpy(yout, y,  ylen * sizeof(double));     // y(0) -> output
	memcpy(state, y,  ylen * sizeof(double));    // y(0) -> initialize temp
	for(size_t i = 1; i < tlen; i++){
		double dt  = ts[i] - ts[i-1];

		if(!fn(ts[i-1], state, ylen, dydt, data)) return 0;
		for(size_t k = 0; k < ylen; k++){
			state[k] += dt * dydt[k];
		}

		memcpy(yout + i * ylen, state,  ylen * sizeof(double));
	}

	return 1;
}

// The caller should preallocate ts and yout
// tmp is organized as: [y, k1, k2, k3, k4, scratch] -> must be allocated
// for 6*ylen doubles
int rk4(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data)
{
	double *state   = tmp;
	double *k1      = tmp + ylen;
	double *k2      = tmp + 2*ylen;
	double *k3      = tmp + 3*ylen;
	double *k4      = tmp + 4*ylen;
	double *scratch = tmp + 5*ylen;

	// Copy initial values
	memcpy(yout, y,  ylen * sizeof(double));     // y(0) -> output
	memcpy(state, y,  ylen * sizeof(double));    // y(0) -> initialize temp
	for(size_t i = 1; i < tlen; i++){
		double t  = ts[i-1];
		double dt = ts[i] - t;

		if(!fn(t, state, ylen, k1, data)) return 0;

		for(size_t k = 0; k < ylen; k++) scratch[k] = state[k] + dt/2 * k1[k];
		if(!fn(t + dt/2, scratch, ylen, k2, data)) return 0;

		for(size_t k = 0; k < ylen; k++) scratch[k] = state[k] + dt/2 * k2[k];
		if(!fn(t + dt/2, scratch, ylen, k3, data)) return 0;

		for(size_t k = 0; k < ylen; k++) scratch[k] = state[k] + dt * k3[k];
		if(!fn(t + dt, scratch, ylen, k4, data)) return 0;

		for(size_t k = 0; k < ylen; k++){
			state[k] += dt/6 * (k1[k] + 2*k2[k] + 2*k3[k] + k4[k]);
		}

		memcpy(yout + i * ylen, state,  ylen * sizeof(double));
	}

	return 1;
}



// ---------- Native wrappers ------------------

typedef struct {
	Arena       *a;
	Environment *e;
	char        *script_path;
	const char  *name;
	Value       *fun;
	int         *silent;
} FnCtx;

// TODO: Allow the user to somehow pass an additional collection of values to
// the function. Probably with closures.
int user_defined_rhs(double t, double *y, size_t ylen, double *yout, void *data){
	FnCtx *ctx       = (FnCtx*) data;

	Value t_val = make_num(t);
	Value state = make_real_vector(y, ylen);

	Value argv[] = {t_val, state};

	size_t scratch = arena_save(ctx->a);
	Value out = call_function(ctx->a, ctx->e, ctx->script_path, ctx->name, ctx->fun, 
														2, argv, 0, NULL, ctx->silent);

	if(out.valueType == VALUE_NAN) { arena_restore(ctx->a, scratch); return 0; }
	if(out.valueType == VALUE_NUMBER && ylen == 1){
		arena_restore(ctx->a, scratch);
		yout[0] = out.value.number;
		return 1;
	}

	double *res = NULL;
	size_t n    = 0;
	if(!as_real_vector(ctx->a, &out, &res, &n)){
		runtime_error(ctx->e, "%s: expected a 1D vector", ctx->name);
		arena_restore(ctx->a, scratch);
		return 0;
	}
	if(n != ylen){
		runtime_error(ctx->e, "%s: returned %zu values, state has %zu", ctx->name, n, ylen);
		arena_restore(ctx->a, scratch);
		return 0;
	}

	memcpy(yout, res, ylen * sizeof(double));
	arena_restore(ctx->a, scratch);

	return 1;
}

static Value ode_native(NATIVE_PARAMS, const char *name, OdeKernel kernel, size_t nwork){
	if(argv[0].valueType != VALUE_FUNCTION){
		runtime_error(e, "%s: first argument must be a function", name);
		return make_nan();
	}

	double *ts;
	size_t nt;
	if(!as_real_vector(a, &argv[1], &ts, &nt)){
		runtime_error(e, "%s: second argument must be a 1D array", name);
		return make_nan();
	}

	double  ynumber = 0; // Only used when y0 is a number
	double *y0;
	size_t ylen;
	if(argv[2].valueType == VALUE_NUMBER){
		ynumber = argv[2].value.number;
		y0      = &ynumber;
		ylen    = 1;
	}else{
		if(!as_real_vector(a, &argv[2], &y0, &ylen)){
			runtime_error(e, "%s: third argument must be a 1D array or a number", name);
			return make_nan();
		}
	}

	FnCtx ctx = {
		.fun         = &argv[0],
		.a           = a,
		.e           = e,
		.silent      = silent,
		.script_path = script,
		.name        = fn_name(&argv[0])
	};
	
	double *yout = arena_alloc(a, nt * ylen * sizeof(double));
	double *tmp  = arena_alloc(a,  nwork * ylen * sizeof(double));
	if(!kernel(user_defined_rhs, ts, y0, tmp, nt, ylen, yout, &ctx)) return make_nan();

	if(ylen == 1) return make_real_vector(yout, nt);

	return make_real_matrix(yout, nt, ylen);

}

static Value native_euler(NATIVE_PARAMS) {
	return ode_native(a, e, script, argc, argv, kwargc, kwargs, silent, "euler", euler, 2);
}
static Value native_rk4(NATIVE_PARAMS) {
	return ode_native(a, e, script, argc, argv, kwargc, kwargs, silent, "rk4", rk4, 6);
}

static const NativeFn ode_natives[] = {
	{ "euler", native_euler,         3, 3 },
	{ "rk4",   native_rk4,         3, 3 },
};

const NativeModule ode_module = { "ode", ode_natives, LEN(ode_natives) };
