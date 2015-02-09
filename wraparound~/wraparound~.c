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

#include <math.h>
#include <stdlib.h>

/*
	wraparound~
	Chris Donahue (http://cdonahue.me) 2014

	This external performs amplitude wraparound on an incoming signal. If a signal extends outside [-1.0, 1.0] it will be wrapped around until it is within [-1.0, 1.0]

	Imagine wrapping the waveform into a cylinder and allowing the signal to clip. Clipped signals will come back into the proper amplitude range but produce harsh discontinuities for an otherwise continuous signal. Similar in spirit to wave folding, and inspired by video game wraparound such as in the game Pacman.

	Inlets from left to right:

		1. audio signal to wrap
		2. audio signal gain (float)

	Accepts the following messages:

		1. "soften": Expects numerical parameters n and alpha. Instructs the external to run a smoothing algorithm to smooth out signal discontinuities created by wraparound. N is the size of the buffer to use for smoothing, alpha is the decay for the exponential moving average smoothing algorithm.
		2. "hard": Returns the external to its default state after a soften message
*/

static t_class* wraparound_class;

typedef struct _wraparound {
    t_object x_obj;
	// parameters
	// amplitude gain for input signal
    t_float gain;
	// boolean for turning on hard wraparound (no smoothing)
	t_int hard;
	// buffer size for exponential moving average smoothing
	t_int soften_n;
	// buffer head
	int soften_buffer_idx;
	// buffer
	float* soften_buffer;
	// exponential decay parameters
	t_float soften_alpha;
	// keeps track of frame timer for softening (active if less than soften_n)
	int soften_buffer_active_n;
	// keeps track of if the last sample was wrapped for sample block transitions
	t_int wrapped_last;
} t_wraparound;

