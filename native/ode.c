//===============================================
//
//  Module: Ordinary Differential Equations
//
//===============================================

#include "core.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

// ---------- Kernels ------------------

static inline double clamp(double x, double a, double b) {
    return (x < a) ? a : (x > b ? b : x);
}

typedef enum {
	SOLVER_OK,
	SOLVER_STEP_TOO_SMALL,
	SOLVER_NOT_FINITE,
	SOLVER_FN_FAILED,
} SolverStatus;

#define TRY(x) do {        \
		SolverStatus _s = (x); \
		if(_s != SOLVER_OK) return _s;} while(0)

typedef SolverStatus(*OdeFn)(double t, double *y, size_t ylen, double *yout, void *data);
typedef SolverStatus(*OdeKernel)(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data, double *tfail);

// The caller should preallocate ts and yout
// tmp is organized as: [y, y'] -> must be allocated
// for 2*ylen doubles
static SolverStatus euler(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data, double *tfail)
{
	double *state = tmp;
	double *dydt  = tmp + ylen;

	// Copy initial values
	memcpy(yout, y,  ylen * sizeof(double));     // y(0) -> output
	memcpy(state, y,  ylen * sizeof(double));    // y(0) -> initialize temp
	for(size_t i = 1; i < tlen; i++){
		double dt  = ts[i] - ts[i-1];

		TRY(fn(ts[i-1], state, ylen, dydt, data));
		for(size_t k = 0; k < ylen; k++){
			state[k] += dt * dydt[k];
		}

		memcpy(yout + i * ylen, state,  ylen * sizeof(double));
	}

	return SOLVER_OK;
}

// The caller should preallocate ts and yout
// tmp is organized as: [y, k1, k2, k3, k4, scratch] -> must be allocated
// for 6*ylen doubles
static SolverStatus rk4(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data, double *tfail)
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

		TRY(fn(t, state, ylen, k1, data));

		for(size_t k = 0; k < ylen; k++) scratch[k] = state[k] + dt/2 * k1[k];
		TRY(fn(t + dt/2, scratch, ylen, k2, data));

		for(size_t k = 0; k < ylen; k++) scratch[k] = state[k] + dt/2 * k2[k];
		TRY(fn(t + dt/2, scratch, ylen, k3, data));

		for(size_t k = 0; k < ylen; k++) scratch[k] = state[k] + dt * k3[k];
		TRY(fn(t + dt, scratch, ylen, k4, data));

		for(size_t k = 0; k < ylen; k++){
			state[k] += dt/6 * (k1[k] + 2*k2[k] + 2*k3[k] + k4[k]);
		}

		memcpy(yout + i * ylen, state,  ylen * sizeof(double));
	}

	return SOLVER_OK;
}

