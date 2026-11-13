//==========================
//
//  Module: Hydraulics
//
//==========================

#include "core.h"

#include <stdio.h>
#include <math.h>

static Value native_swamee_jain(Arena *a, Environment *e, char* script, size_t argc, Value *argv, int
		*silent){
	if(argv[0].valueType != VALUE_NUMBER || argv[1].valueType != VALUE_NUMBER){
		runtime_error(e, "swamee_jain expects two numbers: Re and relative roughness");
		return make_nan();
	}       
	double Re = argv[0].value.number;
	double r  = argv[1].value.number;
	double l  = log10(r / 3.7 + 5.74 / pow(Re, 0.9));
	return make_num(0.25 / (l * l));
}       


static Value native_churchill(Arena *a, Environment *e, char *script, size_t argc, Value *argv, int *silent) {
	if (argc != 2 ||
			argv[0].valueType != VALUE_NUMBER ||
			argv[1].valueType != VALUE_NUMBER) {
		runtime_error(e, "churchill expects two numbers: Re and relative roughness");
		return make_nan();
	}

	double Re = argv[0].value.number;
	double r  = argv[1].value.number;

	if (!isfinite(Re) || !isfinite(r) || Re <= 0.0 || r < 0.0) {
		runtime_error(e, "churchill requires Re > 0 and relative roughness >= 0");
		return make_nan();
	}

	double x = pow(7.0 / Re, 0.9) + 0.27 * r;

	double A = pow(-2.457 * log(x), 16.0);
	double B = pow(37530.0 / Re, 16.0);

	double f = 8.0 * pow(
			pow(8.0 / Re, 12.0) + pow(A + B, -1.5),
			1.0 / 12.0
			);

	return make_num(f);
}


static const NativeFn hydraulics_natives[] = {
	{ "swamee_jain", native_swamee_jain, 2, 2 },
	{ "churchill",   native_churchill,   2, 2   },
};


const NativeModule hydraulics_module = { "hydraulics", hydraulics_natives, LEN(hydraulics_natives) };
