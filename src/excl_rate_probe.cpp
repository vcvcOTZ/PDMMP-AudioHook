/*
 * excl_rate_probe.cpp -- how fast does a WASAPI stream really run on a device?
 *
 * Key lesson: Initialize() succeeding means nothing. A driver can hand you a
 * 5 ms buffer and then never advance the hardware clock (a USB interface: ~88%
 * of real time; some Realtek configs: 0%). So this tool tries every plausible
 * exclusive format and only reports success if the clock really moves.
 *
 * Usage:
 *   excl_rate_probe --list
 *   excl_rate_probe                          (default endpoint, 2 s per format)
 *   excl_rate_probe "<device substring>" [--seconds N] [--shared]
 *
 * Exit codes: 0 measured >= 95 % of real time, 3 measured below that (or no
 *             format worked), 1 device/COM error, 2 bad command line.
 *
 * Fixes in this build:
 *   - --list dereferenced a NULL collection when EnumAudioEndpoints failed
 *   - --shared printed the EXCLUSIVE verdict ("no working exclusive format")
 *     even after a successful shared measurement
 *   - the 0x88890019 "retry" re-sent the identical parameters (dead code)
 *   - a device without IAudioClock was reported as 0 % even when it delivered
 *     frames perfectly; the written rate is now used as a fallback
 *   - device names were converted with CP_ACP while the console was set to
 *     UTF-8, so every non-ASCII name printed as "?????"
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

static const GUID kSubPCM =
{ 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kSubFloat =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static double Now(void)
{
    static LARGE_INTEGER f = { 0 };
    LARGE_INTEGER t;
    if (f.QuadPart <= 0)
    {
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) f.QuadPart = 1;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static void WideToUtf8(LPCWSTR src, char* out, size_t cch)
{
    if (!out || !cch) return;
    out[0] = 0;
    if (src) WideCharToMultiByte(CP_UTF8, 0, src, -1, out, (int)cch - 1, NULL, NULL);
}

static void DevName(IMMDevice* d, char* out, size_t cch)
{
    if (!out || !cch) return;
    out[0] = 0;
    IPropertyStore* ps = NULL;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || !ps) return;
    PROPVARIANT pv; PropVariantInit(&pv);
    HRESULT hr = ps->GetValue(PKEY_Device_FriendlyName, &pv);
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR) WideToUtf8(pv.pwszVal, out, cch);
    if (SUCCEEDED(hr)) PropVariantClear(&pv);
    ps->Release();
}

static int ContainsNoCase(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return 1;
    return 0;
}

struct RateResult
{
    int    ok;          /* a measurement was taken */
    double written;     /* frames pushed into the buffer, per second */
    double clock;       /* device clock advance in NOMINAL frames/s (0 = n/a) */
    double pct;         /* percentage of real time */
    int    usedClock;   /* pct came from the device clock, not from writes */
    int    clockPresent;/* the driver exposed IAudioClock at all */
    int    clockStalled;/* the clock exists but did not advance */
    UINT64 clockFreq;   /* IAudioClock units per second (0 = no clock object) */
    char   err[96];
};