// The caller should preallocate ts and yout
// tmp is organized as: [y, k1, k2, k3, k4, k5, k6, k7, scratch] -> must be allocated
// for 9*ylen doubles
static SolverStatus rk45(OdeFn fn, double *ts, double *y, double *tmp,
					size_t tlen, size_t ylen, double *yout, void *data, double *tfail)
{
	double *state   = tmp;
	double *k1      = tmp + ylen;
	double *k2      = tmp + 2*ylen;
	double *k3      = tmp + 3*ylen;
	double *k4      = tmp + 4*ylen;
	double *k5      = tmp + 5*ylen;
	double *k6      = tmp + 6*ylen;
	double *k7      = tmp + 7*ylen;
	double *scratch = tmp + 8*ylen;


	double a21 = 1.0/5;
	double a31 = 3.0/40,       a32 = 9.0/40;
	double a41 = 44.0/45,      a42 = -56.0/15,      a43 = 32.0/9;
	double a51 = 19372.0/6561, a52 = -25360.0/2187, a53 = 64448.0/6561, a54 = -212.0/729;
	double a61 = 9017.0/3168,  a62 = -355.0/33,     a63 = 46732.0/5247, a64 = 49.0/176,  a65 = -5103.0/18656;
	double a71 = 35.0/384,                          a73 = 500.0/1113,   a74 = 125.0/192, a75 = -2187.0/6784, a76 = 11.0/84;

	double a[] = { 1.0/5,      3.0/10, 4.0/5,       8.0/9};
	double e[] = { 71.0/57600, 0,      -71.0/16695, 71.0/1920, -17253.0/339200, 22.0/525, -1.0/40 };

	double h   = ts[1] - ts[0];

	double atol = 1e-6; // Fixed for now
	double rtol = 1e-3;

	// Copy initial values
	memcpy(yout, y,  ylen * sizeof(double));         // y(0) -> output
	memcpy(state, y,  ylen * sizeof(double));        // y(0) -> initialize temp
	TRY(fn(ts[0], state, ylen, k1, data));  // Precomputation
	for(size_t i = 1; i < tlen; i++){
		double t  = ts[i-1];

		while(t < ts[i]){
			double hs   = MIN(h, ts[i] - t);
			bool capped = hs == ts[i] - t;

			for(size_t k = 0; k < ylen; k++)
				scratch[k] = state[k] + hs * (k1[k]*a21);
			TRY(fn(t + hs*a[0], scratch, ylen, k2, data));

			for(size_t k = 0; k < ylen; k++)
				scratch[k] = state[k] + hs * (k1[k]*a31 + k2[k]*a32);
			TRY(fn(t + hs*a[1], scratch, ylen, k3, data));

			for(size_t k = 0; k < ylen; k++)
				scratch[k] = state[k] + hs * (k1[k]*a41 + k2[k]*a42 + k3[k]*a43);
			TRY(fn(t + hs*a[2], scratch, ylen, k4, data));

			for(size_t k = 0; k < ylen; k++)
				scratch[k] = state[k] + hs * (k1[k]*a51 + k2[k]*a52 + k3[k]*a53 + k4[k]*a54);
			TRY(fn(t + hs*a[3], scratch, ylen, k5, data));

			for(size_t k = 0; k < ylen; k++)
				scratch[k] = state[k] + hs * (k1[k]*a61 + k2[k]*a62 + k3[k]*a63 + k4[k]*a64 + k5[k]*a65);
			TRY(fn(t + hs, scratch, ylen, k6, data));

			for(size_t k = 0; k < ylen; k++)
				scratch[k] = state[k] + hs * (k1[k]*a71 +             k3[k]*a73 + k4[k]*a74 + k5[k]*a75 + k6[k]*a76);
			TRY(fn(t + hs, scratch, ylen, k7, data));

			// Calculate error
			
			double err = 0.0;
			for (size_t k = 0; k < ylen; k++) {
				double err_k = hs * (e[0]*k1[k] + e[1]*k2[k] + e[2]*k3[k] + 
						e[3]*k4[k] + e[4]*k5[k] + e[5]*k6[k] + e[6]*k7[k]);

				double scale = atol + rtol * fmax(fabs(state[k]), fabs(scratch[k]));
				double ratio = err_k / scale;
				err += ratio * ratio;
			}
			err = sqrt(err / ylen);
			if(!isfinite(err)) { *tfail = t; return SOLVER_NOT_FINITE; }

			if(err <= 1){                                      // Accept this step
				t = capped ? ts[i] : t + hs;                     // Snaps to the next grid point
				memcpy(state, scratch,  ylen * sizeof(double));
				memcpy(k1, k7,  ylen * sizeof(double));
			}
			
			h = hs * clamp(0.9 * pow(err, -0.2), 0.2, 5);
			if(h < 1e-12) { *tfail = t; return SOLVER_STEP_TOO_SMALL; }

		}

		memcpy(yout + i * ylen, state,  ylen * sizeof(double));
	}

	return SOLVER_OK;
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
static SolverStatus user_defined_rhs(double t, double *y, size_t ylen, double *yout, void *data){
	FnCtx *ctx       = (FnCtx*) data;

	Value t_val = make_num(t);
	Value state = make_real_vector(y, ylen);

	Value argv[] = {t_val, state};

	size_t scratch = arena_save(ctx->a);
	Value out = call_function(ctx->a, ctx->e, ctx->script_path, ctx->name, ctx->fun, 
														2, argv, 0, NULL, ctx->silent);

	if(out.valueType == VALUE_NAN) { arena_restore(ctx->a, scratch); return SOLVER_FN_FAILED; }
	if(out.valueType == VALUE_NUMBER && ylen == 1){
		arena_restore(ctx->a, scratch);
		yout[0] = out.value.number;
		return SOLVER_OK;
	}

	double *res = NULL;
	size_t n    = 0;
	if(!as_real_vector(ctx->a, &out, &res, &n)){
		runtime_error(ctx->e, "%s: expected a 1D vector", ctx->name);
		arena_restore(ctx->a, scratch);
		return SOLVER_FN_FAILED;
	}
	if(n != ylen){
		runtime_error(ctx->e, "%s: returned %zu values, state has %zu", ctx->name, n, ylen);
		arena_restore(ctx->a, scratch);
		return SOLVER_FN_FAILED;
	}

	memcpy(yout, res, ylen * sizeof(double));
	arena_restore(ctx->a, scratch);

	return SOLVER_OK;
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
	
	double *yout  = arena_alloc(a, nt * ylen * sizeof(double));
	double *tmp   = arena_alloc(a,  nwork * ylen * sizeof(double));
	double  tfail = -1; // In case the solver fails at some time step

	switch(kernel(user_defined_rhs, ts, y0, tmp, nt, ylen, yout, &ctx, &tfail)){
		case SOLVER_OK: break;
		case SOLVER_FN_FAILED: return make_nan(); // Already reported
		case SOLVER_NOT_FINITE: { runtime_error(e, "%s: solver reached a non-finite value at t = %g", name, tfail) ; return make_nan();}
		case SOLVER_STEP_TOO_SMALL: { runtime_error(e, "%s: solver step too small at t = %g", name, tfail) ; return make_nan();}
	}

	if(ylen == 1) return make_real_vector(yout, nt);

	return make_real_matrix(yout, nt, ylen);

}

static Value native_euler(NATIVE_PARAMS) {
	return ode_native(a, e, script, argc, argv, kwargc, kwargs, silent, "euler", euler, 2);
}
static Value native_rk4(NATIVE_PARAMS) {
	return ode_native(a, e, script, argc, argv, kwargc, kwargs, silent, "rk4", rk4, 6);
}
static Value native_rk45(NATIVE_PARAMS) {
	return ode_native(a, e, script, argc, argv, kwargc, kwargs, silent, "rk45", rk45, 9);
}

static const NativeFn ode_natives[] = {
	{ "euler", native_euler, 3, 3 },
	{ "rk4",   native_rk4,   3, 3 },
	{ "rk45",  native_rk45,  3, 3 },
};

const NativeModule ode_module = { "ode", ode_natives, LEN(ode_natives) };
