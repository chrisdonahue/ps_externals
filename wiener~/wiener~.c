#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#define _USE_MATH_DEFINES
#include <math.h>

#include "m_pd.h"

#include "kiss_fft130/kiss_fftr.h"

/*	
	wiener~
	Chris Donahue (http://cdonahue.me) 2014

	This external computes the Spectral flatness or "Wiener entropy" of an incoming audio signal. Spectral flatness is exactly what its name implies: a measure of the flatness of the power spectrum of a signal. It is essentially a measure of the noisiness of a signal, where white noise approaches 1.0 and a pure sine wave tone approaches 0.0. The spectral flatness of an audio signal is computed by calculating the ratio of the geometric mean of the power spectrum to the arithmetic mean.

	The wiener~ external computes spectral flatness using a limited amount of parameters. It accepts the following messages:
		* window_type rectangle	(no windowing of the input signal before FFT)
		* window_type hann		(hann window applied to the input signal before FFT)
		* power_spectrum		(use power spectrum (FFT bin magnitude squared) for entropy computation)
		* amplitude_spectrum	(use amplitude spectrum (FFT bin magnitude) for entropy computation)

	Additional details:
		* FFT size is PD's current block size.
		* Small epsilon value is added to each bin power to ensure no divide by zero craziness and sane output (1.0) for incoming silence
		* KissFFT is used for FFT calculation

	Resources used:
		* http://en.wikipedia.org/wiki/Spectral_flatness
		* http://dsp.stackexchange.com/questions/2045/how-do-you-calculate-spectral-flatness-from-an-fft
		* http://www.audiocontentanalysis.org/code/audio-features/spectral-flatness/
		* http://soundanalysispro.com/download
		* http://kissfft.sourceforge.net/
*/

#define epsilon 1e-20

static t_class* wiener_class;

typedef enum {
	rectangle,
	hann
} fftr_window_type;

typedef struct _wiener {
    t_object x_obj;
    t_float x_f;
	t_outlet* outlet;

	// dsp settings
	int block_size;

	// wiener params
	int wiener_power_spectrum;

	// fft params
	fftr_window_type fftr_input_window_type;

	// fft state
	kiss_fftr_cfg fftr_cfg;
	int fftr_output_size;
	kiss_fft_cpx* fftr_output;
	float* fftr_input_window;
} t_wiener;

/*
	internal state helpers
*/

static void _wiener_fftr_alloc (t_wiener* x) {
	int nfft = x->block_size;
	int fftr_output_size = (nfft / 2) - 1;
	x->fftr_cfg = kiss_fftr_alloc(nfft, 0, 0, 0);
	x->fftr_output_size = fftr_output_size;
	x->fftr_output = (kiss_fft_cpx*) malloc(sizeof(kiss_fft_cpx) * fftr_output_size);
}

static void _wiener_fftr_free (t_wiener* x) {
	if (x->fftr_cfg) {
		free(x->fftr_cfg);
		x->fftr_cfg = NULL;
	}
	if (x->fftr_output) {
		free(x->fftr_output);
		x->fftr_output = NULL;
	}
}

static int _wiener_fftr_input_window_needs_buffer (t_wiener* x) {
	return x->fftr_input_window_type >= 1;
}

static void _wiener_fftr_input_window_alloc (t_wiener* x) {
	int n;
	int block_size = x->block_size;
	float* fftr_input_window;
	double window_value;

	// state for hann window functions
	double cos_inner_value;
	double cos_inner_increment;

	if (block_size > 0 && _wiener_fftr_input_window_needs_buffer(x)) {
		fftr_input_window = (float*) malloc(sizeof(float) * block_size);
		x->fftr_input_window = fftr_input_window;
		
		if (x->fftr_input_window_type == hann) {
			cos_inner_value = 0.0;
			cos_inner_increment = (2.0 * M_PI) / ((double) (block_size - 1));
			for (n = 0; n < block_size; n++) {
				window_value = 0.5 * (1.0 - cos(cos_inner_value));
				cos_inner_value += cos_inner_increment;
				*fftr_input_window++ = (float) window_value;
			}
		}
	}
}

static void _wiener_fftr_input_window_free (t_wiener* x) {
	if (x->fftr_input_window) {
		free(x->fftr_input_window);
		x->fftr_input_window = NULL;
	}
}

static void _wiener_fftr_input_apply_window (t_wiener* x, float* fftr_input) {
	if (_wiener_fftr_input_window_needs_buffer(x)) {
		float* fftr_input_window = x->fftr_input_window;
		int n = x->block_size;

		while (n--) {
			*fftr_input = (*fftr_input_window++) * (*fftr_input++);
		}
	}
}

/*
	message receivers
*/

static void wiener_window_type (t_wiener* x, t_symbol* selector, int argc, t_atom* argv) {
	const char* arg_0;
	fftr_window_type old = x->fftr_input_window_type;

	if (argc != 1) {
		error("window_type: expected 1 argument (rectangle, hann, etc.), received %d", argc);
		return;
	}

	if (argv[0].a_type != A_SYMBOL) {
		error("window_type: supplied argument was not a string");
		return;
	}

	arg_0 = argv[0].a_w.w_symbol->s_name;

	if (strcmp(arg_0, "rectangle") == 0) {
		x->fftr_input_window_type = rectangle;
	}
	else if (strcmp(arg_0, "hann") == 0) {
		x->fftr_input_window_type = hann;
	}
	else {
		error("window_type: supplied argument %s invalid", arg_0);
		return;
	}

	if (x->fftr_input_window_type != old) {
		_wiener_fftr_input_window_free(x);
		_wiener_fftr_input_window_alloc(x);
	}

	post("window_type: %s", arg_0);
}

