/*
 * asio_list.cpp -- Audio Hook companion tool (command-line UI).
 *
 *   Lists the ASIO drivers registered on the machine and lets you test-play a
 *   440 Hz tone through any of them, so you can hear which physical output the
 *   driver actually drives.
 *
 * Usage:
 *   asio_list                          interactive menu (Up/Down, Enter, Q)
 *   asio_list --list                   plain text list, no menu
 *   asio_list --test "Focusrite USB"   play the 3 s tone on that driver
 *   asio_list --test "*"               play on the first registered driver
 *   asio_list --help
 *
 * Exit codes: 0 ok, 1 no drivers found, 2 test failed, 3 bad command line,
 *             4 driver crashed during playback.
 *
 * Notes on the fixes in this build:
 *   - a name that matches nothing is an ERROR now; it used to fall back to the
 *     first driver, so you could "verify" the wrong device
 *   - every ASIO session is properly closed (stop -> disposeBuffers -> Release);
 *     the old build skipped disposeBuffers(), so the previous driver's buffers
 *     (and sometimes the device) stayed claimed while the menu tested the next one
 *   - the tone's phase used to be advanced once per CHANNEL, so left and right
 *     were one block apart and the fade-in ran twice as fast
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <objbase.h>
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifndef interface
#define interface struct
#endif
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

#define TONE_HZ     440.0
#define TONE_AMP    0.12      /* gentle: about -18 dBFS */
#define FADE_SEC    0.12      /* 120 ms fade-in            */

struct Drv {
    char key[256];
    char clsid[256];
    char desc[256];
};

/* ---- global ASIO playback state (used by the bufferSwitch callback) ---- */
static IASIO*          g_asio = NULL;
static ASIOCallbacks   g_cbs;
static ASIOBufferInfo  g_bufs[2];
static ASIOChannelInfo g_ci[2];
static long            g_block = 0;
static double          g_rate = 44100.0;
static double          g_phase = 0.0;
static double          g_env = 0.0;      /* fade-in envelope 0..1 */
static double          g_envStep = 0.0;
static volatile LONG   g_playing = 0;
static int             g_postOut = 0;    /* driver accepts outputReady() */
static volatile LONG   g_busy = 0;       /* inside cbBufferSwitch */

/* bytes per sample for the formats this tool can render; 0 = unsupported */
static size_t FrameBytes(long type)
{
    switch (type) {
    case ASIOSTInt16LSB: return 2;
    case ASIOSTInt24LSB: return 3;
    case ASIOSTInt32LSB: case ASIOSTInt32LSB16: case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
    case ASIOSTFloat32LSB: return 4;
    case ASIOSTFloat64LSB: return 8;
    default: return 0;
    }
}

static void WriteTone(void* p, long type, long block, double ph0)
{
    const double PI2 = 6.283185307179586;
    double w = PI2 * TONE_HZ / g_rate;
    double a = TONE_AMP * g_env;
    switch (type) {
    case ASIOSTInt16LSB: {
        short* o = (short*)p;
        for (long i = 0; i < block; i++) o[i] = (short)(sin(ph0 + w * i) * a * 32767.0);
    } break;
    case ASIOSTInt32LSB: case ASIOSTInt32LSB16: case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20: case ASIOSTInt32LSB24: {
        int32_t* o = (int32_t*)p;
        for (long i = 0; i < block; i++) o[i] = (int32_t)(sin(ph0 + w * i) * a * 2147483647.0);
    } break;
    case ASIOSTFloat32LSB: {
        float* o = (float*)p;
        for (long i = 0; i < block; i++) o[i] = (float)(sin(ph0 + w * i) * a);
    } break;
    case ASIOSTFloat64LSB: {
        double* o = (double*)p;
        for (long i = 0; i < block; i++) o[i] = sin(ph0 + w * i) * a;
    } break;
    case ASIOSTInt24LSB: {
        unsigned char* o = (unsigned char*)p;
        for (long i = 0; i < block; i++) {
            int32_t v = (int32_t)(sin(ph0 + w * i) * a * 8388607.0);
            o[i * 3] = (unsigned char)v; o[i * 3 + 1] = (unsigned char)(v >> 8); o[i * 3 + 2] = (unsigned char)(v >> 16);
        }
    } break;
    default:
        break;   /* never reached: the type is validated before start() */
    }
}

