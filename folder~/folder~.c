#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#ifdef _WIN32
    #ifndef NAN
        static const unsigned long __nan[2] = {0xffffffff, 0x7fffffff};
        #define NAN (*(const float *) __nan)
    #endif
#endif

#include "m_pd.h"

/*
	folder~
	Chris Donahue (http://cdonahue.me) 2014

	This external is a wave folder with a twist! Instead of folding over a simple linear threshold, this external folds an audio signal over two signals representing the lower and upper threshold. Can be used as a traditional wave folder by folding over linear threshold signals.

	Inlets from left to right are:

		1. audio signal to fold
		2. lower threshold of folding
		3. upper threshold of folding
*/

static t_class* folder_class;

typedef struct _folder {
    t_object x_obj;
	// parameters
	// amplitude gain for input signal
    t_float gain;
} t_folder;

/*
	float receiever on second inlet to set gain
*/
void folder_gain (t_folder* x, t_float f) {
	x->gain = f;
	post("gain: %f", x->gain);
}

/*
	main dsp callback
*/
static t_int* folder_perform (t_int* w) {
	// parse args
	t_folder* x = (t_folder*) w[1];
    t_float* in_sig = (t_float*) w[2];
    t_float* in_lower_thresh = (t_float*) w[3];
	t_float* in_upper_thresh = (t_float*) w[4];
    t_float* out = (t_float*) w[5];
    int n = (int) w[6];

	// pull state from struct
	float gain = x->gain;

	// create state
	int folded_current;
	int frame_current_idx = 0;
	float frame_current_sig;
	float frame_current_lower_thresh;
	float below_lower_thresh;
	float frame_current_upper_thresh;
	float above_upper_thresh;
	float frame_current_folded;

	while (n--) {
		frame_current_lower_thresh = *(in_lower_thresh + frame_current_idx) * gain;
		frame_current_upper_thresh = *(in_upper_thresh + frame_current_idx) * gain;
		frame_current_sig = *(in_sig + frame_current_idx) * gain;
		frame_current_folded = frame_current_sig;
		folded_current = 0;
		
		// fold over lower
		below_lower_thresh = frame_current_lower_thresh - frame_current_sig;
		if (below_lower_thresh > 0.0f) {
			frame_current_folded = frame_current_lower_thresh + below_lower_thresh;
			folded_current = 1;
		}

		// fold over upper
		above_upper_thresh = frame_current_sig - frame_current_upper_thresh;
		if (above_upper_thresh > 0.0f) {
			frame_current_folded = frame_current_upper_thresh - above_upper_thresh;
			folded_current = 1;
		}

		*(out + frame_current_idx++) = frame_current_folded;
	}

    return (w + 7);
}

/*
	pd callback: register dsp
*/
static void folder_dsp (t_folder* x, t_signal** sp) {
    dsp_add(folder_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec, sp[0]->s_n);
}

/*
	pd callback: initialize object
*/
static void* folder_new (t_floatarg f) {
    t_folder* x = (t_folder*) pd_new(folder_class);
	x->gain = f;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	outlet_new(&x->x_obj, gensym("signal"));

    return (void*) x;
}

/*
	pd callback: setup object
*/
void folder_tilde_setup (void) {
    folder_class = class_new(gensym("folder~"), (t_newmethod) folder_new, 0, sizeof(t_folder), 0, A_DEFFLOAT, 0);

    class_addmethod(folder_class, (t_method) folder_gain, gensym("gain"), A_FLOAT, 0);
    CLASS_MAINSIGNALIN(folder_class, t_folder, gain);
    class_addmethod(folder_class, (t_method) folder_dsp, gensym("dsp"), A_CANT, 0);
}