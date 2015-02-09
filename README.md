pd_externals
============

Repository for various PD externals I've authored. Feel free to do whatever you want with any of these. Some of them can be created internally within pd~ (with slightly less efficiency) and were made as class projects. See the source C file for a better description of each.

* blend~: Blends two signals according to the value of a control signal
* folder~: Performs wave folding on an input signal using the values of two other signals as the folding thresholds
* wavecap~: Captures a time window signal and uses it as a wavetable
* wiener~: Calculates the [Wiener entropy](http://en.wikipedia.org/wiki/Spectral_flatness) (spectral flatness) of an input signal. Probably the most realistically useful external out of the bunch. Requires [KissFFT](http://kissfft.sourceforge.net/) to compile
* wraparound~: Wraps a signal around a torus when it is outside the range of -1.0 and 1.0