static void cbBufferSwitch(long index, ASIOBool direct)
{
    (void)direct;
    InterlockedIncrement(&g_busy);
    if (g_playing)
    {
        /* Same starting phase for both channels, advanced ONCE per buffer. */
        double ph0 = g_phase;
        for (int c = 0; c < 2; c++) {
            void* b = g_bufs[c].buffers[index];
            if (b) WriteTone(b, g_ci[c].type, g_block, ph0);
        }
        const double PI2 = 6.283185307179586;
        g_phase = fmod(ph0 + PI2 * TONE_HZ / g_rate * g_block, PI2);
        if (g_env < 1.0) { g_env += g_envStep; if (g_env > 1.0) g_env = 1.0; }
    }
    else
    {
        for (int c = 0; c < 2; c++) {
            void* b = g_bufs[c].buffers[index];
            size_t fb = FrameBytes(g_ci[c].type);
            if (b && fb) memset(b, 0, fb * (size_t)g_block);
        }
    }
    if (g_postOut && g_asio) g_asio->outputReady();
    InterlockedDecrement(&g_busy);
}

static long cbAsioMessage(long selector, long value, void* msg, double* opt)
{
    (void)msg; (void)opt;
    switch (selector) {
    case kAsioSelectorSupported:
        return (value == kAsioEngineVersion) ? 1L : 0L;
    case kAsioEngineVersion:
        return 2L;
    default:
        return 0L;
    }
}

static int ContainsNoCase(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return 1;
    return 0;
}