static void wiener_amplitude_spectrum (t_wiener* x) {
	x->wiener_power_spectrum = 0;

	post("using amplitude spectrum for Wiener entropy calculation");
}

static void wiener_power_spectrum (t_wiener* x) {
	x->wiener_power_spectrum = 1;

	post("using power spectrum for Wiener entropy calculation");
}

/*
	main dsp callback
*/
static t_int* wiener_perform (t_int* w) {
	// pull state from args
	t_wiener* x = (t_wiener*) w[1];
    float* fftr_input = (float*) w[2];
    int n = (int) w[4];

	// pull state from struct
	t_outlet* outlet = x->outlet;
	int block_size = x->block_size;
	int wiener_power_spectrum = x->wiener_power_spectrum;
	kiss_fftr_cfg fftr_cfg = x->fftr_cfg;
	int fftr_output_size = x->fftr_output_size;
	double fftr_output_size_d = (double) fftr_output_size;
	kiss_fft_cpx* fftr_output = x->fftr_output;
	
	// create state
	int i;
	double fftr_bin_amplitude;
	double fftr_bin_power;
	double fftr_bins_amplitude_sum = 0.0;
	double fftr_bins_power_sum = 0.0;
	double fftr_bins_amplitude_sum_ln = 0.0;
	double fftr_bins_power_sum_ln = 0.0;
	double wiener_numerator;
	double wiener_denominator;
	float wiener_entropy;

	// apply window
	_wiener_fftr_input_apply_window(x, fftr_input);
	
	// compute fft
	kiss_fftr(fftr_cfg, fftr_input, fftr_output);

	// compute wiener entropy
	for (i = 0; i < fftr_output_size; i++) {
		kiss_fft_cpx bin = fftr_output[i];

		// calculate bin magnitude
		fftr_bin_power = bin.r * bin.r + bin.i * bin.i + epsilon;

		// sum power
		if (wiener_power_spectrum) {
			fftr_bins_power_sum += fftr_bin_power;
			fftr_bins_power_sum_ln += log(fftr_bin_power);
		}
		// sum amplitude
		else {
			fftr_bin_amplitude = sqrt(fftr_bin_power);

			fftr_bins_amplitude_sum += fftr_bin_amplitude;
			fftr_bins_amplitude_sum_ln += log(fftr_bin_amplitude);
		}
	}

	// calculate wiener entropy
	if (wiener_power_spectrum) {
		wiener_numerator = exp(fftr_bins_power_sum_ln / fftr_output_size_d);
		wiener_denominator = fftr_bins_power_sum / fftr_output_size_d;
	}
	else {
		wiener_numerator = exp(fftr_bins_amplitude_sum_ln / fftr_output_size_d);
		wiener_denominator = fftr_bins_amplitude_sum / fftr_output_size_d;
	}
	wiener_entropy = (float) (wiener_numerator / wiener_denominator);

	// output
	outlet_float(outlet, wiener_entropy);

    return (w + 4);
}

/*
	pd callback: register dsp
*/
static void wiener_dsp (t_wiener* x, t_signal** sp) {
	int block_size = sp[0]->s_n;
	if (x->block_size != block_size) {
		x->block_size = block_size;

		_wiener_fftr_free(x);
		_wiener_fftr_alloc(x);

		_wiener_fftr_input_window_free(x);
		_wiener_fftr_input_window_alloc(x);
	}

    dsp_add(wiener_perform, 3, x, sp[0]->s_vec, sp[0]->s_n);
}

/*
	pd callback: initialize object
*/
static void* wiener_new (void) {
    t_wiener* x = (t_wiener*) pd_new(wiener_class);
	x->x_f = 0.0f;

	x->block_size = -1;
	
	x->wiener_power_spectrum = 0;

	x->fftr_input_window_type = hann;

	x->fftr_cfg = NULL;
	x->fftr_output_size = -1;
	x->fftr_output = NULL;
	x->fftr_input_window = NULL;

    //inlet_new(&x->x_obj, &x->x_obj.ob_pd, 0, 0);
	x->outlet = outlet_new(&x->x_obj, &s_float);

    return (void*) x;
}

/*
	pd callback: delete object
*/
static void wiener_delete (t_wiener* x) {
	_wiener_fftr_free(x);
	_wiener_fftr_input_window_free(x);
}

/*
	pd callback: setup object
*/
void wiener_tilde_setup (void) {
    wiener_class = class_new(gensym("wiener~"), (t_newmethod) wiener_new, (t_method) wiener_delete, sizeof(t_wiener), 0, A_NULL, 0);

	class_addmethod(wiener_class, (t_method) wiener_window_type, gensym("window_type"), A_GIMME, 0);
	class_addmethod(wiener_class, (t_method) wiener_amplitude_spectrum, gensym("amplitude_spectrum"), A_NULL, 0);
	class_addmethod(wiener_class, (t_method) wiener_power_spectrum, gensym("power_spectrum"), A_NULL, 0);
	
    CLASS_MAINSIGNALIN(wiener_class, t_wiener, x_f);
    class_addmethod(wiener_class, (t_method) wiener_dsp, gensym("dsp"), A_CANT, 0);
}