/*
 * excl_rate_probe.cpp -- how fast does a WASAPI stream really run on a device?
 *
 * Key lesson: Initialize() succeeding means nothing. A driver can hand you a
 * 5 ms buffer and then never advance the hardware clock (Focusrite USB: 88.5%
 * of real time; some Realtek configs: 0%). So this tool tries every plausible
 * exclusive format and only reports success if the device clock really moves.
 *
 * Usage:
 *   excl_rate_probe --list
 *   excl_rate_probe "<device substring>" [--seconds N] [--shared]
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
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
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

static void DevName(IMMDevice* d, char* out, size_t cch)
{
    out[0] = 0;
    IPropertyStore* ps = NULL;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || !ps) return;
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        WideCharToMultiByte(CP_ACP, 0, pv.pwszVal, -1, out, (int)cch - 1, NULL, NULL);
    PropVariantClear(&pv);
    ps->Release();
}

/* Fill silence for `sec` seconds. Reports frames/s delivered, device clock
   frames/s, and the clock as a percentage of nominal. */
static void RunOne(IAudioClient* cl, WAVEFORMATEX* wf, UINT32 bufFrames,
                   double sec, double* outRate, double* outClock, double* outPct)
{
    *outRate = *outClock = *outPct = 0.0;
    HANDLE ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!ev || FAILED(cl->SetEventHandle(ev))) { if (ev) CloseHandle(ev); return; }
    IAudioRenderClient* rc = NULL;
    if (FAILED(cl->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) || !rc)
    { CloseHandle(ev); return; }
    IAudioClock* ck = NULL;
    cl->GetService(__uuidof(IAudioClock), (void**)&ck);

    UINT32 bpf = wf->nBlockAlign;

    BYTE* p0 = NULL;
    if (SUCCEEDED(rc->GetBuffer(bufFrames, &p0)) && p0)
    {
        memset(p0, 0, (size_t)bufFrames * bpf);
        rc->ReleaseBuffer(bufFrames, 0);
    }
    if (FAILED(cl->Start())) { rc->Release(); CloseHandle(ev); return; }

    UINT64 c0 = 0;
    if (ck) ck->GetPosition(&c0, NULL);

    unsigned long long frames = 0;
    double t0 = Now();
    for (;;)
    {
        double el = Now() - t0;
        if (el >= sec) break;
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

    UINT64 c1 = 0;
    if (ck) ck->GetPosition(&c1, NULL);
    cl->Stop();

    *outRate  = (double)frames / el;
    *outClock = (ck && c1 > c0) ? (double)(c1 - c0) / el : 0.0;
    if (wf->nSamplesPerSec) *outPct = 100.0 * *outClock / (double)wf->nSamplesPerSec;

    if (ck) ck->Release();
    rc->Release();
    CloseHandle(ev);
}

