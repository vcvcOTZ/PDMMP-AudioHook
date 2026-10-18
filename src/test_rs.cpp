/* test_rs.cpp -- standalone check of the linear resampler used by the WASAPI
 * shared backend. Same algorithm as audio_hook.cpp's RsPull(); the ring is
 * replaced by a finite ramp so the output can be compared analytically:
 *   for a ramp src[i] = i, the interpolated output must satisfy
 *   out[k] ~= k * step, and the produced frame count must be ~= n / step.
 */
#include <stdio.h>
#include <math.h>

static double g_rsStep = 1.0;
static double g_rsFrac = 0.0;
static short  g_rsCur[2] = { 0, 0 };
static short  g_rsNext[2] = { 0, 0 };
static int    g_rsPrimed = 0, g_rsHasNext = 0;
static short  g_rsBuf[1024 * 2];
static unsigned g_rsN = 0, g_rsI = 0;

/* mock ring: ramp 0,1,2,... */
static long g_srcPos = 0, g_srcLen = 0;
static unsigned MockRead(short* dst, unsigned want)
{
    unsigned n = 0;
    while (n < want && g_srcPos < g_srcLen)
    {
        /* continuous triangle wave, period 65536: linear interpolation is
           exact on it, and there is no wrap discontinuity to confuse the check */
        long ph = g_srcPos % 65536;
        int v = (int)(ph < 32768 ? ph : 65535 - ph);
        dst[n * 2] = (short)v;
        dst[n * 2 + 1] = (short)v;
        g_srcPos++; n++;
    }
    return n;
}

static void RsReset(void)
{
    g_rsStep = 1.0; g_rsFrac = 0.0; g_rsPrimed = 0; g_rsHasNext = 0;
    g_rsN = g_rsI = 0;
    g_rsCur[0] = g_rsCur[1] = g_rsNext[0] = g_rsNext[1] = 0;
}
static int RsNext(short* out)
{
    if (g_rsI >= g_rsN)
    {
        g_rsN = MockRead(g_rsBuf, 1024);
        g_rsI = 0;
        if (!g_rsN) return 0;
    }
    out[0] = g_rsBuf[g_rsI * 2];
    out[1] = g_rsBuf[g_rsI * 2 + 1];
    g_rsI++;
    return 1;
}
static unsigned RsPull(short* dst, unsigned n)
{
    if (g_rsStep == 1.0) return MockRead(dst, n);
    unsigned produced = 0;
    while (produced < n)
    {
        if (!g_rsPrimed)
        {
            if (!RsNext(g_rsCur)) break;
            g_rsPrimed = 1; g_rsFrac = 0.0;
        }
        if (!g_rsHasNext)
        {
            if (!RsNext(g_rsNext)) break;
            g_rsHasNext = 1;
        }
        double f = g_rsFrac;
        dst[produced * 2]     = (short)(g_rsCur[0] + (g_rsNext[0] - g_rsCur[0]) * f);
        dst[produced * 2 + 1] = (short)(g_rsCur[1] + (g_rsNext[1] - g_rsCur[1]) * f);
        produced++;
        g_rsFrac += g_rsStep;
        while (g_rsFrac >= 1.0)
        {
            g_rsCur[0] = g_rsNext[0]; g_rsCur[1] = g_rsNext[1];
            g_rsFrac -= 1.0;
            if (!RsNext(g_rsNext)) { g_rsHasNext = 0; break; }
        }
    }
    return produced;
}

static int Test(const char* label, double step, long srcFrames)
{
    RsReset();
    g_rsStep = step;
    g_srcPos = 0; g_srcLen = srcFrames;

    short out[2048 * 2];
    long produced = 0, firstBad = -1;
    double maxErr = 0.0;
    for (;;)
    {
        unsigned got = RsPull(out, 970);
        if (!got) break;
        for (unsigned i = 0; i < got; i++, produced++)
        {
            double x = fmod((double)produced * step, 65536.0);
            double expect = (x < 32768.0) ? x : 65535.0 - x;
            double err = fabs((double)out[i * 2] - expect);
            if (err > maxErr) maxErr = err;
            if (err > 1.5 && firstBad < 0) firstBad = produced;
        }
    }
    double expectCount = (double)srcFrames / step;
    printf("%-22s step=%.6f src=%ld -> produced=%ld (expected %.0f, delta %.0f) maxErr=%.2f firstBad=%ld\n",
           label, step, srcFrames, produced, expectCount, produced - expectCount, maxErr, firstBad);
    return (fabs(produced - expectCount) <= 2.0 && maxErr <= 1.5) ? 0 : 1;
}

int main(void)
{
    int fail = 0;
    fail |= Test("44.1k -> 48k", 44100.0 / 48000.0, 44100);
    fail |= Test("44.1k -> 32k", 44100.0 / 32000.0, 44100);
    fail |= Test("44.1k -> 96k", 44100.0 / 96000.0, 44100);
    printf(fail ? "FAILED\n" : "ALL OK\n");
    return fail;
}
