//=================================
//
//  Module: Orbital Mechanics
//
//=================================

#include "core.h"

#include <stdio.h>
#include <math.h>

// Stumpff functions
// Near z = 0 the closed forms cancel catastrophically, so we
// switch to the Taylor series. At the cutoff the series is good to ~1e-15
// and the closed form to ~1e-13 (cancellation grows like eps / z)

#define STUMPFF_SERIES_CUTOFF 1e-3

static double stumpff_s(double z) {
  if (fabs(z) < STUMPFF_SERIES_CUTOFF) {
    return 1.0 / 6.0 - z / 120.0 + z * z / 5040.0;
  }
  if (z > 0) {
    double sqrt_z = sqrt(z);
    double s_sqrt_z = sin(sqrt_z);
    return (sqrt_z - s_sqrt_z) / (z * sqrt_z);
  }
  if (z < 0) {
    double sqrt_mz = sqrt(-z);
    double sh_sqrt_z = sinh(sqrt_mz);
    return (sh_sqrt_z - sqrt_mz) / (-z * sqrt_mz);
  }
  return 1.0 / 6.0;
}

static double stumpff_c(double z) {
  if (fabs(z) < STUMPFF_SERIES_CUTOFF) {
    return 1.0 / 2.0 - z / 24.0 + z * z / 720.0;
  }
  if (z > 0) {
    return (1 - cos(sqrt(z))) / z;
  }
  if (z < 0) {
    return (cosh(sqrt(-z)) - 1.0) / (-z);
  }
  return 1.0 / 2.0;
}

// Kepler equation

static double universal_anomaly_solve(double mu, double r0, double vr0,
                                     double alpha, double dt, int *flag) {
  double chi = sqrt(mu) * fabs(alpha) * dt; // Universal anomaly
  double tol = 1e-8;
  double err = 1;
  double der = 0;
  double fn = 0;
  double delta = 0;
  double sqrt_mu = sqrt(mu);
  size_t max_iter = 100;
  size_t iter = 0;
  int max_iter_reached = 0;
  while (err > tol) {
    double chi2 = chi * chi;
    double chi3 = chi * chi2;
    double z = alpha * chi2;
    double sz = stumpff_s(z);
    double cz = stumpff_c(z);
    der = r0 * vr0 / sqrt_mu * chi * (1 - alpha * chi2 * sz) +
          (1 - alpha * r0) * chi2 * cz + r0;
    fn = r0 * vr0 / sqrt_mu * chi2 * cz + (1 - alpha * r0) * chi3 * sz +
         r0 * chi - sqrt_mu * dt;
    delta = fn / der;
    chi -= delta;
    err = fabs(delta);
    iter += 1;
    if (iter >= max_iter) {
      max_iter_reached = 1;
      break;
    }
  }

  if (flag != NULL) {
    if (max_iter_reached) {
      *flag = 1;
    } else {
      *flag = 0;
    }
  }

  return chi;
}


static Value native_stumpff_s(NATIVE_PARAMS) {
	if(argv[0].valueType != VALUE_NUMBER){
		runtime_error(e, "stumpff_s expects one number");
		return make_nan();
	}
	double z = argv[0].value.number;
	return make_num(stumpff_s(z));
}

static Value native_stumpff_c(NATIVE_PARAMS) {
	if(argv[0].valueType != VALUE_NUMBER){
		runtime_error(e, "stumpff_c expects one number");
		return make_nan();
	}
	double z = argv[0].value.number;
	return make_num(stumpff_c(z));
}

static Value native_universal_anomaly(NATIVE_PARAMS) {
	if(
			argv[0].valueType != VALUE_NUMBER ||
			argv[1].valueType != VALUE_NUMBER ||
			argv[2].valueType != VALUE_NUMBER ||
			argv[3].valueType != VALUE_NUMBER ||
			argv[4].valueType != VALUE_NUMBER
			)
	{
		runtime_error(e, "universal_anomaly expects five numbers");
		return make_nan();
	}
	int flag     = 0;
	double mu    = argv[0].value.number;
	double r0    = argv[1].value.number;
	double vr0   = argv[2].value.number;
	double alpha = argv[3].value.number;
	double dt    = argv[4].value.number;
	double chi   = universal_anomaly_solve(mu, r0, vr0, alpha, dt, &flag);
	if(flag != 0){
		runtime_error(e, "universal_anomaly: max number of iterations exceeded");
		return make_nan();
	}
	return make_num(chi);
}