/* Returns the number of usable entries (entries without a CLSID are skipped). */
static int EnumDrivers(Drv* drvs, int max)
{
    int n = 0, skipped = 0;
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;

    DWORD idx = 0;
    for (;;) {
        char keyname[256] = { 0 };
        DWORD klen = (DWORD)sizeof(keyname) - 1;
        if (RegEnumKeyExA(hk, idx++, keyname, &klen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (klen >= sizeof(keyname) - 1) continue;   /* truncated name: unusable */

        HKEY hs;
        if (RegOpenKeyExA(hk, keyname, 0, KEY_READ, &hs) != ERROR_SUCCESS) continue;

        if (n >= max) { RegCloseKey(hs); skipped++; continue; }

        Drv* d = &drvs[n];
        memset(d, 0, sizeof(*d));
        strncpy_s(d->key, sizeof(d->key), keyname, _TRUNCATE);

        DWORD ty = 0, sz = (DWORD)sizeof(d->clsid) - 1;
        LONG r1 = RegQueryValueExA(hs, "clsid", NULL, &ty, (LPBYTE)d->clsid, &sz);
        if (r1 != ERROR_SUCCESS || ty != REG_SZ) d->clsid[0] = 0;
        d->clsid[sizeof(d->clsid) - 1] = 0;

        ty = 0; sz = (DWORD)sizeof(d->desc) - 1;
        LONG r2 = RegQueryValueExA(hs, "description", NULL, &ty, (LPBYTE)d->desc, &sz);
        if (r2 != ERROR_SUCCESS || ty != REG_SZ) d->desc[0] = 0;
        d->desc[sizeof(d->desc) - 1] = 0;

        RegCloseKey(hs);

        if (!d->clsid[0]) { skipped++; continue; }   /* broken registration */
        n++;
    }
    RegCloseKey(hk);
    if (skipped) printf("(%d registry entr%s skipped: no CLSID / list full)\n",
                        skipped, skipped == 1 ? "y" : "ies");
    return n;
}

static const char* DrvName(const Drv* d)
{
    return d->desc[0] ? d->desc : d->key;
}

/* Play a 440 Hz tone for `seconds` (interruptible by any key).
 * Returns 0 = ok, 1 = failed (see g_lastErr), 2 = driver crashed. */
static char g_lastErr[192] = "";

static int TestTone(const char* clsid, int seconds)
{
    g_lastErr[0] = 0;
    if (!clsid || !clsid[0]) { strcpy_s(g_lastErr, "no CLSID"); return 1; }

    wchar_t wc[256];
    MultiByteToWideChar(CP_ACP, 0, clsid, -1, wc, 256);
    CLSID id;
    if (FAILED(CLSIDFromString(wc, &id))) { strcpy_s(g_lastErr, "bad CLSID string"); return 1; }

    IASIO* asio = NULL;
    int buffers = 0, started = 0;
    int r = 1;

    __try {
        if (FAILED(CoCreateInstance(id, NULL, CLSCTX_INPROC_SERVER, id, (void**)&asio)) || !asio)
        { strcpy_s(g_lastErr, "CoCreateInstance failed"); __leave; }

        if (asio->init((void*)GetDesktopWindow()) != ASIOTrue)
        { strcpy_s(g_lastErr, "init() returned false"); __leave; }

        long nin = 0, nout = 0;
        if (asio->getChannels(&nin, &nout) != ASE_OK || nout < 2)
        { sprintf_s(g_lastErr, "needs 2 outputs, driver reports %ld", nout); __leave; }

        long mn = 0, mx = 0, pr = 0, gr = 0;
        if (asio->getBufferSize(&mn, &mx, &pr, &gr) != ASE_OK || mx <= 0)
        { strcpy_s(g_lastErr, "getBufferSize failed"); __leave; }

        long block = (pr > 0) ? pr : ((mn > 0) ? mn : 512);
        if (gr > 0)      block = mn + ((block - mn + gr - 1) / gr) * gr;
        else if (gr < 0) { long b = (mn > 0 ? mn : 16); for (int k = 0; b < block && b < mx && k < 32; k++) b <<= 1; block = b; }
        if (block < mn) block = mn;
        if (block > mx) block = mx;
        if (block <= 0) block = 512;
        g_block = block;

        double sr = 0;
        if (asio->getSampleRate(&sr) == ASE_OK && sr > 0) g_rate = sr;

        g_postOut = (asio->outputReady() == ASE_OK) ? 1 : 0;

        memset(&g_bufs, 0, sizeof(g_bufs));
        for (int c = 0; c < 2; c++) { g_bufs[c].isInput = ASIOFalse; g_bufs[c].channelNum = c; }
        memset(&g_cbs, 0, sizeof(g_cbs));
        g_cbs.bufferSwitch = cbBufferSwitch;
        g_cbs.asioMessage = cbAsioMessage;

        if (asio->createBuffers(g_bufs, 2, block, &g_cbs) != ASE_OK)
        { strcpy_s(g_lastErr, "createBuffers failed"); __leave; }
        buffers = 1;

        for (int c = 0; c < 2; c++) {
            memset(&g_ci[c], 0, sizeof(g_ci[c]));
            g_ci[c].channel = c; g_ci[c].isInput = ASIOFalse;
            if (asio->getChannelInfo(&g_ci[c]) != ASE_OK)
            { sprintf_s(g_lastErr, "getChannelInfo(ch%d) failed", c); __leave; }
            if (!FrameBytes(g_ci[c].type))
            { sprintf_s(g_lastErr, "unsupported sample type %ld on ch%d", g_ci[c].type, c); __leave; }
        }

        /* pre-fill both buffer halves with silence before start */
        for (long h = 0; h < 2; h++)
            for (int c = 0; c < 2; c++)
                if (g_bufs[c].buffers[h])
                    memset(g_bufs[c].buffers[h], 0, FrameBytes(g_ci[c].type) * (size_t)block);

        g_asio = asio;
        g_phase = 0.0;
        g_env = 0.0;
        {
            double blocks = FADE_SEC * g_rate / (double)block;
            g_envStep = (blocks > 1.0) ? (1.0 / blocks) : 1.0;
        }

        InterlockedExchange(&g_playing, 1);
        if (asio->start() != ASE_OK)
        {
            InterlockedExchange(&g_playing, 0);
            strcpy_s(g_lastErr, "start failed");
            __leave;
        }
        started = 1;

        for (int i = 0; i < seconds * 10; i++) {
            Sleep(100);
            if (_kbhit()) { _getch(); break; }   /* any key stops early */
        }
        r = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(g_lastErr, "driver raised an access violation");
        r = 2;
        InterlockedExchange(&g_playing, 0);
    }

    /* Always close the driver: stop the stream, free the buffers, drop the COM
       reference (there is no IASIO::exit() - ASIOExit() only exists for hosts
       that link the driver DLL directly). Skipping disposeBuffers() left the
       previous driver's memory (and sometimes the device) claimed.

       stop() does NOT guarantee that bufferSwitch has returned (driver
       implementations differ), and a callback writing into buffers we just
       freed is a use-after-free on the driver's thread - outside our __try.
       Wait for the in-flight counter to reach 0 first. */
    InterlockedExchange(&g_playing, 0);
    if (asio)
    {
        __try {
            if (started) asio->stop();
            if (buffers)
            {
                for (int i = 0; i < 200 && g_busy; i++) Sleep(1);
                if (!g_busy) asio->disposeBuffers();
                else
                {
                    if (r == 0) { r = 2; strcpy_s(g_lastErr, "callback still running - buffers leaked"); }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            if (r == 0) { r = 2; strcpy_s(g_lastErr, "driver faulted while shutting down"); }
        }
    }
    g_asio = NULL;
    if (asio) asio->Release();
    return r;
}

static void EnableVT(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static void Draw(const Drv* drvs, int n, int sel, const char* status)
{
    printf("\x1b[H");   /* cursor home (we never clear, just redraw) */
    printf("=== ASIO Driver Test  (%d drivers) ===\n", n);
    printf("  Up/Down: select   Enter: play 3 s tone   Q: quit\n\n");
    for (int i = 0; i < n; i++) {
        if (i == sel)
            printf("\x1b[7m  [%2d] %-44s\x1b[0m\n", i + 1, DrvName(&drvs[i]));
        else
            printf("  [%2d] %-44s\n", i + 1, DrvName(&drvs[i]));
    }
    /* \x1b[K wipes the rest of the line, otherwise a shorter status leaves
       fragments of the previous one behind */
    printf("\n  %s\x1b[K\n", status ? status : "");
}

static void RunTui(Drv* drvs, int n)
{
    int sel = 0;
    char status[256] = "Press Enter to test the selected driver.";
    EnableVT();
    printf("\x1b[2J");   /* clear once */
    Draw(drvs, n, sel, status);
    for (;;) {
        int ch = _getch();
        if (ch == 0xE0 || ch == 0) {
            int k = _getch();
            if (k == 0x48 && sel > 0) sel--;
            else if (k == 0x50 && sel < n - 1) sel++;
        }
        else if (ch == '\r' || ch == '\n') {
            const char* nm = DrvName(&drvs[sel]);
            snprintf(status, sizeof(status), "Playing 440 Hz on '%s' ... (any key stops)", nm);
            Draw(drvs, n, sel, status);
            int r = TestTone(drvs[sel].clsid, 3);
            if (r == 0)      snprintf(status, sizeof(status), "OK - heard the tone? then this is the driver. (Q to quit)");
            else if (r == 2) snprintf(status, sizeof(status), "driver crashed: %s", g_lastErr);
            else             snprintf(status, sizeof(status), "failed: %s", g_lastErr);
        }
        else if (ch == 0x1B || ch == 'q' || ch == 'Q') {
            break;
        }
        Draw(drvs, n, sel, status);
    }
    printf("\x1b[0m\x1b[2J");   /* reset + clear */
}

static void Usage(void)
{
    printf("asio_list [--list | --test \"driver\" | --help]\n"
           "  --test NAME   case-insensitive substring of the driver name; \"*\" = first\n");
}

int main(int argc, char** argv)
{
    int list_mode = 0, test_mode = 0;
    const char* test_drv = NULL;

    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--list")) { list_mode = 1; }
        else if (!_stricmp(argv[i], "--test")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "!! --test needs a driver name (use \"*\" for the first one)\n");
                Usage();
                return 3;
            }
            test_mode = 1;
            test_drv = argv[++i];
        }
        else if (!_stricmp(argv[i], "--help") || !_stricmp(argv[i], "-h")) { Usage(); return 0; }
        else {
            fprintf(stderr, "!! unknown argument '%s'\n", argv[i]);
            Usage();
            return 3;
        }
    }

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
    {
        fprintf(stderr, "!! CoInitializeEx failed\n");
        return 1;
    }

    Drv drvs[64];
    int n = EnumDrivers(drvs, 64);
    if (n == 0) {
        printf("No ASIO drivers found (HKLM\\SOFTWARE\\ASIO).\n");
        CoUninitialize();
        return 1;
    }

    int rc = 0;
    if (test_mode) {
        int pick = -1;
        if (!test_drv[0] || test_drv[0] == '*') pick = 0;
        else {
            for (int i = 0; i < n; i++)
                if (ContainsNoCase(DrvName(&drvs[i]), test_drv)) { pick = i; break; }
        }
        if (pick < 0) {
            /* Never silently test a different driver: that is how you end up
               "confirming" the wrong device. */
            fprintf(stderr, "!! no ASIO driver matches '%s'. Registered drivers:\n", test_drv);
            for (int i = 0; i < n; i++) fprintf(stderr, "   [%d] %s\n", i + 1, DrvName(&drvs[i]));
            CoUninitialize();
            return 2;
        }
        printf("Testing 440 Hz tone (3 s) on: %s\n", DrvName(&drvs[pick]));
        fflush(stdout);
        int r = TestTone(drvs[pick].clsid, 3);
        if (r == 0)      printf("done.\n");
        else if (r == 2) { printf("driver crashed: %s\n", g_lastErr); rc = 4; }
        else             { printf("failed: %s\n", g_lastErr); rc = 2; }
    }
    else if (list_mode) {
        for (int i = 0; i < n; i++) printf("[%d] %s\n", i + 1, DrvName(&drvs[i]));
    }
    else {
        RunTui(drvs, n);
    }

    CoUninitialize();
    return rc;
}
