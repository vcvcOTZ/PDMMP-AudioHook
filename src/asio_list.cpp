/*
 * asio_list.cpp -- Audio Hook companion tool (command-line UI).
 *
 *   Lists the ASIO drivers registered on the machine and lets you test-play a
 *   440 Hz tone through any of them, so you can hear which physical output the
 *   driver actually drives.
 *
 * Usage:
 *   asio_list                 interactive menu (Up/Down to select, Enter to test)
 *   asio_list --list          plain text list only (no menu)
 *   asio_list --test "Focusrite USB"   play the 3 s tone directly
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

/* bytes per sample frame for a given ASIO sample type. */
static size_t FrameBytes(long type)
{
    switch (type) {
    case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
    case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
    case ASIOSTFloat64LSB: case ASIOSTFloat64MSB: return 8;
    default: return 4;   /* int32 / float32 / int32LSB16-24 */
    }
}

static void WriteTone(void* p, long type, long block)
{
    const double PI2 = 6.283185307179586;
    double w = PI2 * 440.0 / g_rate;
    double a = 0.12 * g_env;   /* softer level, with a ~120 ms fade-in */
    double ph0 = g_phase;
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
        memset(p, 0, FrameBytes(type) * (size_t)block);
        break;
    }
    g_phase = ph0 + w * block;
    if (g_phase > PI2) g_phase = fmod(g_phase, PI2);
    if (g_env < 1.0) { g_env += g_envStep; if (g_env > 1.0) g_env = 1.0; }
}

