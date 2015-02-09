#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#ifndef NAN
    static const unsigned long __nan[2] = {0xffffffff, 0x7fffffff};
    #define NAN (*(const float *) __nan)
#endif

#include "m_pd.h"

#include <math.h>
#include <stdlib.h>

/*
	wavecap~
	Chris Donahue (http://cdonahue.me) 2014

	This external is a wavetable oscillator that captures pitch from the envelope of an incoming signal. It also records the data for its wavetable via the first inlet. The 

	The wavecap~ external accepts the following messages:
		* bang					(starts recording a wavetable from inlet 1)
		* table_size n			(n must be an even power of 2) [default: 1024]
		* table_interp n		(n must be 0, 1 or 2 where 0 is truncate, 1 is 2-sample linear interpolation and 2 is 4-sample linear interpolation) [default: 0]
		* env_atk_ms n			(envelope follower attack in ms) [default 500]
		* env_dcy_ms n			(envelope follower decay in ms) [default 10]
		* env_enable			(enables envelope following) [default off]
		* env_disable			(disables envelope following)

	Resources used:
		* Oscil.cpp from in class example on 10/16/14
		* http://musicdsp.org/showArchiveComment.php?ArchiveID=136
*/

static t_class* wavecap_class;

typedef enum {
	truncate,
	lin_2,
	lin_4,
	interp_types_num,
} interp_type;

typedef struct _wavecap {
    t_object x_obj;
	t_float f;
	
	// dsp settings
	int block_size;
	float sample_rate;
	float nyquist_rate;

	// table parameters
	int table_record;
	uint32_t table_size;
	uint32_t table_mask;
	float* table;
	interp_type table_interp;

	// env parameters
	int env_enabled;
	float env_atk_ms;
	float env_dcy_ms;

	// computed
	float env_atk_coeff;
	float env_dcy_coeff;
	float env_last;

	// table oscillator state
	float phase;
	float phaseIncrement;
} t_wavecap;

/*
	bang receiever
*/

static void wavecap_table_record (t_wavecap* x) {
	x->table_record = x->table_size;
	post("recording...");
}

/*
	internal state helpers
*/

static void _wavecap_table_free (t_wavecap* x) {
	if (x->table) {
		free(x->table);
	}
}

static void _wavecap_table_alloc (t_wavecap* x) {
	if (x->table_size > 0) {
		x->table = (float*) calloc(x->table_size, sizeof(float));
	}
	else {
		x->table = NULL;
	}
}

static void _wavecap_table_reset_phase (t_wavecap* x) {
	x->phase = 0.0f;
	x->phaseIncrement = 0.0f;
}

static void _wavecap_env_atk_coeff_recompute (t_wavecap* x) {
	x->env_atk_coeff = exp(log(0.01)/(x->env_atk_ms * x->sample_rate * 0.001));
	x->env_last = 0.0f;
}

static void _wavecap_env_dcy_coeff_recompute (t_wavecap* x) {
	x->env_dcy_coeff = exp(log(0.01)/(x->env_dcy_ms * x->sample_rate * 0.001));
	x->env_last = 0.0f;
}

/*
	message receivers
*/

static void wavecap_env_disable (t_wavecap* x) {
	x->env_enabled = 0;
	post("inlet 2 envelope follower disabled");
}

static void wavecap_env_enable (t_wavecap* x) {
	x->env_enabled = 1;
	post("inlet 2 envelope follower enabled");
}

static void wavecap_table_size (t_wavecap* x, t_float f) {
	uint32_t table_size_new = (uint32_t) f;
	
	// make sure it's non-zero and a power of two
	if (table_size_new == 0 || (table_size_new & (table_size_new - 1)) != 0) {
		error("table_size: %d is not a non-zero power of two", table_size_new);
		return;
	}

	// check if it changed
	if (table_size_new != x->table_size) {
		x->table_size = table_size_new;
		x->table_mask = table_size_new - 1;
		_wavecap_table_free(x);
		_wavecap_table_alloc(x);
	}

	post("table_size: %d", x->table_size);
}