static Value native_propagate(NATIVE_PARAMS) {
	if(
			argv[0].valueType != VALUE_TENSOR ||
			argv[1].valueType != VALUE_TENSOR ||
			argv[2].valueType != VALUE_NUMBER ||
			argv[3].valueType != VALUE_NUMBER
			)
	{
		runtime_error(e, "propagate expects: r0, v0, dt, mu");
		return make_nan();
	}
	double dt    = argv[2].value.number;
	double mu    = argv[3].value.number;

	double *r_vec, *v_vec;
	size_t nr, nv;
	if(!as_real_vector(a, &argv[0], &r_vec, &nr) || !as_real_vector(a, &argv[1], &v_vec, &nv)){
		runtime_error(e, "propagate: r0 and v0 must be real vectors");
		return make_nan();
	}
	if(nr != nv){
		runtime_error(e, "propagate: r0 and v0 must have the same number of dimensions");
		return make_nan();
	}
	if(!(mu > 0)){
		runtime_error(e, "propagate: mu must be positive");
		return make_nan();
	}

	double r_norm = 0;
	double v_norm = 0;
	double r_dot_v = 0;
	for(size_t i = 0; i < nr; i++){
		r_norm  += r_vec[i] * r_vec[i];
		v_norm  += v_vec[i] * v_vec[i];
		r_dot_v += r_vec[i] * v_vec[i];
	}
	r_norm = sqrt(r_norm);
	v_norm = sqrt(v_norm);
	if(!(r_norm > 0)){
		runtime_error(e, "propagate: r0 must be nonzero");
		return make_nan();
	}

	double vr0   = r_dot_v / r_norm; // Radial velocity
	double alpha = 2 / r_norm - v_norm * v_norm / mu;
	int flag     = 0;
	double chi   = universal_anomaly_solve(mu, r_norm, vr0, alpha, dt, &flag);

	if(flag != 0){
		runtime_error(e, "propagate: max number of iterations exceeded");
		return make_nan();
	}

	// Lagrange coefficients
	double sqrt_mu = sqrt(mu);
	double chi2    = chi * chi;
	double chi3    = chi * chi2;
	double z       = alpha * chi2;
	double sz      = stumpff_s(z);
	double cz      = stumpff_c(z);

	double f = 1 - chi2 / r_norm * cz;
	double g = dt - chi3 / sqrt_mu * sz;

	double *r_out = arena_alloc(a, nr * sizeof(double));
	double *v_out = arena_alloc(a, nr * sizeof(double));
	Value  *items = arena_alloc(a, 2 * sizeof(Value));
	if(r_out == NULL || v_out == NULL || items == NULL){
		runtime_error(e, "propagate: out of memory");
		return make_nan();
	}

	double r_new = 0;
	for(size_t i = 0; i < nr; i++){
		r_out[i] = f * r_vec[i] + g * v_vec[i];
		r_new   += r_out[i] * r_out[i];
	}
	r_new = sqrt(r_new);

	double fdot = sqrt_mu / (r_new * r_norm) * (alpha * chi3 * sz - chi);
	double gdot = 1 - chi2 / r_new * cz;

	for(size_t i = 0; i < nr; i++){
		v_out[i] = fdot * r_vec[i] + gdot * v_vec[i];
	}

	items[0] = make_real_vector(r_out, nr);
	items[1] = make_real_vector(v_out, nr);
	return make_tuple((Tuple){ .items = items, .size = 2 });
}


static const NativeFn orbital_natives[] = {
	{ "stumpff_s",         native_stumpff_s,         1, 1 },
	{ "stumpff_c",         native_stumpff_c,         1, 1 },
	{ "universal_anomaly", native_universal_anomaly, 5, 5 },
	{ "propagate",         native_propagate,         4, 4 },
};

const NativeModule orbital_module = { "orbital", orbital_natives, LEN(orbital_natives) };