static void cbBufferSwitch(long index, ASIOBool direct)
{
    (void)direct;
    for (int c = 0; c < 2; c++) {
        void* b = g_bufs[c].buffers[index];
        if (!b) continue;   /* single-buffer or malformed driver */
        if (g_playing) WriteTone(b, g_ci[c].type, g_block);
        else memset(b, 0, FrameBytes(g_ci[c].type) * (size_t)g_block);
    }
    if (g_postOut && g_asio) g_asio->outputReady();
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

static int EnumDrivers(Drv* drvs, int max)
{
    int n = 0;
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    DWORD idx = 0;
    for (;;) {
        char keyname[256] = { 0 }; DWORD klen = sizeof(keyname) - 1;
        if (RegEnumKeyExA(hk, idx++, keyname, &klen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (n >= max) break;
        HKEY hs;
        if (RegOpenKeyExA(hk, keyname, 0, KEY_READ, &hs) != ERROR_SUCCESS) continue;
        Drv* d = &drvs[n];
        strncpy(d->key, keyname, 255); d->key[255] = 0;
        d->clsid[0] = 0; d->desc[0] = 0;
        DWORD ty = 0, sz = sizeof(d->clsid) - 1;
        RegQueryValueExA(hs, "clsid", NULL, &ty, (LPBYTE)d->clsid, &sz);
        sz = sizeof(d->desc) - 1;
        RegQueryValueExA(hs, "description", NULL, &ty, (LPBYTE)d->desc, &sz);
        RegCloseKey(hs);
        n++;
    }
    RegCloseKey(hk);
    return n;
}

static const char* DrvName(const Drv* d)
{
    return d->desc[0] ? d->desc : d->key;
}

/* Play a 440 Hz tone for `seconds` (interruptible by any key).
 * Returns 0 = ok, 1 = failed, 2 = driver crashed. */
static int TestTone(const char* clsid, int seconds)
{
    if (!clsid || !clsid[0]) return 1;
    wchar_t wc[256];
    MultiByteToWideChar(CP_ACP, 0, clsid, -1, wc, 256);
    CLSID id;
    if (FAILED(CLSIDFromString(wc, &id))) return 1;

    IASIO* asio = NULL;
    int r = 1;
    __try {
        if (FAILED(CoCreateInstance(id, NULL, CLSCTX_INPROC_SERVER, id, (void**)&asio)) || !asio) goto done;
        if (asio->init((void*)GetDesktopWindow()) != ASIOTrue) goto done;
        long nin = 0, nout = 0;
        asio->getChannels(&nin, &nout);
        if (nout < 2) goto done;
        long mn = 0, mx = 0, pr = 0, gr = 0;
        asio->getBufferSize(&mn, &mx, &pr, &gr);
        /* clamp to a legal block size (respect min/max and granularity). */
        g_block = pr > 0 ? pr : (mn > 0 ? mn : 512);
        if (gr > 0) g_block = mn + ((g_block - mn + gr - 1) / gr) * gr;
        else if (gr < 0) { long b = mn; while (b < g_block && b < mx) b <<= 1; g_block = b; }
        if (g_block < mn) g_block = mn;
        if (g_block > mx) g_block = mx;
        double sr = 0; asio->getSampleRate(&sr); if (sr > 0) g_rate = sr;

        g_postOut = (asio->outputReady() == ASE_OK) ? 1 : 0;

        memset(&g_bufs, 0, sizeof(g_bufs));
        for (int c = 0; c < 2; c++) { g_bufs[c].isInput = ASIOFalse; g_bufs[c].channelNum = c; }
        memset(&g_cbs, 0, sizeof(g_cbs));
        g_cbs.bufferSwitch = cbBufferSwitch;
        g_cbs.asioMessage = cbAsioMessage;
        if (asio->createBuffers(g_bufs, 2, g_block, &g_cbs) != ASE_OK) goto done;
        for (int c = 0; c < 2; c++) {
            memset(&g_ci[c], 0, sizeof(g_ci[c]));
            g_ci[c].channel = c; g_ci[c].isInput = ASIOFalse;
            asio->getChannelInfo(&g_ci[c]);
        }
        for (long h = 0; h < 2; h++)
            for (int c = 0; c < 2; c++)
                memset(g_bufs[c].buffers[h], 0, FrameBytes(g_ci[c].type) * (size_t)g_block);

        g_asio = asio;
        g_phase = 0.0;
        g_env = 0.0;
        {
            double blocks = 0.12 * g_rate / g_block;   /* ~120 ms fade-in */
            g_envStep = blocks > 1.0 ? 1.0 / blocks : 1.0;
        }
        InterlockedExchange(&g_playing, 1);
        if (asio->start() != ASE_OK) { InterlockedExchange(&g_playing, 0); goto done; }

        for (int i = 0; i < seconds * 10; i++) {
            Sleep(100);
            if (_kbhit()) { _getch(); break; }   /* any key stops early */
        }
        asio->stop();
        InterlockedExchange(&g_playing, 0);
        r = 0;
    done:
        g_asio = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 2;
    }
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
    printf("\n  %s\n", status ? status : "");
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
            if (r == 0) snprintf(status, sizeof(status), "OK - heard the tone? then this is the driver. (Q to quit)");
            else if (r == 2) snprintf(status, sizeof(status), "driver crashed during playback");
            else snprintf(status, sizeof(status), "failed to play on '%s'", nm);
        }
        else if (ch == 0x1B || ch == 'q' || ch == 'Q') {
            break;
        }
        Draw(drvs, n, sel, status);
    }
    printf("\x1b[0m\x1b[2J");   /* reset + clear */
}

int main(int argc, char** argv)
{
    int list_mode = 0, test_mode = 0;
    const char* test_drv = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) list_mode = 1;
        else if (strcmp(argv[i], "--test") == 0) { test_mode = 1; if (i + 1 < argc) test_drv = argv[++i]; }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("asio_list [--list | --test \"driver\"]\n");
            return 0;
        }
        else test_drv = argv[i];
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    Drv drvs[64];
    int n = EnumDrivers(drvs, 64);
    if (n == 0) { printf("No ASIO drivers found (HKLM\\SOFTWARE\\ASIO).\n"); CoUninitialize(); return 1; }

    if (test_mode) {
        int pick = -1;
        if (test_drv && test_drv[0] && test_drv[0] != '*') {
            for (int i = 0; i < n; i++)
                if (strstr(DrvName(&drvs[i]), test_drv)) { pick = i; break; }
        }
        if (pick < 0) pick = 0;   /* "*" or no match -> first driver */
        printf("Testing 440 Hz tone (3 s) on: %s\n", DrvName(&drvs[pick]));
        fflush(stdout);
        int r = TestTone(drvs[pick].clsid, 3);
        printf("%s\n", r == 0 ? "done." : (r == 2 ? "driver crashed." : "failed."));
    }
    else if (list_mode) {
        for (int i = 0; i < n; i++) printf("[%d] %s\n", i + 1, DrvName(&drvs[i]));
    }
    else {
        RunTui(drvs, n);
    }

    CoUninitialize();
    return 0;
}
