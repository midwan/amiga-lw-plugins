#ifndef TRIG_APPROX_H
#define TRIG_APPROX_H

/*
 * Self-contained sine/cosine for plugins: no libm calls.
 *
 * The libnix libm differs per toolchain and CPU variant. GCC 6.5's libm881
 * called mathieeedoubtrans.library and returned the result in fp0 (an 040fpu
 * Modeler plugin calling it hung Modeler); GCC 16's libm881 uses fsin/fcos,
 * which the 68040/68060 do not implement and must trap to 68040.library or
 * 68060.library. These versions use only + and *, so every build runs the
 * same code.
 *
 * Taylor series after range reduction to [-pi/2, pi/2];
 * sin(x) = x*(1 + x2*(c1 + x2*(...))), cos(x) = 1 + x2*(c1 + x2*(...)).
 * Max error below 1e-9 on that range. Range reduction subtracts whole turns
 * counted in a long long, so the result degrades only as the double input
 * loses precision; beyond 2^52 turns, where a double holds no fraction of a
 * turn, it returns the value at 0.
 */

#ifdef __GNUC__
#define TRIG_UNUSED __attribute__((unused))
#else
#define TRIG_UNUSED
#endif

static double
TRIG_UNUSED
reduce_angle(double x)
{
	double two_pi = 6.283185307179586;
	double pi = 3.141592653589793;
	double turns = x / two_pi;
	long long whole;

	if (turns > 4503599627370496.0 || turns < -4503599627370496.0)
		return 0.0;
	whole = (long long)turns;
	x = x - (double)whole * two_pi;
	if (x > pi)
		x = x - two_pi;
	else if (x < -pi)
		x = x + two_pi;
	return x;
}

static double
TRIG_UNUSED
sin_approx(double angle)
{
	static const double c[8] = {
		1.0,
		-0.16666666666666666,
		0.008333333333333333,
		-0.0001984126984126984,
		2.7557319223985893e-06,
		-2.505210838544172e-08,
		1.6059043836821613e-10,
		-7.647163731819816e-13
	};
	double x = reduce_angle(angle);
	double x2;
	double acc;
	int i;

	if (x > 1.5707963267948966)
		x = 3.141592653589793 - x;
	else if (x < -1.5707963267948966)
		x = -3.141592653589793 - x;
	x2 = x * x;
	acc = c[7];
	for (i = 6; i >= 0; i--)
		acc = acc * x2 + c[i];
	return x * acc;
}

static double
TRIG_UNUSED
cos_approx(double angle)
{
	static const double c[9] = {
		1.0,
		-0.5,
		0.041666666666666664,
		-0.001388888888888889,
		2.48015873015873e-05,
		-2.755731922398589e-07,
		2.08767569878681e-09,
		-1.1470745597729725e-11,
		4.779477332387385e-14
	};
	double x = reduce_angle(angle);
	double x2;
	double acc;
	int negate = 0;
	int i;

	if (x > 1.5707963267948966) {
		x = 3.141592653589793 - x;
		negate = 1;
	} else if (x < -1.5707963267948966) {
		x = -3.141592653589793 - x;
		negate = 1;
	}
	x2 = x * x;
	acc = c[8];
	for (i = 7; i >= 0; i--)
		acc = acc * x2 + c[i];
	return negate ? -acc : acc;
}

#endif
