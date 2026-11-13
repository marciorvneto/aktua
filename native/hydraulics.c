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

static const NativeFn hydraulics_natives[] = {
	{ "swamee_jain", native_swamee_jain, 2, 2 },
};


const NativeModule hydraulics_module = { "hydraulics", hydraulics_natives, LEN(hydraulics_natives) };