static void wavecap_table_interp (t_wavecap* x, t_float f) {
	int i = (int) f;
	if (i < 0 || i >= interp_types_num) {
		error("table_interp: %d invalid, must be in the interval [%d, %d)", i, 0, interp_types_num);
		return;
	}

	x->table_interp = (interp_type) i;
	post("table_interp: %d", x->table_interp);
}

static void wavecap_env_atk_ms (t_wavecap* x, t_float f) {
	x->env_atk_ms = f;
	_wavecap_env_atk_coeff_recompute(x);
	post("env_atk_ms: %f", x->env_atk_ms);
}

static void wavecap_env_dcy_ms (t_wavecap* x, t_float f) {
	x->env_dcy_ms = f;
	_wavecap_env_dcy_coeff_recompute(x);
	post("env_dcy_ms: %f", x->env_dcy_ms);
}

/*
	interpolators
*/

/*
	main dsp callback
*/
static t_int* wavecap_perform (t_int* w) {
	// parse args
	t_wavecap* x = (t_wavecap*) w[1];
    t_float* in_table = (t_float*) w[2];
	t_float* in_env = (t_float*) w[3];
    t_float* out = (t_float*) w[4];
	int n = x->block_size;
	float sample_rate = x->sample_rate;
	//float nyquist_rate = x->nyquist_rate;

	// pull state from struct
	int table_record = x->table_record;
	uint32_t table_size = x->table_size;
	uint32_t table_mask = x->table_mask;
	float* table = x->table;
	interp_type table_interp = x->table_interp;
	int env_enabled = x->env_enabled;
	float env_atk_ms = x->env_atk_ms;
	float env_dcy_ms = x->env_dcy_ms;
	float env_atk_coeff = x->env_atk_coeff;
	float env_dcy_coeff = x->env_dcy_coeff;
	float env_last = x->env_last;
	float phase = x->phase;
	float phaseIncrement = x->phaseIncrement;

	// create state
	int n_computed = 0;
	int table_idx;
	float env_tmp = 0.0f;

	// alias Terbe's variables
	long longPhase;
	float phaseMix;
	uint32_t tableMask = table_mask;
	float* wavetable = table;
	long truncphase;
	float fr;
	float inm1;
	float in;
	float inp1;
	float inp2;
	float freqAngle;

	// record table
	if (table_record > 0) {
		table += table_size - table_record;
		while (table_record > 0 && n_computed < n) {
			*table++ = *in_table++;
			table_record--;

			*out++ = 0.0f;
			n_computed++;
		}
		if (table_record == 0) {
			table = x->table;
			_wavecap_table_reset_phase(x);
			post("done!");
		}
		x->table_record = table_record;
	}

	// follow envelope and generate wave
	in_env += n_computed;
	while (n_computed < n) {
		// envelope follower
		if (env_enabled) {
			env_tmp = fabsf(*in_env++);
			if (env_tmp > env_last) {
				env_last = env_atk_coeff * (env_last - env_tmp) + env_tmp;
			}
			else {
				env_last = env_dcy_coeff * (env_last - env_tmp) + env_tmp;
			}
		}
		else {
			env_last = *in_env++;
		}

		// wavetable oscillator
		phaseIncrement = fabsf(env_last) * table_size;

		// interpolate
		switch (table_interp) {
		case truncate:
			longPhase = (long)phase;
			longPhase = longPhase & tableMask;

			*(out++) = *(wavetable + longPhase);
			break;
		case lin_2:
			longPhase = (long)phase;
			longPhase = longPhase & tableMask;
			phaseMix = phase - (float)longPhase;

			// Xa * (1.0 - pM) + Xb * pM = Xa - Xa*pM + Xb*pM = Xa + pM*(Xb - Xa)
			*(out++) = *(wavetable + longPhase)
			+ (phaseMix * (*(wavetable + ((longPhase + 1)&tableMask)) - *(wavetable + longPhase)));
			break;
		case lin_4:
			truncphase = (long) phase;
			fr = phase - (float) truncphase;
			inm1 = wavetable[(truncphase - 1) & tableMask];
			in = wavetable[(truncphase + 0) & tableMask];
			inp1 = wavetable[(truncphase + 1) & tableMask];
			inp2 = wavetable[(truncphase + 2) & tableMask];

			*(out++) = in + 0.5 * fr * (inp1 - inm1 +
				fr * (4.0 * inp1 + 2.0 * inm1 - 5.0 * in - inp2 +
				fr * (3.0 * (in - inp1) - inm1 + inp2)));
			break;
		default:
			*(out++) = 0.0f;
		}

		n_computed++;
		phase += phaseIncrement;
		while (phase >= (float) table_size)
			phase = phase - (float) table_size;
		while (phase < 0.0f)
			phase = phase + (float) table_size;
	}
	x->env_last = env_last;
	x->phase = phase;
	x->phaseIncrement = phaseIncrement;

    return (w + 5);
}