/*
	retrieves the nth past frame in the history of the soften buffer
	returns NAN if n is negative or greater than or equal to buffer size
*/
#ifdef _WIN32
static __inline float soften_buffer_retrieve (t_wraparound* x, int n) {
#else
static inline float soften_buffer_retrieve (t_wraparound* x, int n) {
#endif
	int soften_buffer_idx_requested;

	if (n < 0 || n >= x->soften_n) {
		return NAN;
	}

	soften_buffer_idx_requested = x->soften_buffer_idx - 1 - n;
	if (soften_buffer_idx_requested < 0) {
		soften_buffer_idx_requested += x->soften_n;
	}
	return x->soften_buffer[soften_buffer_idx_requested];
}

/*
	pushes a frame onto the soften buffer
	soften_buffer_idx always points to the place where the next frame will go
*/
#ifdef _WIN32
static __inline void soften_buffer_push (t_wraparound* x, float frame) {
#else
static inline void soften_buffer_push (t_wraparound* x, float frame) {
#endif
	x->soften_buffer[x->soften_buffer_idx++] = frame;
	if (x->soften_buffer_idx >= x->soften_n) {
		x->soften_buffer_idx = 0;
	}
}

/*
	calculates the exponential decay moving average for the current frame (slower DEBUG version which calls soften_buffer_retrieve)
*/
#ifdef _WIN32
static __inline float calculate_exponential_moving_average (t_wraparound* x) {
#else
static inline float calculate_exponential_moving_average (t_wraparound* x) {
#endif
	float dividend = 0.0f;
	float divisor = 0.0f;
	int i;
	float i_alpha;

	for (i = 0; i < x->soften_n; i++) {
		i_alpha = pow(x->soften_alpha, i);
		dividend += i_alpha * soften_buffer_retrieve(x, i);
		divisor += i_alpha;
	}
	return dividend/divisor;
}

/*
	calculates the exponential decay moving average for the current frame (faster version which wraps the circular buffer internally)
*/
#ifdef _WIN32
static __inline float calculate_exponential_moving_average_fast (t_wraparound* x) {
#else
static inline float calculate_exponential_moving_average_fast (t_wraparound* x) {
#endif
	float dividend = 0.0f;
	float divisor = 0.0f;
	int i = 0;
	int soften_n = x->soften_n;
	int soften_buffer_idx = x->soften_buffer_idx - 1;
	int soften_buffer_idx_before_add = soften_buffer_idx;
	float i_alpha;

	while (i <= soften_buffer_idx_before_add) {
		i_alpha = pow(x->soften_alpha, i);
		dividend += i_alpha * x->soften_buffer[soften_buffer_idx--];
		divisor += i_alpha;
		i++;
	}
	soften_buffer_idx += soften_n;
	while (i < soften_n) {
		i_alpha = pow(x->soften_alpha, i);
		dividend += i_alpha * x->soften_buffer[soften_buffer_idx--];
		divisor += i_alpha;
		i++;
	}

	return dividend/divisor;
}

/*
	message receiver to set to soften
*/
void wraparound_soften (t_wraparound* x, t_symbol* selector, int argcount, t_atom* argvec) {
	float soften_n;
	float soften_alpha;

	// check arg count
	if (argcount != 2) {
		error("expected 2 arguments for soften (buffer size and alpha), received %d", argcount);
		return;
	}

	// check arg types
	if (argvec[0].a_type != A_FLOAT) {
		error("provided soften buffer size is not a number");
		return;
	}
	if (argvec[1].a_type != A_FLOAT) {
		error("provided soften alpha decay is not a number");
		return;
	}

	// check arg ranges
	soften_n = argvec[0].a_w.w_float;
	if (soften_n < 2.0f) {
		error("soften buffer length must be greater than 2");
		return;
	}
	soften_alpha = argvec[1].a_w.w_float;
	if (soften_alpha < 0.0f) {
		error("soften alpha decay must be greater than 0");
		return;
	}
	
	// assign args
	x->hard = 0;
	x->soften_n = (t_int) soften_n;
	if (x->soften_buffer) {
		free(x->soften_buffer);
	}
	x->soften_buffer_idx = 0;
	x->soften_buffer = (float*) calloc(x->soften_n, sizeof(float));
	x->soften_buffer_active_n = x->soften_n;
	x->soften_alpha = soften_alpha;

	post("soften: n=%d, alpha=%f", x->soften_n, x->soften_alpha);
}

/*
	message receiver to set to hard wrap
*/
void wraparound_hard (t_wraparound* x) {
	x->hard = 1;
	post("hard");
}

/*
	float receiever on second inlet to set gain
*/
void wraparound_gain (t_wraparound* x, t_float f) {
	x->gain = f;
	post("gain: %f", x->gain);
}

/*
	main dsp callback
*/
static t_int* wraparound_perform (t_int* w) {
	// parse args
	t_wraparound* x = (t_wraparound*) w[1];
    t_float* in = (t_float*) w[2];
    t_float* out = (t_float*) w[3];
    int n = (int) w[4];

	// pull state from struct
	float gain = x->gain;
	int wrapped_last = x->wrapped_last;
	int hard = x->hard;
	int soften_n = x->soften_n;
	float soften_alpha = x->soften_alpha;

	// create state
	int wrapped_current = 0;
	int frame_current_idx = 0;
	float frame_current;
	float frame_current_wrapped;

	if (hard) {
		while (n--) {
			frame_current = *(in + frame_current_idx) * gain;
			
			// calculate wrapped frame
			wrapped_current = 0;
			while (frame_current < -1.0f) {
				frame_current += 2.0f;
				wrapped_current = 1;
			}
			while (frame_current > 1.0f) {
				frame_current -= 2.0f;
				wrapped_current = 1;
			}

			*(out + frame_current_idx++) = frame_current;
		}
		// ideally we would memcpy into the soften buffer here for maximum accuracy at transition time but we don't have a size for that yet
	}
	else {
		while (n--) {
			frame_current = *(in + frame_current_idx) * gain;
			
			// calculate wrapped frame
			wrapped_current = 0;
			frame_current_wrapped = frame_current;
			while (frame_current_wrapped < -1.0f) {
				frame_current_wrapped += 2.0f;
				wrapped_current = 1;
			}
			while (frame_current_wrapped > 1.0f) {
				frame_current_wrapped -= 2.0f;
				wrapped_current = 1;
			}

			// push hard wrapped frame onto soften buffer
			soften_buffer_push(x, frame_current_wrapped);

			// if we're switching from unwrapped to wrapped or vice versa activate softening
			if (wrapped_current ^ wrapped_last) {
				x->soften_buffer_active_n = 0;
			}

			// if we're actively softening then continue to do so
			if (x->soften_buffer_active_n < soften_n) {
				frame_current = calculate_exponential_moving_average_fast(x);
				x->soften_buffer_active_n++;
			}
			else {
				frame_current = frame_current_wrapped;
			}

			*(out + frame_current_idx++) = frame_current;
			wrapped_last = wrapped_current;
		}
	}

	x->wrapped_last = wrapped_current;

    return (w + 5);
}

/*
	pd callback: register dsp
*/
static void wraparound_dsp (t_wraparound* x, t_signal** sp) {
    dsp_add(wraparound_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

/*
	pd callback: initialize object
*/
static void* wraparound_new (t_floatarg f) {
    t_wraparound* x = (t_wraparound*) pd_new(wraparound_class);
	x->gain = f;
	x->hard = 1;
	x->soften_n = 0;
	x->soften_buffer_idx = 0;
	x->soften_buffer = NULL;
	x->soften_alpha = 0.0f;
	x->soften_buffer_active_n = 0;
	x->wrapped_last = 0;

    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("gain"));
	outlet_new(&x->x_obj, gensym("signal"));

    return (void*) x;
}

/*
	pd callback: delete object
*/
static void wraparound_delete (t_wraparound* x) {
	if (x->soften_buffer) {
		free(x->soften_buffer);
	}
}

/*
	pd callback: setup object
*/
void wraparound_tilde_setup (void) {
    wraparound_class = class_new(gensym("wraparound~"), (t_newmethod) wraparound_new, (t_method) wraparound_delete, sizeof(t_wraparound), 0, A_DEFFLOAT, 0);

    class_addmethod(wraparound_class, (t_method) wraparound_soften, gensym("soften"), A_GIMME, 0);
    class_addmethod(wraparound_class, (t_method) wraparound_hard, gensym("hard"), 0);
    class_addmethod(wraparound_class, (t_method) wraparound_gain, gensym("gain"), A_FLOAT, 0);
    CLASS_MAINSIGNALIN(wraparound_class, t_wraparound, gain);
    class_addmethod(wraparound_class, (t_method) wraparound_dsp, gensym("dsp"), A_CANT, 0);
}