int main(int argc, char** argv)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    SetConsoleOutputCP(CP_UTF8);

    IMMDeviceEnumerator* en = NULL;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
    { printf("enumerator failed\n"); return 1; }

    if (argc >= 2 && _stricmp(argv[1], "--list") == 0)
    {
        IMMDeviceCollection* col = NULL;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
        UINT32 n = 0; col->GetCount(&n);
        for (UINT32 i = 0; i < n; i++)
        {
            IMMDevice* d = NULL;
            if (SUCCEEDED(col->Item(i, &d)) && d)
            {
                char nm[256]; DevName(d, nm, sizeof(nm));
                printf("  [%u] %s\n", i, nm);
                d->Release();
            }
        }
        col->Release(); en->Release(); CoUninitialize();
        return 0;
    }

    int shared = 0, seconds = 2;
    for (int i = 1; i < argc; i++)
    {
        if (_stricmp(argv[i], "--shared") == 0) shared = 1;
        else if (_stricmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);
    }
    const char* want = (argc >= 2 && argv[1][0] != '-') ? argv[1] : "default";

    IMMDevice* dev = NULL;
    char devName[256] = { 0 };
    if (_stricmp(want, "default") == 0)
    {
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev)
            DevName(dev, devName, sizeof(devName));
    }
    else
    {
        IMMDeviceCollection* col = NULL;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
        UINT32 n = 0; col->GetCount(&n);
        for (UINT32 i = 0; i < n; i++)
        {
            IMMDevice* d = NULL;
            if (FAILED(col->Item(i, &d)) || !d) continue;
            char nm[256]; DevName(d, nm, sizeof(nm));
            if (strstr(nm, want)) { dev = d; strcpy_s(devName, sizeof(devName), nm); break; }
            d->Release();
        }
        col->Release();
    }
    if (!dev) { printf("device '%s' not found\n", want); en->Release(); CoUninitialize(); return 1; }
    printf("== device: %s   (%s, %.0f s per format)\n", devName,
           shared ? "SHARED" : "EXCLUSIVE", (double)seconds);

    IAudioClient* cl = NULL;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cl)) || !cl)
    { printf("activate failed\n"); return 1; }

    struct Cand { int rate, bits, ch, flt; };
    Cand cand[] = {
        { 48000, 16, 2, 0 }, { 44100, 16, 2, 0 },
        { 48000, 24, 2, 0 }, { 44100, 24, 2, 0 },
        { 48000, 32, 2, 1 }, { 44100, 32, 2, 1 },
    };
    const int NCAND = (int)(sizeof(cand) / sizeof(cand[0]));

    WAVEFORMATEXTENSIBLE wfx;
    int bestIdx = -1;
    double bestPct = 0.0;
    UINT32 bestFrames = 0;

    if (shared)
    {
        WAVEFORMATEX* mix = NULL;
        if (FAILED(cl->GetMixFormat(&mix)) || !mix) { printf("GetMixFormat failed\n"); return 1; }
        memset(&wfx, 0, sizeof(wfx));
        size_t cbsz = sizeof(WAVEFORMATEX) + mix->cbSize;
        if (cbsz > sizeof(wfx)) cbsz = sizeof(wfx);
        memcpy(&wfx, mix, cbsz);
        printf("   mix: %u Hz / %u bit / %u ch tag=0x%X\n",
               mix->nSamplesPerSec, mix->wBitsPerSample, mix->nChannels, mix->wFormatTag);
        CoTaskMemFree(mix);

        HRESULT hr = cl->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    0, 0, &wfx.Format, NULL);
        if (FAILED(hr)) { printf("   SHARED Initialize 0x%08X\n", (unsigned)hr); return 1; }
        UINT32 bf = 0; cl->GetBufferSize(&bf);
        printf("   buffer %u fr (%.2f ms)\n", bf, bf * 1000.0 / wfx.Format.nSamplesPerSec);
        double r, c, p;
        RunOne(cl, &wfx.Format, bf, (double)seconds, &r, &c, &p);
        printf("   -> written %.0f fr/s, device clock %.0f fr/s (%.1f%% of %u)\n",
               r, c, p, wfx.Format.nSamplesPerSec);
        goto done;
    }

    for (int i = 0; i < NCAND; i++)
    {
        /* a client can only be Initialize()d once - re-activate for every format */
        if (cl) { cl->Release(); cl = NULL; }
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cl)) || !cl)
        { printf("\n-- %d Hz / %d bit / %d ch --\n   activate failed\n",
                 cand[i].rate, cand[i].bits, cand[i].ch); continue; }

        memset(&wfx, 0, sizeof(wfx));
        wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels       = (WORD)cand[i].ch;
        wfx.Format.nSamplesPerSec  = cand[i].rate;
        wfx.Format.wBitsPerSample  = (WORD)cand[i].bits;
        wfx.Format.nBlockAlign     = (WORD)(cand[i].ch * cand[i].bits / 8);
        wfx.Format.nAvgBytesPerSec = cand[i].rate * wfx.Format.nBlockAlign;
        wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = (WORD)cand[i].bits;
        wfx.dwChannelMask = 0x3;
        wfx.SubFormat = cand[i].flt ? kSubFloat : kSubPCM;

        printf("\n-- %d Hz / %d bit / %d ch --\n", cand[i].rate, cand[i].bits, cand[i].ch);

        WAVEFORMATEX* closest = NULL;
        HRESULT chk = cl->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, &closest);
        if (closest) CoTaskMemFree(closest);
        if (chk != S_OK) { printf("   not supported (0x%08X)\n", (unsigned)chk); continue; }

        REFERENCE_TIME def = 0, mn = 0;
        cl->GetDevicePeriod(&def, &mn);
        UINT32 af = (UINT32)((double)def * cand[i].rate / 1e7 + 0.5);
        REFERENCE_TIME al = (REFERENCE_TIME)(1e7 * af / (double)cand[i].rate + 0.5);

        HRESULT hr = cl->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    al, al, &wfx.Format, NULL);
        if (hr == (HRESULT)0x88890019)   /* BUFFER_SIZE_NOT_ALIGNED: try the engine period */
        {
            hr = cl->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                al, al, &wfx.Format, NULL);
        }
        if (FAILED(hr)) { printf("   Initialize 0x%08X\n", (unsigned)hr); continue; }

        UINT32 bf = 0; cl->GetBufferSize(&bf);
        printf("   initialized: buffer %u fr (%.2f ms) - measuring...\n",
               bf, bf * 1000.0 / cand[i].rate);

        double r, c, p;
        RunOne(cl, &wfx.Format, bf, (double)seconds, &r, &c, &p);
        printf("   -> written %.0f fr/s, device clock %.0f fr/s (%.1f%% of %d)\n",
               r, c, p, cand[i].rate);

        if (p > bestPct) { bestPct = p; bestIdx = i; bestFrames = bf; }
    }

done:
    printf("\n== SUMMARY '%s'\n", devName);
    if (bestIdx < 0)
        printf("   NO working exclusive format: every candidate either failed to\n"
               "   initialize or never advanced the device clock.\n");
    else
        printf("   best: %d Hz / %d bit / %d ch  buffer %u fr  ->  %.1f%% of real time\n",
               cand[bestIdx].rate, cand[bestIdx].bits, cand[bestIdx].ch, bestFrames, bestPct);

    if (cl) cl->Release();
    dev->Release();
    en->Release();
    CoUninitialize();
    return 0;
}