/*
	pd callback: register dsp
*/
static void wavecap_dsp (t_wavecap* x, t_signal** sp) {
	// store sample rate
	float sr = sp[0]->s_sr;
	if (x->sample_rate != sr) {
		x->sample_rate = sr;
		x->nyquist_rate = sr / 2.0f;

		// recompute state if sample rate changed
		_wavecap_env_atk_coeff_recompute(x);
		_wavecap_env_dcy_coeff_recompute(x);
	}

	// store block size
	x->block_size = sp[0]->s_n;

    dsp_add(wavecap_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec);
}

/*
	pd callback: initialize object
*/
static void* wavecap_new (t_floatarg f) {
    t_wavecap* x = (t_wavecap*) pd_new(wavecap_class);
	x->f = f;
	x->block_size = -1;
	x->sample_rate = 0.0f;
	x->nyquist_rate = 0.0f;

	// set parameter defaults
	x->table_record = 0;
	x->table_size = 1024;
	x->table_mask = 1023;
	x->table_interp = truncate;
	
	x->env_enabled = 0;
	x->env_atk_ms = 10.0f;
	x->env_dcy_ms = 500.0f;

	// compute env follower coefficients
	x->env_atk_coeff = NAN;
	x->env_dcy_coeff = NAN;
	x->env_last = 0.0f;

	// call helpers
	_wavecap_table_alloc(x);
	_wavecap_table_reset_phase(x);

	// create inlets
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, 0);
	outlet_new(&x->x_obj, &s_signal);

    return (void*) x;
}

/*
	pd callback: delete object
*/
static void wavecap_delete (t_wavecap* x) {
	_wavecap_table_free(x);
}

/*
	pd callback: setup object
*/
void wavecap_tilde_setup (void) {
    wavecap_class = class_new(gensym("wavecap~"), (t_newmethod) wavecap_new, (t_method) wavecap_delete, sizeof(t_wavecap), 0, A_NULL, 0);
	
	class_addbang(wavecap_class, (t_method) wavecap_table_record);
	class_addmethod(wavecap_class, (t_method) wavecap_env_enable, gensym("env_enable"), A_NULL, 0);
	class_addmethod(wavecap_class, (t_method) wavecap_env_disable, gensym("env_disable"), A_NULL, 0);
	class_addmethod(wavecap_class, (t_method) wavecap_table_size, gensym("table_size"), A_FLOAT, 0);
    class_addmethod(wavecap_class, (t_method) wavecap_table_interp, gensym("table_interp"), A_FLOAT, 0);
    class_addmethod(wavecap_class, (t_method) wavecap_env_atk_ms, gensym("env_atk_ms"), A_FLOAT, 0);
    class_addmethod(wavecap_class, (t_method) wavecap_env_dcy_ms, gensym("env_dcy_ms"), A_FLOAT, 0);

    CLASS_MAINSIGNALIN(wavecap_class, t_wavecap, f);
    class_addmethod(wavecap_class, (t_method) wavecap_dsp, gensym("dsp"), A_CANT, 0);
}