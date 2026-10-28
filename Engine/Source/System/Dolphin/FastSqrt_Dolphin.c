// Square roots for the GameCube (and Wii): the Gekko has no square root instruction, and newlib's sqrtf / sqrt are
// software (__ieee754_sqrtf works it out a bit at a time, hundreds of cycles). Everything that measures a length or
// normalizes a vector paid for it -- the dynamic bones, CPU skinning and morph targets, Bullet, Lua's math.sqrt. These
// replace them in the link (libEngine comes before libm): the hardware's reciprocal square root estimate (frsqrte),
// refined by Newton's method to full double precision, then times x and one last correction on the root itself --
// within a bit of the last place of the exact root (OctSqrtSelfTest measures it against newlib's at the log's start).

#include <stdint.h>

static inline double OctRsqrtEstimate(double x)
{
    double y;
    __asm__("frsqrte %0,%1" : "=f"(y) : "f"(x));
    return y;
}

static inline double OctSqrtD(double x)
{
    if (!(x > 0.0))
        return (x == 0.0 || x != x) ? x : __builtin_nan("");   // (0 and -0 as they are, NaN on, negative NaN)
    if (x == __builtin_inf())
        return x;
    const double h = 0.5 * x;
    double y = OctRsqrtEstimate(x);                              // (to 1/32 at worst: 5 bits)
    y = y * (1.5 - h * y * y);                                   // (10)
    y = y * (1.5 - h * y * y);                                   // (20)
    y = y * (1.5 - h * y * y);                                   // (40)
    y = y * (1.5 - h * y * y);                                   // (past a double's 53)
    double r = x * y;
    return r + 0.5 * (x - r * r) * y;                            // (the root itself corrected once)
}

double sqrt(double x)
{
    return OctSqrtD(x);
}

float sqrtf(float x)
{
    return (float)OctSqrtD((double)x);
}

// The worst relative error against newlib's own (exact) roots over a spread of values, float and double, for the log
double __ieee754_sqrt(double);
float __ieee754_sqrtf(float);

void OctSqrtSelfTest(double* worstD, double* worstF)
{
    double wd = 0.0, wf = 0.0;
    double x = 1.0e-30;
    for (int i = 0; i < 4000; ++i)
    {
        x *= 1.0371;                                              // (1e-30 to about 1e33, irregular steps)
        const double e = __ieee754_sqrt(x), g = OctSqrtD(x);
        const double rd = (g > e ? g - e : e - g) / e;
        if (rd > wd) wd = rd;
        const float xf = (float)x;
        if (xf > 0.0f && xf < 3.0e38f)
        {
            const float ef = __ieee754_sqrtf(xf), gf = sqrtf(xf);
            const double rf = (double)(gf > ef ? gf - ef : ef - gf) / (double)ef;
            if (rf > wf) wf = rf;
        }
    }
    *worstD = wd;
    *worstF = wf;
}