/* Fill silence for `sec` seconds and measure what really happens. */
static void RunOne(IAudioClient* cl, WAVEFORMATEX* wf, UINT32 bufFrames,
                   double sec, RateResult* res)
{
    memset(res, 0, sizeof(*res));

    if (!bufFrames) { strcpy_s(res->err, "no buffer size"); return; }

    HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!ev) { strcpy_s(res->err, "CreateEvent failed"); return; }

    HRESULT hr = cl->SetEventHandle(ev);
    if (FAILED(hr))
    {
        sprintf_s(res->err, "SetEventHandle 0x%08X", (unsigned)hr);
        CloseHandle(ev);
        return;
    }

    IAudioRenderClient* rc = NULL;
    hr = cl->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
    if (FAILED(hr) || !rc)
    {
        sprintf_s(res->err, "GetService(RenderClient) 0x%08X", (unsigned)hr);
        CloseHandle(ev);
        return;
    }

    IAudioClock* ck = NULL;
    cl->GetService(__uuidof(IAudioClock), (void**)&ck);   /* optional */

    UINT32 bpf = wf->nBlockAlign ? wf->nBlockAlign : 4;

    /* the pre-filled buffer is delivered too - it used to be left out of the
       frame count, which under-reported the rate by one buffer per run */
    unsigned long long frames = 0;
    BYTE* p0 = NULL;
    if (SUCCEEDED(rc->GetBuffer(bufFrames, &p0)) && p0)
    {
        memset(p0, 0, (size_t)bufFrames * bpf);
        rc->ReleaseBuffer(bufFrames, 0);
        frames += bufFrames;
    }

    hr = cl->Start();
    if (FAILED(hr))
    {
        sprintf_s(res->err, "Start 0x%08X", (unsigned)hr);
        if (ck) ck->Release();
        rc->Release();
        CloseHandle(ev);
        return;
    }

    UINT64 c0 = 0, ckFreq = 0;
    int clockPresent = 0;
    if (ck)
    {
        clockPresent = 1;
        ck->GetPosition(&c0, NULL);
        ck->GetFrequency(&ckFreq);
    }

    double t0 = Now();
    for (;;)
    {
        if (Now() - t0 >= sec) break;
        WaitForSingleObject(ev, 2);
        UINT32 pad = 0;
        if (FAILED(cl->GetCurrentPadding(&pad))) { Sleep(1); continue; }
        UINT32 n = bufFrames > pad ? bufFrames - pad : 0;
        if (!n) continue;
        BYTE* p = NULL;
        if (FAILED(rc->GetBuffer(n, &p)) || !p) continue;
        memset(p, 0, (size_t)n * bpf);
        rc->ReleaseBuffer(n, 0);
        frames += n;
    }
    double el = Now() - t0;
    if (el <= 0.0) el = 1e-6;

    UINT64 c1 = 0;
    if (ck) ck->GetPosition(&c1, NULL);
    cl->Stop();

    res->ok      = 1;
    res->written = (double)frames / el;
    res->clockFreq  = ckFreq;
    res->clockPresent = clockPresent;

    /* IAudioClock counts in ITS OWN units, and GetFrequency() is the only way
       to know how many of those units are one second. Assuming "units ==
       samples at the nominal rate" is wrong: a shared-mode stream here reports
       8x nominal, which made a perfectly healthy device look like 784 %. */
    double nominal = (double)wf->nSamplesPerSec;
    double clockUnits = (ck && c1 > c0) ? (double)(c1 - c0) / el : 0.0;
    if (clockPresent && ckFreq > 0 && clockUnits > 0.0)
    {
        res->pct = 100.0 * clockUnits / (double)ckFreq;             /* real-time %  */
        res->clock = clockUnits * nominal / (double)ckFreq;         /* in frames    */
        res->usedClock = 1;
    }
    else if (clockPresent)
    {
        /* The object exists but the position never moved: that IS the failure
           mode we are looking for, so score it 0 % - do not fall back to the
           written rate, which would make a dead device look healthy. */
        res->pct = 0.0;
        res->clock = 0.0;
        res->usedClock = 1;
        res->clockStalled = 1;
    }
    else
    {
        res->pct = 100.0 * res->written / nominal;                  /* no clock obj */
        res->clock = 0.0;
        res->usedClock = 0;
    }

    if (ck) ck->Release();
    rc->Release();
    CloseHandle(ev);
}

static int ListDevices(IMMDeviceEnumerator* en)
{
    IMMDeviceCollection* col = NULL;
    HRESULT hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr) || !col)
    {
        printf("!! EnumAudioEndpoints failed 0x%08X\n", (unsigned)hr);
        return 1;
    }

    /* mark the default endpoint: it is the one the game would use */
    char defName[256] = { 0 };
    IMMDevice* def = NULL;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def)
    {
        DevName(def, defName, sizeof(defName));
        def->Release();
    }

    UINT32 n = 0;
    if (FAILED(col->GetCount(&n))) n = 0;
    for (UINT32 i = 0; i < n; i++)
    {
        IMMDevice* d = NULL;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        char nm[256] = { 0 };
        DevName(d, nm, sizeof(nm));
        printf("  [%u] %s%s\n", i, nm, (defName[0] && !strcmp(nm, defName)) ? "   (default)" : "");
        d->Release();
    }
    col->Release();
    return 0;
}

static void Usage(void)
{
    printf("excl_rate_probe [--list | \"<device substring>\" [--seconds N] [--shared]]\n"
           "  --seconds N   1..60 (default 2) seconds measured per format\n"
           "  --shared      measure a normal shared-mode stream instead of exclusive\n");
}

