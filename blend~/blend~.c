#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#include "m_pd.h"
#include <math.h>

/*	
	blend~
	Chris Donahue (http://cdonahue.me) 2014

	This external uses a control signal to blend two signals together. If the control signal is at or above 1.0, the output signal will be signal 1. If the control signal is at or below -1.0, the output will be signal 2. If the control signal is in between, the signal 1 and signal 2 will be blended together linearly.

	Inlets from left to right are:

		1. blend control signal
		2. audio signal 1
		3. audio signal 2

	Detail for linear blend:

		ctrl[x] = ctrl signal at sample x
		sig1[x] = audio signal 1 at sample x
		sig2[x] = audio signal 2 at sample x
		out[x] = output signal at sample x
		a[x] = coefficient for signal 1 at sample x (control signal distance from -1.0)
		b[x] = coefficient for signal 2 at sample x (control signal distance from 1.0)

		a[x] = (ctrl[x] - (-1.0)) = (ctrl[x] + 1.0)
		b[x] = |(ctrl[x] - (1.0))| = |(ctrl[x] - 1.0)|
		out[x] = [(a[x] * sig1[x]) + (b[x] * sig2[x])] / 2.0
			   = [(ctrl[x] + 1.0) * sig1[x]] + [|(ctrl[x] - 1.0)| * sig2[x]] / 2.0

*/

/*
	blend~
	Chris Donahue (http://cdonahue.me) 2014

	This external uses a control signal to blend two signals together. If the control signal is at or above 1.0, the 

	This is a wave folder with a twist! Instead of folding over a simple linear threshold, this external folds an audio signal over two signals representing the lower and upper threshold. Can be used as a traditional wave folder by folding over linear threshold signals. Inlets from left to right are:

		1. signal to fold
		2. lower threshold of folding
		3. upper threshold of folding
*/

static t_class* blend_class;

typedef struct _blend {
    t_object x_obj;
    t_float gain_ctrl;
} t_blend;

/*
	main dsp callback
*/
static t_int* blend_perform (t_int* w) {
	// pull state from args
	float gain_ctrl = ((t_blend*) w[1])->gain_ctrl;
    t_float* in_ctrl = (t_float*) w[2];
    t_float* in_sig1 = (t_float*) w[3];
    t_float* in_sig2 = (t_float*) w[4];
    t_float* out = (t_float*) w[5];
    int n = (int) w[6];

	// create state
	int sample_current_idx = 0;
	short slope_positive;
	float a;
	float b;
	float sig1;
	float sig2;
	float ctrl;

	while (n--) {
		// control signal
		ctrl = *(in_ctrl + sample_current_idx) * gain_ctrl;

		// hard clip control signal
		if (ctrl > 1.0f) {
			ctrl = 1.0f;
		}
		else if (ctrl < -1.0f) {
			ctrl = -1.0f;
		}

		// calculate a/b
		// if the signal inverted from last sample
		a = (ctrl + 1.0f);
		b = fabs(ctrl - 1.0f);

		// audio signal
		sig1 = *(in_sig1 + sample_current_idx);
		sig2 = *(in_sig2 + sample_current_idx);

		*(out + sample_current_idx++) = ((a * sig1) + (b * sig2)) / 2.0f;
	}

    return (w + 7);
}

/*
	pd callback: register dsp
*/
static void blend_dsp (t_blend* x, t_signal** sp) {
    dsp_add(blend_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec, sp[0]->s_n);
}

/*
	pd callback: initialize object
*/
static void* blend_new (void) {
    t_blend* x = (t_blend*) pd_new(blend_class);
	x->gain_ctrl = 1.0;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	outlet_new(&x->x_obj, gensym("signal"));
    return (x);
}

/*
	pd callback: setup object
*/
void blend_tilde_setup(void) {
    blend_class = class_new(gensym("blend~"), (t_newmethod) blend_new, 0, sizeof(t_blend), 0, A_GIMME, 0);
	
    CLASS_MAINSIGNALIN(blend_class, t_blend, gain_ctrl);
    class_addmethod(blend_class, (t_method) blend_dsp, gensym("dsp"), A_CANT, 0);
}