/* test_rs.cpp -- offline checks for the WASAPI shared backend's resampler.
 *
 * Test 1 (math): a triangle source with unlimited supply verifies the
 * interpolation itself - produced frame count and per-sample error.
 *
 * Test 2 (gate): reproduces what WoThread does with a *finite* pool (CRI never
 * hands us more than FAKE_FRAMES = 441 source frames at a time) and asserts that
 * no output frame is padded with silence. This is the case that was missed when
 * the resampler was added: `n = min(free, RingAvail())` mixed OUTPUT frames
 * (device rate) with SOURCE frames (44.1 kHz), so on a 32 kHz endpoint the
 * resampler ran dry and ~27 % of every period was silence (music at 72.6 % speed).
 *
 * Build: cl -nologo -utf-8 -MT -O2 -W4 test_rs.cpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static double g_rsStep = 1.0;
static double g_rsFrac = 0.0;
static short  g_rsCur[2] = { 0, 0 };
static short  g_rsNext[2] = { 0, 0 };
static int    g_rsPrimed = 0, g_rsHasNext = 0;
static short  g_rsBuf[1024 * 2];
static unsigned g_rsN = 0, g_rsI = 0;

/* mock source: a pool of `g_srcLen` frames, read sequentially from g_srcPos */
static long g_srcPos = 0, g_srcLen = 0;

static int SrcSample(long i)
{
    long ph = i % 65536;                  /* continuous triangle wave */
    return (int)(ph < 32768 ? ph : 65535 - ph);
}

static unsigned MockRead(short* dst, unsigned want)
{
    unsigned n = 0;
    while (n < want && g_srcPos < g_srcLen)
    {
        int v = SrcSample(g_srcPos);
        dst[n * 2] = (short)v;
        dst[n * 2 + 1] = (short)v;
        g_srcPos++; n++;
    }
    return n;
}

static unsigned MockAvail(void)
{
    long pool = g_srcLen - g_srcPos;
    if (pool < 0) pool = 0;
    return (unsigned)pool + (g_rsN - g_rsI);
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

/* ---- test 1: interpolation math (unlimited supply) ---- */
static int TestMath(const char* label, double step, long srcFrames)
{
    RsReset();
    g_rsStep = step;
    g_srcPos = 0; g_srcLen = srcFrames;

    short out[2048 * 2];
    long produced = 0;
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
        }
    }
    double expectCount = (double)srcFrames / step;
    int ok = (fabs((double)produced - expectCount) <= 2.0 && maxErr <= 1.5);
    printf("%-26s step=%.6f produced=%ld (exp %.0f) maxErr=%.2f  %s\n",
           label, step, produced, expectCount, maxErr, ok ? "ok" : "FAIL");
    return ok;
}

/* ---- test 2: WoThread gate with a finite pool ----
 * CRI never gives us more than FAKE_FRAMES source frames at once. The writer
 * asks for min(freeOutputFrames, availSourceFrames / step) output frames; every
 * one of them must be real audio, not padding. */
#define POOL 441
static int TestGate(const char* label, double step)
{
    RsReset();
    g_rsStep = step;
    g_srcPos = 0;
    g_srcLen = POOL;                  /* CRI's staging buffer is full */

    short out[2048 * 2];
    unsigned long long wanted = 0, produced = 0;
    const unsigned freeOut = 970;     /* a typical 22 ms engine buffer @44.1k */

    for (int iter = 0; iter < 4000; iter++)
    {
        unsigned avail = MockAvail();
        /* the fix: source frames must be converted to producible output frames */
        unsigned gate = (step > 1.0) ? (unsigned)((double)avail / step) : avail;
        unsigned n = freeOut < gate ? freeOut : gate;
        if (!n) break;

        unsigned got = RsPull(out, n);
        wanted += n;
        produced += got;

        /* CRI keeps its staging buffer topped up at 44.1 kHz */
        long buffered = (long)(g_rsN - g_rsI);
        g_srcLen = g_srcPos + (POOL > buffered ? POOL - buffered : 0);
    }

    double pct = wanted ? 100.0 * (double)produced / (double)wanted : 0.0;
    int ok = (pct > 99.0);
    printf("%-26s step=%.6f music/total = %5.1f%%  %s\n",
           label, step, pct, ok ? "ok" : "FAIL (padding)");
    return ok;
}

int main(void)
{
    int fail = 0;
    printf("-- interpolation math ------------------------------------------\n");
    fail |= !TestMath("44.1k -> 48k", 44100.0 / 48000.0, 44100);
    fail |= !TestMath("44.1k -> 32k", 44100.0 / 32000.0, 44100);
    fail |= !TestMath("44.1k -> 96k", 44100.0 / 96000.0, 44100);

    printf("-- WoThread gate (441-frame CRI pool) --------------------------\n");
    fail |= !TestGate("44.1k (no resample)", 1.0);
    fail |= !TestGate("44.1k -> 48k", 44100.0 / 48000.0);
    fail |= !TestGate("44.1k -> 96k", 44100.0 / 96000.0);
    fail |= !TestGate("44.1k -> 32k", 44100.0 / 32000.0);
    fail |= !TestGate("44.1k -> 22.05k", 44100.0 / 22050.0);

    printf(fail ? "\nFAILED\n" : "\nALL OK\n");
    return fail;
}