int main(int argc, char** argv)
{
    int shared = 0, list_mode = 0, seconds = 2;
    const char* want = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "--list")) list_mode = 1;
        else if (!_stricmp(argv[i], "--shared")) shared = 1;
        else if (!_stricmp(argv[i], "--help") || !_stricmp(argv[i], "-h")) { Usage(); return 0; }
        else if (!_stricmp(argv[i], "--seconds"))
        {
            if (i + 1 >= argc) { fprintf(stderr, "!! --seconds needs a value\n"); Usage(); return 2; }
            char* end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (!end || *end) { fprintf(stderr, "!! --seconds '%s' is not a number\n", argv[i]); Usage(); return 2; }
            seconds = (int)v;
        }
        else if (argv[i][0] == '-') { fprintf(stderr, "!! unknown option '%s'\n", argv[i]); Usage(); return 2; }
        else if (!want) want = argv[i];
        else { fprintf(stderr, "!! only one device substring is allowed\n"); Usage(); return 2; }
    }
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;

    SetConsoleOutputCP(CP_UTF8);
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
    {
        printf("!! CoInitializeEx failed\n");
        return 1;
    }

    IMMDeviceEnumerator* en = NULL;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
    {
        printf("!! enumerator failed\n");
        CoUninitialize();
        return 1;
    }

    if (list_mode)
    {
        int rc = ListDevices(en);
        en->Release();
        CoUninitialize();
        return rc;
    }

    if (!want) want = "default";

    IMMDevice* dev = NULL;
    char devName[256] = { 0 };
    if (!_stricmp(want, "default"))
    {
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev)
            DevName(dev, devName, sizeof(devName));
    }
    else
    {
        IMMDeviceCollection* col = NULL;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col)
        {
            UINT32 n = 0;
            col->GetCount(&n);
            for (UINT32 i = 0; i < n; i++)
            {
                IMMDevice* d = NULL;
                if (FAILED(col->Item(i, &d)) || !d) continue;
                char nm[256] = { 0 };
                DevName(d, nm, sizeof(nm));
                if (ContainsNoCase(nm, want)) { dev = d; strcpy_s(devName, sizeof(devName), nm); break; }
                d->Release();
            }
            col->Release();
        }
    }
    if (!dev)
    {
        printf("device '%s' not found (run with --list)\n", want);
        en->Release();
        CoUninitialize();
        return 1;
    }
    printf("== device: %s   (%s, %d s per format)\n", devName,
           shared ? "SHARED" : "EXCLUSIVE", seconds);

    IAudioClient* cl = NULL;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cl)) || !cl)
    {
        printf("!! activate failed\n");
        dev->Release();
        en->Release();
        CoUninitialize();
        return 1;
    }

    int exitCode = 3;

    if (shared)
    {
        WAVEFORMATEX* mix = NULL;
        if (FAILED(cl->GetMixFormat(&mix)) || !mix)
        {
            printf("!! GetMixFormat failed\n");
        }
        else
        {
            WAVEFORMATEXTENSIBLE wfx;
            memset(&wfx, 0, sizeof(wfx));
            size_t cbsz = sizeof(WAVEFORMATEX) + mix->cbSize;
            if (cbsz > sizeof(wfx)) cbsz = sizeof(wfx);
            memcpy(&wfx, mix, cbsz);
            UINT32 rate = mix->nSamplesPerSec;
            printf("   mix: %u Hz / %u bit / %u ch tag=0x%X\n",
                   rate, mix->wBitsPerSample, mix->nChannels, mix->wFormatTag);
            CoTaskMemFree(mix);

            HRESULT hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        0, 0, &wfx.Format, NULL);
            if (FAILED(hr))
                printf("   !! SHARED Initialize 0x%08X\n", (unsigned)hr);
            else
            {
                UINT32 bf = 0;
                if (FAILED(cl->GetBufferSize(&bf)) || !bf)
                    printf("   !! GetBufferSize failed\n");
                else
                {
                    printf("   buffer %u fr (%.2f ms)\n", bf, bf * 1000.0 / (double)rate);
                    RateResult r;
                    RunOne(cl, &wfx.Format, bf, (double)seconds, &r);
                    if (!r.ok) printf("   !! %s\n", r.err);
                    else
                    {
                        printf("   -> written %.0f fr/s, device clock %.0f fr/s (%llu units/s)%s\n",
                               r.written, r.clock, (unsigned long long)r.clockFreq,
                               r.clockStalled ? " [clock not advancing]" :
                               (r.usedClock ? "" : " [no clock object]"));
                        printf("\n== SUMMARY '%s'  [SHARED]\n", devName);
                        printf("   %s %.1f%% of real time (%u Hz)\n",
                               r.usedClock ? "device clock:" : "written rate:", r.pct, rate);
                        if (r.pct >= 95.0) exitCode = 0;
                    }
                }
            }
        }
    }
    else
    {
        struct Cand { int rate, bits, ch, flt, valid; };
        Cand cand[] = {
            /* 44.1 kHz first: that is what the game runs at, so a hit here is
               the directly useful answer. `bits` is the CONTAINER size, `valid`
               the significant bits - pro interfaces often accept only 24-in-32,
               which the old table (wValidBits = bits) never offered. */
            { 44100, 16, 2, 0, 16 }, { 48000, 16, 2, 0, 16 },
            { 44100, 24, 2, 0, 24 }, { 48000, 24, 2, 0, 24 },
            { 44100, 32, 2, 0, 24 }, { 48000, 32, 2, 0, 24 },
            { 44100, 32, 2, 1, 32 }, { 48000, 32, 2, 1, 32 },
        };
        const int NCAND = (int)(sizeof(cand) / sizeof(cand[0]));

        int bestIdx = -1;
        RateResult best;
        memset(&best, 0, sizeof(best));
        UINT32 bestFrames = 0;

        for (int i = 0; i < NCAND; i++)
        {
            /* a client can only be Initialize()d once - re-activate per format */
            if (cl) { cl->Release(); cl = NULL; }
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cl)) || !cl)
            {
                printf("\n-- %d Hz / %d bit / %d ch --\n   !! activate failed\n",
                       cand[i].rate, cand[i].bits, cand[i].ch);
                cl = NULL;
                continue;
            }

            WAVEFORMATEXTENSIBLE wfx;
            memset(&wfx, 0, sizeof(wfx));
            wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
            wfx.Format.nChannels       = (WORD)cand[i].ch;
            wfx.Format.nSamplesPerSec  = cand[i].rate;
            wfx.Format.wBitsPerSample  = (WORD)cand[i].bits;
            wfx.Format.nBlockAlign     = (WORD)(cand[i].ch * cand[i].bits / 8);
            wfx.Format.nAvgBytesPerSec = cand[i].rate * wfx.Format.nBlockAlign;
            wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wfx.Samples.wValidBitsPerSample = (WORD)cand[i].valid;
            wfx.dwChannelMask = 0x3;
            wfx.SubFormat = cand[i].flt ? kSubFloat : kSubPCM;

            printf("\n-- %d Hz / %d%s bit / %d ch --\n", cand[i].rate,
                   cand[i].valid, (cand[i].valid != cand[i].bits) ? "-in-32" : "", cand[i].ch);

            WAVEFORMATEX* closest = NULL;
            HRESULT chk = cl->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, &closest);
            if (closest) CoTaskMemFree(closest);
            if (chk != S_OK)
            {
                printf("   not supported (0x%08X)\n", (unsigned)chk);
                continue;
            }

            REFERENCE_TIME def = 0, mn = 0;
            if (FAILED(cl->GetDevicePeriod(&def, &mn)) || def <= 0)
            {
                printf("   !! GetDevicePeriod failed, using 10 ms\n");
                def = 100000;   /* 10 ms */
            }
            UINT32 af = (UINT32)((double)def * cand[i].rate / 1e7 + 0.5);
            if (!af) af = (UINT32)(cand[i].rate / 100);   /* 10 ms fallback */
            REFERENCE_TIME al = (REFERENCE_TIME)(1e7 * (double)af / (double)cand[i].rate + 0.5);

            HRESULT hr = cl->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        al, al, &wfx.Format, NULL);
            if (FAILED(hr))
            {
                /* The old "retry" re-sent exactly the same parameters, so it
                   could never succeed; report the code instead. */
                printf("   Initialize 0x%08X%s\n", (unsigned)hr,
                       (hr == (HRESULT)0x88890019) ? " (buffer size not aligned)" : "");
                continue;
            }

            UINT32 bf = 0;
            if (FAILED(cl->GetBufferSize(&bf)) || !bf)
            {
                printf("   !! GetBufferSize failed\n");
                continue;
            }
            printf("   initialized: buffer %u fr (%.2f ms) - measuring...\n",
                   bf, bf * 1000.0 / (double)cand[i].rate);

            RateResult r;
            RunOne(cl, &wfx.Format, bf, (double)seconds, &r);
            if (!r.ok) { printf("   !! %s\n", r.err); continue; }

            printf("   -> written %.0f fr/s, device clock %.0f fr/s%s\n",
                   r.written, r.clock,
                   r.clockStalled ? " (clock not advancing)" :
                   (r.usedClock ? "" : " (no clock object)"));

            if (bestIdx < 0 || r.pct > best.pct)
            {
                best = r;
                bestIdx = i;
                bestFrames = bf;
            }
        }

        printf("\n== SUMMARY '%s'\n", devName);
        if (bestIdx < 0)
            printf("   NO usable exclusive format: every candidate either failed to\n"
                   "   initialize or could not be measured.\n");
        else
        {
            printf("   best: %d Hz / %d%s bit / %d ch  buffer %u fr  ->  %.1f%% of real time%s\n",
                   cand[bestIdx].rate, cand[bestIdx].valid,
                   (cand[bestIdx].valid != cand[bestIdx].bits) ? "-in-32" : "",
                   cand[bestIdx].ch, bestFrames, best.pct,
                   best.clockStalled ? " (clock not advancing)" :
                   (best.usedClock ? "" : " (from written frames)"));
            if (best.pct >= 95.0) exitCode = 0;
        }
    }

    if (cl) cl->Release();
    dev->Release();
    en->Release();
    CoUninitialize();
    return exitCode;
}
