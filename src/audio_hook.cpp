/*
 * audio_hook.cpp -- PDMMP audio hook (v13.7 build, 2026-10-18, CURRENT SOURCE)
 * ---------------------------------------------------------------------------
 * Two independent layers:
 *
 *   1) WASAPI buffer trim (always on, fake_client = 0)
 *        stream1 (game audio)  : drop RATEADJUST -> engine buffer 44.5 -> 22.0 ms
 *        stream2 (movie audio) : duration 666664 -> 166666 (66.7 -> 22.0 ms)
 *
 *   2) Client takeover (fake_client = 1, default since v13)
 *        IAudioClient::Initialize for stream1 is intercepted and the real
 *        Initialize is NEVER called: CRI gets our own client object, so the
 *        engine buffer leaves the path and the device stays free. The PCM CRI
 *        mixes goes into a lock-free ring, then out through one of:
 *            output = asio     native ASIO driver
 *            output = shared   our own WASAPI shared stream (resamples if the
 *                              endpoint is not 44.1 kHz)
 *            output = excl     WASAPI exclusive, guarded by a 2 s self-test
 *
 * Patched vtable slots (data layer only, never a code byte):
 *   IAudioClient          [3] Initialize, [14] GetService,
 *                         [13] SetEventHandle (only when fake_client = 1),
 *                         [4]..[12] GetBufferSize..Reset (only when fake_client = 1)
 *   IAudioRenderClient    [3]/[4]  (lazy; legacy tap path only)
 *
 * Not patched: any code in DivaMegaMix.exe. Denuvo-safe by construction.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <intrin.h>
#include <new>         /* std::nothrow */

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")

/* ASIO SDK headers (copies live in dev/asiosdk/, build with -Iasiosdk) */
#ifndef interface
#define interface struct
#endif
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

static FILE* g_log = NULL;
static char  g_dir[MAX_PATH] = { 0 };
static char  g_detachPath[MAX_PATH] = { 0 };
static volatile LONG g_sessionActive = 0;

/* ---- v13 config ----
 * Defaults match README: a missing/old audio_hook.txt must NOT silently
 * downgrade the mod to the v9 behaviour, so both flags default to 1. */
static int g_asioEn     = 1;    /* 1 = ASIO backend available            */
static int g_asioBlock  = 64;   /* ASIO block size (16..1024 step 16)     */
static int g_muteMode   = 1;    /* 1 = tap-and-zero (v11), 0 = SetMute    */
static int g_handoffMs  = 0;    /* 0 = auto (measured buffer + 2ms)       */
static char g_asioMatch[96] = "*";               /* "", "*" or "auto" = probe every driver */
static int  g_fakeClient = 1;    /* 1 = own CRI's stream1 client (no engine buffer) */
static int  g_outMode = 0;       /* 0 = ASIO, 1 = WASAPI exclusive, 2 = WASAPI shared */
static volatile LONG g_woState = 0;   /* 0=off 2=starting 3=running 4=failed */
static volatile LONG g_woReading = 0; /* WoThread is actively consuming the ring */
static volatile LONG g_woFault = 0;   /* WoThread hit a dead device and bailed out */
static volatile LONG g_woSelftest = 0;/* exclusive self-test running: fill silence to keep the clock moving */
static volatile LONG g_woUnder = 0;
static volatile LONG g_woCbs   = 0;
static volatile LONG64 g_woOut = 0;   /* frames handed to the output stream */
static volatile LONG g_fakeGbFail = 0;   /* CRI GetBuffer() rejected (queue full) */
static volatile LONG g_timerPeriod = 0;  /* timeBeginPeriod(1) is in effect */

static volatile LONG g_initCalls = 0;
static volatile LONG g_rcCalls   = 0;

static LARGE_INTEGER g_qpcFreq, g_qpc0;

static double MsNow(void)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    if (g_qpcFreq.QuadPart <= 0) return 0.0;   /* never divide by zero */
    return (double)(c.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_Initialize)(IAudioClient* self, AUDCLNT_SHAREMODE shareMode,
    DWORD flags, REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
    const WAVEFORMATEX* pFormat, LPCGUID audioSessionGuid);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GS)(IAudioClient* self, REFIID riid, void** ppv);
typedef HRESULT (STDMETHODCALLTYPE *PFN_RC_GB)(void* self, UINT32 frames, BYTE** ppData);
typedef HRESULT (STDMETHODCALLTYPE *PFN_RC_RB)(void* self, UINT32 frames, DWORD flags);

static PFN_Initialize g_orig = NULL;
static PFN_GS   oGS   = NULL;
static PFN_RC_GB oRCGB = NULL;
static PFN_RC_RB oRCRB = NULL;
static volatile LONG g_rcPatched = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char body[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    /* one fwrite per line: the timestamp can no longer be split from its text
       when the game thread, the ASIO callback and the watchdog log at once */
    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\n",
                        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                        (unsigned)st.wMilliseconds, body);
    if (n < 0) n = (int)strlen(line);   /* truncated body: _snprintf_s returns -1 */
    if (n > 0) fwrite(line, 1, (size_t)n, g_log);
    fflush(g_log);
}

static void MakePath(char* out, size_t cch, const char* name)
{
    strncpy_s(out, cch, g_dir, _TRUNCATE);
    strncat_s(out, cch, name, _TRUNCATE);
}

static int IsExecPtr(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static const char* HrBrief(HRESULT hr)
{
    if (hr == S_OK) return "S_OK";
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return "DEVICE_IN_USE";
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return "UNSUPPORTED_FORMAT";
    if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR) return "BUFFER_SIZE_ERROR";
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) return "BUFFER_SIZE_NOT_ALIGNED";
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return "DEVICE_INVALIDATED";
    if (hr == AUDCLNT_E_NOT_INITIALIZED) return "NOT_INITIALIZED";
    if (hr == E_INVALIDARG) return "E_INVALIDARG";
    if (hr == E_FAIL) return "E_FAIL";
    return "";
}

static BOOL PatchSlot(void** vtbl, int idx, void* newFn, void** outOrig)
{
    if (!vtbl || !IsExecPtr(vtbl[idx])) return FALSE;
    if (vtbl[idx] == newFn) return FALSE;   /* already ours -> never re-hook ourselves */
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &old)) return FALSE;
    if (outOrig) *outOrig = vtbl[idx];
    vtbl[idx] = newFn;
    DWORD tmp = 0;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[idx], sizeof(void*));
    return TRUE;
}

struct SlotPatch { int idx; void* fn; void** orig; };

/* Patch an entire set of slots or none at all. A half-patched vtable used to
 * leave slot 3 rewritten while the "patched" flag stayed 0, so the next attempt
 * captured our own hook as the "original" and recursed until the stack died. */
static BOOL PatchSlots(void** vtbl, const SlotPatch* list, int n)
{
    if (!vtbl) return FALSE;
    for (int i = 0; i < n; i++)
    {
        int idx = list[i].idx;
        if (!IsExecPtr(vtbl[idx])) return FALSE;
        if (vtbl[idx] == list[i].fn) return FALSE;   /* already hooked elsewhere */
    }
    int done = 0;
    for (; done < n; done++)
        if (!PatchSlot(vtbl, list[done].idx, list[done].fn, list[done].orig)) break;
    if (done == n) return TRUE;
    for (int i = done - 1; i >= 0; i--)
        PatchSlot(vtbl, list[i].idx, *list[i].orig, NULL);   /* roll back */
    return FALSE;
}

/* ===================================================================== */
/*  ASIO bridge state                                                    */
/* ===================================================================== */

static volatile LONG   g_asioState = 0;   /* 0=off 1=wait 2=starting 3=running 4=failed */
static volatile LONG   g_tapOn     = 0;
static volatile LONG64 g_tapFrames = 0;
static volatile LONG64 g_zeroed    = 0;
static volatile LONG64 g_ringDrops = 0;
static volatile LONG   g_underruns = 0;
static volatile LONG   g_asoCbs    = 0;
static volatile LONG   g_overloads = 0;   /* kAsioOverload messages (counted, not logged) */
static volatile LONG   g_muted     = 0;   /* legacy SetMute path state */
static volatile LONG   g_zeroTap   = 0;   /* v11: zero the game buffer after copy */
static volatile LONG   g_kicked    = 0;   /* stream1 died while bridging */
static volatile LONG   g_asioStartReq = 0;
static volatile LONG   g_s1BufFrames  = 0; /* measured stream1 buffer (frames) */
static volatile LONG   g_asioBusy     = 0; /* inside AsioFill (callback in flight) */

#define S1_MAX 8
static volatile LONG g_s1ClientN = 0;
static void* g_s1Clients[S1_MAX] = { 0 };

struct RcSlot { void* rc; BYTE* ptr; UINT32 frames; };
#define S1RC_MAX 16
static volatile LONG g_s1RcN = 0;
static RcSlot g_rcSlots[S1RC_MAX] = { 0 };

/* Both tables are rings: when the game rebuilds stream1 (device switch, plug,
   driver preemption) the newest client replaces the oldest entry instead of
   being dropped, so the tap cannot silently stop working after 8 rebuilds. */
static int IsS1Client(void* client)
{
    if (!client) return 0;
    for (int i = 0; i < S1_MAX; i++) if (g_s1Clients[i] == client) return 1;
    return 0;
}

static void AddS1Client(void* client)
{
    LONG n = InterlockedIncrement(&g_s1ClientN);
    int slot = (int)((n - 1) % S1_MAX);
    g_s1Clients[slot] = client;
    Log("[asio] stream1 client registered: %p (total %ld, slot %d)", client, n, slot);
    if (n > S1_MAX)
        Log("[asio] stream1 rebuilt %ld times - oldest client entries replaced", n - S1_MAX);
}

static int RcSlotFind(void* rc)
{
    for (int i = 0; i < S1RC_MAX; i++) if (g_rcSlots[i].rc == rc) return i;
    return -1;
}

static void AddS1Rc(void* rc)
{
    if (RcSlotFind(rc) >= 0) return;
    LONG n = InterlockedIncrement(&g_s1RcN);
    int slot = (int)((n - 1) % S1RC_MAX);
    g_rcSlots[slot].rc = rc;
    g_rcSlots[slot].ptr = NULL;
    g_rcSlots[slot].frames = 0;
    Log("[asio] stream1 render-client registered: %p (total %ld, slot %d)", rc, n, slot);
}

/* ---- device notification logger (diagnostic) ---- */

static void NLogId(const char* tag, LPCWSTR id, double tMs)
{
    char b[400] = { 0 };
    if (id) WideCharToMultiByte(CP_ACP, 0, id, -1, b, 399, NULL, NULL);
    const char* brace = strrchr(b, '{');
    Log("[notify] t=%.2fms %s '%s'", tMs, tag, brace ? brace : b);
}

class NotifyLogger : public IMMNotificationClient
{
public:
    volatile LONG ref;
    NotifyLogger() : ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() { ULONG n = (ULONG)InterlockedDecrement(&ref); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD st) {
        NLogId("STATE_CHANGED", id, MsNow()); Log("        newState=0x%X", st); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) { NLogId("DEVICE_ADDED", id, MsNow()); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) { NLogId("DEVICE_REMOVED", id, MsNow()); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow f, ERole r, LPCWSTR id) {
        (void)id;
        Log("[notify] t=%.2fms DEFAULT_CHANGED flow=%d role=%d", MsNow(), (int)f, (int)r);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY k) {
        wchar_t guid[64] = { 0 };
        StringFromGUID2(k.fmtid, guid, 63);
        char gb[96] = { 0 };
        WideCharToMultiByte(CP_ACP, 0, guid, -1, gb, 95, NULL, NULL);
        char b[400] = { 0 };
        if (id) WideCharToMultiByte(CP_ACP, 0, id, -1, b, 399, NULL, NULL);
        const char* brace = strrchr(b, '{');
        Log("[notify] t=%.2fms PROPCHANGED key={%s,%lu} dev='%s'", MsNow(), gb, (unsigned long)k.pid, brace ? brace : b);
        return S_OK;
    }
};

static NotifyLogger* g_notifier = NULL;

/* ---------------- lock-free ring (int16 stereo frames) ---------------- */

#define RING_CAP 65536            /* frames (1.49s @44.1k); power of two */
static short* g_ring = NULL;
static volatile LONG64 g_rw = 0, g_rr = 0;
/* frames actually consumed by an output backend - this is the play position we
   report through the fake IAudioClock (and it was hard-wired to 0 before) */
static volatile LONG64 g_fakePlayed = 0;

static unsigned RingAvail(void)
{
    if (!g_ring) return 0;
    LONG64 w = g_rw, r = g_rr;
    if (w < r) return 0;
    return (unsigned)(w - r);
}

static void RingWrite(const short* src, unsigned frames)
{
    if (!g_ring || !src || !frames) return;   /* no ring = nothing to feed */
    if (frames > RING_CAP) frames = RING_CAP;
    LONG64 w = g_rw;
    unsigned used = (unsigned)(w - g_rr);
    if (used + frames > RING_CAP) { InterlockedIncrement64(&g_ringDrops); return; }
    unsigned idx = (unsigned)(w & (RING_CAP - 1));
    unsigned first = RING_CAP - idx; if (first > frames) first = frames;
    memcpy(g_ring + (size_t)idx * 2, src, (size_t)first * 4);
    if (frames > first) memcpy(g_ring, src + (size_t)first * 2, (size_t)(frames - first) * 4);
    MemoryBarrier();
    g_rw = w + frames;
}

static unsigned RingRead(short* dst, unsigned want)
{
    if (!g_ring || !dst || !want) return 0;
    LONG64 r = g_rr;
    unsigned avail = (unsigned)(g_rw - r);
    unsigned n = avail < want ? avail : want;
    if (!n) return 0;
    unsigned idx = (unsigned)(r & (RING_CAP - 1));
    unsigned first = RING_CAP - idx; if (first > n) first = n;
    memcpy(dst, g_ring + (size_t)idx * 2, (size_t)first * 4);
    if (n > first) memcpy(dst + (size_t)first * 2, g_ring, (size_t)(n - first) * 4);
    MemoryBarrier();
    g_rr = r + n;
    InterlockedExchangeAdd64(&g_fakePlayed, (LONG64)n);   /* consumed == played */
    return n;
}

static void RingResyncToLatest(unsigned keep)
{
    LONG64 w = g_rw;
    LONG64 r = w - (LONG64)keep;
    if (r < 0) r = 0;
    g_rr = r;
}

/* ---------------- ASIO host objects ---------------- */

static IASIO*         g_asio = NULL;
static ASIOCallbacks  g_acbs;
static ASIOBufferInfo g_abufs[2];
static ASIOChannelInfo g_aci[2];
static long           g_ablock = 0;
static int            g_apostOut = 0;
static short*         g_tmp = NULL;      /* block*2 shorts staging (interleaved) */
static long           g_tmpBlock = 0;    /* block size g_tmp was sized for */

/*  v11 FIX: src is INTERLEAVED stereo (L0,R0,L1,R1,...). Each ASIO channel
 *  must take src[i*2 + ch], not src[i]. The v10 code copied the interleaved
 *  stream straight into each channel: every 32 frames got stretched into 64
 *  samples (one octave down + half the content dropped) = the reported
 *  "shredded + pitch down" audio.  */
/* bytes per sample for the ASIO formats we know about (0 = unsupported) */
static int AsioTypeBytes(long t)
{
    switch (t)
    {
    case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
    case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
    case ASIOSTInt32LSB: case ASIOSTInt32LSB16: case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
    case ASIOSTInt32MSB: case ASIOSTInt32MSB16: case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20: case ASIOSTInt32MSB24:
    case ASIOSTFloat32LSB: case ASIOSTFloat32MSB:
        return 4;
    case ASIOSTFloat64LSB: case ASIOSTFloat64MSB:
        return 8;
    default: return 0;
    }
}

/* Formats WriteSamples() can actually render. It used to accept everything
   AsioTypeBytes() knows about, so a driver reporting Int16MSB / Float64 passed
   the "unsupported format" gate, fell into the default: branch and played
   100 % silence while the log said the sample format looked fine (audit N22). */
static int IsTypeRenderable(long t)
{
    switch (t)
    {
    case ASIOSTInt16LSB:
    case ASIOSTInt24LSB:
    case ASIOSTInt32LSB: case ASIOSTInt32LSB16: case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
    case ASIOSTFloat32LSB:
        return 1;
    default:
        return 0;
    }
}

static void WriteSamples(void* p, long type, int ch, const short* src, unsigned got, unsigned total)
{
    if (!p) return;
    unsigned i;
    switch (type)
    {
    case ASIOSTInt32LSB16: case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
    case ASIOSTInt32LSB: {
        int32_t* o = (int32_t*)p;
        for (i = 0; i < got; i++) o[i] = ((int32_t)src[i * 2 + ch]) << 16;
        for (; i < total; i++) o[i] = 0;
    } break;
    case ASIOSTInt16LSB: {
        short* o = (short*)p;
        for (i = 0; i < got; i++) o[i] = src[i * 2 + ch];
        for (; i < total; i++) o[i] = 0;
    } break;
    case ASIOSTInt24LSB: {
        unsigned char* o = (unsigned char*)p;
        for (i = 0; i < total; i++) {
            /* 16-bit -> 24-bit is <<8, NOT <<16: with <<16 only bits 16..23 of the
               word are stored, i.e. the LOW byte of the sample, so every value
               below 256 became silence and 16384 became 0 (audit N27). */
            int32_t v = (i < got) ? (((int32_t)src[i * 2 + ch]) << 8) : 0;
            o[i * 3] = (unsigned char)v; o[i * 3 + 1] = (unsigned char)(v >> 8); o[i * 3 + 2] = (unsigned char)(v >> 16);
        }
    } break;
    case ASIOSTFloat32LSB: {
        float* o = (float*)p;
        for (i = 0; i < got; i++) o[i] = src[i * 2 + ch] / 32768.0f;
        for (; i < total; i++) o[i] = 0.0f;
    } break;
    default: {
        /* unknown / 8-byte formats: clear with the real sample width, otherwise
           half of every frame keeps stale data and plays back as noise */
        int b = AsioTypeBytes(type);
        memset(p, 0, (size_t)total * (size_t)(b > 0 ? b : 4));
    } break;
    }
}

static void FakePoke(void);   /* v13, defined in the fake-client section */
static BOOL WoStart(int exclusive);   /* v13, defined in the WASAPI-output section */
static void WoShutdown(void);         /* v13, defined in the WASAPI-output section */
static void RsFlush(void);            /* v13.3, drop buffered resampler frames */

static void AsioFill(long index)
{
    InterlockedIncrement(&g_asioBusy);   /* teardown waits for this to reach 0 */
    InterlockedIncrement(&g_asoCbs);
    long blk = g_ablock;
    unsigned got = RingRead(g_tmp, (unsigned)blk);
    /* robustness: this runs on the ASIO realtime callback thread. Never do
       file I/O, allocation, or logging here — it can stall the callback and
       worsen underruns. The monitor thread reports the counter once a second. */
    if (got < (unsigned)blk)
        InterlockedIncrement(&g_underruns);
    for (int c = 0; c < 2; c++)
        WriteSamples(g_abufs[c].buffers[index], g_aci[c].type, c, g_tmp, got, (unsigned)blk);
    if (g_apostOut && g_asio) g_asio->outputReady();
    FakePoke();   /* v13: top up CRI's fake client from the real output clock */
    InterlockedDecrement(&g_asioBusy);
}

static ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
{
    (void)direct;
    if (params && (params->timeInfo.flags & kSamplePositionValid)) { /* not used */ }
    AsioFill(index);
    return params;
}

static void cbBufferSwitch(long index, ASIOBool direct) { (void)direct; AsioFill(index); }
static void cbSampleRateDidChange(ASIOSampleRate r) { (void)r; }

static long cbAsioMessage(long selector, long value, void* msg, double* opt)
{
    (void)msg; (void)opt;
    switch (selector)
    {
    case kAsioSelectorSupported:
        /* Only claim the selectors we actually handle. Advertising
           kAsioResyncRequest / kAsioLatenciesChanged / kAsioResetRequest and
           then ignoring them in the default branch confuses the driver; the
           callback runs on the realtime thread, so kAsioOverload must not do
           file I/O (audit R7). */
        if (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo)
            return 1L;
        return 0L;
    case kAsioEngineVersion:    return 2L;
    case kAsioSupportsTimeInfo: return 1L;
    case kAsioSupportsTimeCode: return 0L;
    case kAsioOverload:         InterlockedIncrement(&g_overloads); return 0L;
    default:                    return 0L;
    }
}

/* ---------------- legacy session mute (mute_mode = 0) ---------------- */

/* Legacy mute path (mute_mode = 0) and the boot-time stale-mute cleanup.
 *
 * v13.1: this no longer goes through stream1's IAudioClient. With
 * fake_client = 1 that object never had a real Initialize(), so
 * GetService(ISimpleAudioVolume) returned AUDCLNT_E_NOT_INITIALIZED (0x88890001)
 * and the promised "clear the v10d mute residue" silently did nothing.
 * We now walk this process's own sessions on the default endpoint, which works
 * with or without the fake client (same approach as session_tool --unmute). */
static BOOL MuteSession(BOOL mute)
{
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager2* sm = NULL;
    IAudioSessionEnumerator* ses = NULL;
    BOOL ok = FALSE;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
        return FALSE;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) goto done;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&sm)) || !sm) goto done;
    if (FAILED(sm->GetSessionEnumerator(&ses)) || !ses) goto done;

    int count = 0;
    if (FAILED(ses->GetCount(&count))) goto done;
    for (int i = 0; i < count; i++)
    {
        IAudioSessionControl* ctl = NULL;
        if (FAILED(ses->GetSession(i, &ctl)) || !ctl) continue;
        IAudioSessionControl2* ctl2 = NULL;
        if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctl2)) && ctl2)
        {
            DWORD spid = 0;
            ctl2->GetProcessId(&spid);
            if (spid == GetCurrentProcessId())   /* never touch other apps / system sounds */
            {
                ISimpleAudioVolume* vol = NULL;
                if (SUCCEEDED(ctl2->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol)) && vol)
                {
                    if (SUCCEEDED(vol->SetMute(mute, NULL))) ok = TRUE;
                    vol->Release();
                }
            }
            ctl2->Release();
        }
        ctl->Release();
    }
done:
    if (ses) ses->Release();
    if (sm) sm->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (!ok) Log("[mute] no audio session of this process found (pid=%lu)",
                 (unsigned long)GetCurrentProcessId());
    return ok;
}

/* ---------------- driver load / prepare / teardown ---------------- */

/* "" / "*" / "auto" = no name filter: try every registered driver in turn.
   This is what the shipped config uses: the author's own card used to be the
   default value, which is useless on any other machine. */
static int AsioMatchAny(const char* m)
{
    return (!m || !m[0] || strcmp(m, "*") == 0 || _stricmp(m, "auto") == 0);
}

/* case-insensitive substring (README promises case-insensitive matching, but
   plain strstr() is not - "focusrite" must match "Focusrite USB ASIO") */
static const char* StrIStr(const char* hay, const char* needle)
{
    size_t nl = strlen(needle);
    if (!nl) return hay;
    size_t hl = strlen(hay);
    if (hl < nl) return NULL;
    for (size_t i = 0; i <= hl - nl; i++)
        if (_strnicmp(hay + i, needle, nl) == 0) return hay + i;
    return NULL;
}

/* Instantiate one driver by CLSID. Returns TRUE when g_asio is usable. */
static BOOL AsioConnectOne(const char* nm, const wchar_t* clsidW)
{
    CLSID id;
    if (FAILED(CLSIDFromString((LPCOLESTR)clsidW, &id))) return FALSE;
    HRESULT hr = CoCreateInstance(id, NULL, CLSCTX_INPROC_SERVER, id, (void**)&g_asio);
    Log("[asio] CoCreateInstance('%s') -> hr=0x%08X", nm, (unsigned)hr);
    if (SUCCEEDED(hr) && g_asio) return TRUE;
    g_asio = NULL;
    return FALSE;
}

static long AsioChooseBlock(long want, long mn, long mx, long pr, long gr)
{
    if (mn <= 0) mn = 16;            /* some drivers report minBuffSize = 0 */
    if (mx < mn) mx = mn;
    long block = want > 0 ? want : (pr > 0 ? pr : mn);
    if (gr < 0)
    {
        /* power-of-two granularity. Starting from 0 (or a bad min) used to
           shift left forever and hang the worker thread. */
        long b = mn;
        for (int guard = 0; b < block && b < mx && guard < 32; guard++) b <<= 1;
        block = b;
    }
    else if (gr > 0) block = mn + ((block - mn + gr - 1) / gr) * gr;
    if (block < mn) block = mn;
    if (block > mx) block = mx;
    return block;
}

/* One driver, end to end. Every failure path releases the driver, otherwise a
 * driver left init()ed stays claimed by this process for its whole lifetime
 * (DAWs then see "device in use"). */
static BOOL AsioPrepareOne(const char* regname, const wchar_t* clsidW)
{
    int buffers = 0, inited = 0;
    g_apostOut = 0;
    if (!AsioConnectOne(regname, clsidW)) return FALSE;

    if (g_asio->init((void*)GetDesktopWindow()) != ASIOTrue)
    {
        char e[124] = { 0 };
        g_asio->getErrorMessage(e);
        Log("[asio] init failed: %s", e);
        goto fail;                     /* init() did NOT succeed -> only Release() */
    }
    inited = 1;
    char nm[64] = { 0 };
    g_asio->getDriverName(nm);
    long nin = 0, nout = 0;
    if (g_asio->getChannels(&nin, &nout) != ASE_OK || nout < 2)
    {
        Log("[asio] channels in=%ld out=%ld - need >=2 outputs", nin, nout);
        goto fail;
    }
    long mn = 0, mx = 0, pr = 0, gr = 0;
    if (g_asio->getBufferSize(&mn, &mx, &pr, &gr) != ASE_OK) { Log("[asio] getBufferSize failed"); goto fail; }
    long block = AsioChooseBlock(g_asioBlock, mn, mx, pr, gr);
    if (block < 16) { Log("[asio] !! unusable ASIO block size %ld", block); goto fail; }

    ASIOSampleRate sr = 0;
    g_asio->getSampleRate(&sr);
    Log("[asio] driver='%s' block=%ld rate=%g (want block=%d mute_mode=%d driver_match='%s')",
        nm, block, sr, g_asioBlock, g_muteMode, g_asioMatch);

    /* v11 portability guard: never feed 44.1k data into a non-44.1k engine */
    if (fabs(sr - 44100.0) > 1.0)
    {
        Log("[asio] !! sample rate %g != 44100 - refusing to bridge (avoids pitch shift)", sr);
        goto fail;
    }

    if (g_asio->outputReady() == ASE_OK) g_apostOut = 1;

    memset(g_abufs, 0, sizeof(g_abufs));
    for (int c = 0; c < 2; c++) { g_abufs[c].isInput = ASIOFalse; g_abufs[c].channelNum = c; }

    memset(&g_acbs, 0, sizeof(g_acbs));
    g_acbs.bufferSwitch = cbBufferSwitch;
    g_acbs.sampleRateDidChange = cbSampleRateDidChange;
    g_acbs.asioMessage = cbAsioMessage;
    g_acbs.bufferSwitchTimeInfo = cbBufferSwitchTimeInfo;

    if (g_asio->createBuffers(g_abufs, 2, block, &g_acbs) != ASE_OK) { Log("[asio] createBuffers failed"); goto fail; }
    buffers = 1;
    g_ablock = block;

    for (int c = 0; c < 2; c++)
    {
        memset(&g_aci[c], 0, sizeof(ASIOChannelInfo));
        g_aci[c].channel = c;
        g_aci[c].isInput = ASIOFalse;
        if (g_asio->getChannelInfo(&g_aci[c]) != ASE_OK)
        {
            Log("[asio] !! getChannelInfo(ch%ld) failed - refusing to run silent", c);
            goto fail;
        }
        if (!IsTypeRenderable(g_aci[c].type))
        {
            Log("[asio] !! sample type %ld on ch%ld is not renderable - refusing", g_aci[c].type, c);
            goto fail;
        }
    }
    Log("[asio] out ch0 type=%ld '%s' | ch1 type=%ld '%s'", g_aci[0].type, g_aci[0].name, g_aci[1].type, g_aci[1].name);

    long il = 0, ol = 0;
    if (g_asio->getLatencies(&il, &ol) == ASE_OK)
        Log("[asio] latencies in=%ld out=%ld frames (%.2f / %.2f ms)", il, ol, il * 1000.0 / 44100.0, ol * 1000.0 / 44100.0);

    /* (Re)size the staging buffer for THIS block. A retry after a rollback can
       hand us a larger block, and RingRead() writes `block` frames - the old
       one-shot allocation then overflowed the heap (audit N2/N21). */
    if (!g_tmp || g_tmpBlock != block)
    {
        free(g_tmp);
        g_tmp = (short*)malloc((size_t)block * 2 * sizeof(short));
        g_tmpBlock = g_tmp ? block : 0;
    }
    if (!g_tmp) { Log("[asio] staging alloc failed (%ld frames)", block); goto fail; }

    /* pre-fill both halves with silence (spec: at least index 1) */
    for (long h = 0; h < 2; h++)
        for (int c = 0; c < 2; c++)
            WriteSamples(g_abufs[c].buffers[h], g_aci[c].type, c, g_tmp, 0, (unsigned)block);

    return TRUE;

fail:
    /* every failure path ends here: a driver left init()ed stays claimed by
       this process for its whole lifetime (DAWs then see "device in use") */
    Log("[asio] prepare failed -> releasing the driver (device becomes free again)");
    if (g_asio)
    {
        if (inited) g_asio->stop();          /* never call into a driver that refused init() */
        if (buffers) g_asio->disposeBuffers();
        g_asio->Release();
        g_asio = NULL;
    }
    g_apostOut = 0;
    return FALSE;
}

/* Try drivers in registry order until one is fully ready. The old code stopped
 * at the first driver whose COM server merely answered: on a machine with
 * several registered drivers that could be a virtual mixer/loopback device, a
 * disconnected interface, or one that cannot run at 44.1 kHz - and the mod then
 * sat in "prepare failed" while a perfectly good driver was never tried.
 *
 * Trade-off (audit R3): probing really does init() each candidate, and init()
 * has side effects on some drivers (panel UI, a brief device claim). It is
 * bounded at 8 attempts and every failure releases the driver, but a machine
 * with many DAW-installed drivers is better off pinning asio_driver=<name>. */
static BOOL AsioPrepare(void)
{
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hk) != ERROR_SUCCESS)
    {
        Log("[asio] HKLM\\SOFTWARE\\ASIO not found (no ASIO driver installed?)");
        return FALSE;
    }

    int any = AsioMatchAny(g_asioMatch);
    int matched = 0, tried = 0, ok = 0;
    DWORD idx = 0;
    for (;;)
    {
        char keyname[256] = { 0 }; DWORD klen = sizeof(keyname) - 1;
        if (RegEnumKeyExA(hk, idx++, keyname, &klen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hs;
        if (RegOpenKeyExA(hk, keyname, 0, KEY_READ, &hs) != ERROR_SUCCESS) continue;
        char clsid[256] = { 0 }; DWORD ty = 0, sz = sizeof(clsid) - 1;
        RegQueryValueExA(hs, "clsid", NULL, &ty, (LPBYTE)clsid, &sz);
        char desc[256] = { 0 }; sz = sizeof(desc) - 1;
        RegQueryValueExA(hs, "description", NULL, &ty, (LPBYTE)desc, &sz);
        RegCloseKey(hs);
        const char* nm = desc[0] ? desc : keyname;
        if (!clsid[0]) continue;                       /* unplayable registry entry */
        if (!any && !StrIStr(nm, g_asioMatch)) continue;   /* case-insensitive (audit R5) */

        matched++;
        if (tried >= 8) { Log("[asio] !! %d matching driver(s), stopped after 8 attempts", matched); break; }
        tried++;

        wchar_t wc[256] = { 0 };
        MultiByteToWideChar(CP_ACP, 0, clsid, -1, wc, 256);
        if (AsioPrepareOne(nm, wc)) { ok = 1; break; }
        Log("[asio] driver '%s' is not usable -> trying the next registered driver", nm);
    }
    RegCloseKey(hk);

    if (!ok)
        Log("[asio] !! no usable ASIO driver (match='%s', %d candidate(s), %d attempt(s))",
            g_asioMatch, matched, tried);
    else if (tried > 1 || any)
        Log("[asio] picked the first usable driver after %d attempt(s); if this is not your "
            "interface, run tools/asio_list.exe and pin asio_driver=<name>", tried);
    return ok ? TRUE : FALSE;
}

static void AsioTeardown(void)
{
    if (!g_asio) return;
    g_asio->stop();
    /* ASIO does not promise that bufferSwitch has returned when stop() does:
       freeing the buffers while the callback is still running is a UAF, and the
       "device kicked" path is exactly when it is most likely. */
    for (int i = 0; i < 200 && g_asioBusy; i++) Sleep(1);
    if (g_asioBusy)
        Log("[asio] !! callback still in flight after 200 ms - skipping disposeBuffers (leaking, not crashing)");
    else
        g_asio->disposeBuffers();
    g_asio->Release();
    g_asio = NULL;
    g_apostOut = 0;
}

/* rollback: drop ASIO and make sure the game still has an audible path */
static void AsioRollback(const char* why)
{
    Log("[asio] ROLLBACK: %s", why);
    InterlockedExchange(&g_zeroTap, 0);              /* game audio returns (tap path) */
    InterlockedExchange(&g_tapOn, 0);                /* stop copying to the ring */
    if (g_muted) { MuteSession(FALSE); InterlockedExchange(&g_muted, 0); }
    AsioTeardown();
    InterlockedExchange(&g_asioState, 4);
    InterlockedExchange(&g_asioStartReq, 0);         /* a rebuilt stream1 may try ASIO again */

    if (g_fakeClient)
    {
        /* With the fake client there is NO real WASAPI stream behind us, so the
           old "game audio returns via WASAPI" is impossible: leaving it at that
           meant permanent silence. Switch to our own shared stream instead. */
        Log("[asio] fake client: no engine stream exists -> switching to the WASAPI shared backend");
        if (!WoStart(0)) Log("[out] !! shared fallback failed as well - no audio path left");
    }
    else
        Log("[asio] WASAPI path is audible again (v11 behaviour)");
}

/* ---------------- start worker ---------------- */

/* wait until the tap has been running near real-time for ~300ms           */
static BOOL WaitForTapSteady(void)
{
    ULONGLONG wt0 = GetTickCount64();
    LONG64 prev = g_tapFrames;
    int good = 0, iters = 0;
    for (;;)
    {
        Sleep(100);
        iters++;
        LONG64 now = g_tapFrames;
        LONG64 d = now - prev;
        prev = now;
        if (d >= 3900) { good++; if (good >= 3) return TRUE; }   /* ~88% real-time: the tap is
                                                                     merely *producing* - looser
                                                                     than the excl self-test's 95%
                                                                     rollback bar (which judges the
                                                                     device clock itself) */
        else good = 0;
        /* robustness: never spin forever. If the tap can't reach real-time in
           30 s (device contention, audio service fault, ...), give up and let
           the game keep its normal WASAPI path. */
        if (GetTickCount64() - wt0 > 30000) return FALSE;
        if ((iters % 50) == 0)
            Log("[asio] still waiting for steady game audio (taps=%lld, last100ms=%lld)",
                (long long)now, (long long)d);
    }
}

static DWORD WINAPI AsioStartThread(LPVOID)
{
    /* IMPORTANT: the ASIO driver used during development only answers the
       driver-clsid IID from an STA apartment (MTA returns E_NOINTERFACE), and
       this is not unique to it - always initialise ASIO on an STA thread. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InterlockedExchange(&g_asioState, 2);

    Log("[asio] worker up - waiting for the game's audio tap to run steadily");
    if (!WaitForTapSteady())
    {
        Log("[asio] !! tap never reached steady real-time (30s) - keeping the current path");
        InterlockedExchange(&g_asioState, 4);
        InterlockedExchange(&g_asioStartReq, 0);
        if (g_fakeClient && g_woState != 3)
            if (!WoStart(0)) Log("[out] !! shared fallback failed - no audio");
        return 0;
    }
    Log("[handoff] tap steady (taps=%lld)", (long long)g_tapFrames);

    if (!AsioPrepare())
    {
        InterlockedExchange(&g_asioState, 4);
        InterlockedExchange(&g_asioStartReq, 0);   /* a rebuilt stream1 may try ASIO again */
        if (g_fakeClient)
        {
            Log("[asio] !! prepare failed -> falling back to WASAPI shared output");
            if (!WoStart(0)) Log("[out] !! shared fallback also failed - no audio");
        }
        else Log("[asio] !! prepare failed - staying on the WASAPI path (v9 behaviour)");
        return 0;
    }

    if (!g_fakeClient)
    {
        /* legacy tap path only: point the reader at the newest write (the frame
           the game will stop on), then silence the game side and let its engine
           queue drain. With the fake client there is no queue and no real
           stream, so this whole dance is skipped (v13.0 ran it anyway and threw
           away up to 10 ms of already mixed audio on every start). */
        RingResyncToLatest(0);

        long handoffMs = g_handoffMs;
        if (handoffMs <= 0)
        {
            long bf = g_s1BufFrames;
            if (bf < 100) bf = 970;
            handoffMs = (long)(bf * 1000.0 / 44100.0) + 2;
        }
        if (handoffMs < 5) handoffMs = 5;
        if (handoffMs > 150) handoffMs = 150;

        if (g_muteMode != 0)
        {
            InterlockedExchange(&g_zeroTap, 1);
            Log("[handoff] mode=tap-zero: zeroing starts; waiting %ld ms for the engine queue to drain", handoffMs);
        }
        else
        {
            if (!MuteSession(TRUE))
            {
                Log("[asio] !! mute failed -> rolling back to avoid double audio");
                AsioRollback("mute failed");
                return 0;
            }
            InterlockedExchange(&g_muted, 1);
            Log("[handoff] mode=set-mute: session muted; waiting %ld ms", handoffMs);
        }

        /* precise wait: Sleep(1) up to the last 2 ms, then spin */
        {
            double tEnd = MsNow() + (double)handoffMs;
            while (MsNow() < tEnd - 2.0) Sleep(1);
            while (MsNow() < tEnd) { YieldProcessor(); }
        }
    }
    else
        Log("[handoff] fake client: no engine queue to drain, starting ASIO directly");

    /* ASIO is about to become the ring consumer. The shared backend may still be
       running if it was started as the fallback after an earlier ASIO failure
       (or if the game rebuilt stream1 and re-tried ASIO). Two consumers reading
       the same ring each grab half the frames -> torn, pitch-shifted audio on
       both outputs (audit R1). Stop the shared stream first. */
    if (g_woState == 3) WoShutdown();

    if (g_asio->start() != ASE_OK)
    {
        Log("[asio] !! start failed");
        AsioRollback("start failed");
        return 0;
    }

    InterlockedExchange(&g_asioState, 3);
    Log("[handoff] ASIO started - output resumed seamlessly (block=%ld). monitoring...", g_ablock);

    /* monitor: kick / stall / runaway underruns (runs until the process ends) */
    int stall = 0, undFail = 0;
    LONG last = g_asoCbs, lastUnd = g_underruns, lastOvl = g_overloads;
    for (long long i = 1; i > 0; i++)
    {
        Sleep(1000);
        if (g_kicked)
        {
            AsioRollback("stream1 was invalidated (kick)");
            return 0;
        }
        if (g_asoCbs == last)
        {
            if (++stall >= 3)
            {
                AsioRollback("no ASIO callbacks for 3s");
                return 0;
            }
        }
        else stall = 0;
        last = g_asoCbs;

        LONG und = g_underruns;
        LONG dUnd = und - lastUnd;
        lastUnd = und;
        if (i > 10 && dUnd > 100)
        {
            if (++undFail >= 2)
            {
                AsioRollback("runaway underruns");
                return 0;
            }
        }
        else undFail = 0;
        if (g_overloads != lastOvl)          /* report from the monitor thread, */
        {                                    /* never from the realtime callback */
            Log("[asio] !! driver overload message (total %ld)", g_overloads);
            lastOvl = g_overloads;
        }
        if (i <= 20 || (i % 60) == 0)
            Log("[asio] t=%llds cbs=%ld und=%ld avail=%u zeroTap=%ld muted=%ld",
                i, g_asoCbs, g_underruns, RingAvail(), g_zeroTap, g_muted);
    }
    return 0;
}

/* ===================================================================== */
/*  render-client hooks (tap + zero)                                     */
/* ===================================================================== */

static HRESULT STDMETHODCALLTYPE Hook_RC_GetBuffer(void* self, UINT32 frames, BYTE** ppData)
{
    void* ra = _ReturnAddress();

    HRESULT hr = oRCGB ? oRCGB(self, frames, ppData) : E_FAIL;
    LONG n = InterlockedIncrement(&g_rcCalls);
    if (n <= 8)
        Log("[rc] GetBuffer(%u fr) hr=0x%08X %s", frames, (unsigned)hr, HrBrief(hr));
    if (FAILED(hr))
    {
        Log("[rc] !! GetBuffer(%u fr) FAILED hr=0x%08X from exe+0x%llX", frames, (unsigned)hr,
            (unsigned long long)((UINT_PTR)ra - (UINT_PTR)GetModuleHandleA(NULL)));
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED && g_asioState >= 2) InterlockedExchange(&g_kicked, 1);
    }

    /* v10 tap: remember where stream1's data will be written */
    if (SUCCEEDED(hr) && g_tapOn && frames > 0 && ppData && *ppData)
    {
        int si = RcSlotFind(self);
        if (si >= 0)
        {
            /* length first, pointer last: the release hook uses ptr != NULL as
               its gate, so it can never pair a new pointer with an old length */
            g_rcSlots[si].frames = frames;
            MemoryBarrier();
            g_rcSlots[si].ptr = *ppData;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_RC_ReleaseBuffer(void* self, UINT32 frames, DWORD flags)
{
    void* ra = _ReturnAddress();

    /* v11 tap + zero: copy the freshly written frames into the ring;
       when the bridge runs in tap-and-zero mode, wipe the game buffer right
       after the copy so the WASAPI engine plays silence (no SetMute, no
       driver reconfig, no double audio). */
    if (g_tapOn && frames > 0)
    {
        int si = RcSlotFind(self);
        if (si >= 0)
        {
            unsigned n = frames;
            if (g_rcSlots[si].frames > 0 && n > g_rcSlots[si].frames) n = g_rcSlots[si].frames;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                /* CRI really does send this (log: ReleaseBuffer(441, flags=0x2)).
                   Skipping it used to starve the ring during silence, which the
                   monitor then saw as "runaway underruns" and rolled back. */
                static const short zeros[512 * 2] = { 0 };
                unsigned left = n;
                while (left)
                {
                    unsigned c = left > 512 ? 512u : left;
                    RingWrite(zeros, c);
                    left -= c;
                }
            }
            else if (g_rcSlots[si].ptr)
            {
                RingWrite((const short*)g_rcSlots[si].ptr, n);
                if (g_zeroTap)
                {
                    memset(g_rcSlots[si].ptr, 0, (size_t)n * 4);
                    InterlockedExchangeAdd64(&g_zeroed, (LONG64)n);
                }
            }
            else n = 0;
            InterlockedExchangeAdd64(&g_tapFrames, (LONG64)n);
        }
    }

    HRESULT hr = oRCRB ? oRCRB(self, frames, flags) : E_FAIL;
    LONG n = InterlockedIncrement(&g_rcCalls);
    if (n <= 8)
        Log("[rc] ReleaseBuffer(%u fr, flags=0x%X) hr=0x%08X", frames, (unsigned)flags, (unsigned)hr);
    if (FAILED(hr))
    {
        Log("[rc] !! ReleaseBuffer(%u fr) FAILED hr=0x%08X from exe+0x%llX", frames, (unsigned)hr,
            (unsigned long long)((UINT_PTR)ra - (UINT_PTR)GetModuleHandleA(NULL)));
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED && g_asioState >= 2) InterlockedExchange(&g_kicked, 1);
    }
    return hr;
}

/* v13 forward decls (definitions live in the fake-client section below) */
static int    FakeIsSelf(IAudioClient* self);
static HRESULT FakeSetEventHandle(IAudioClient* self, HANDLE hEvent);
static HRESULT FakeGetService(IAudioClient* self, REFIID riid, void** ppv);
static void   FakePoke(void);

static HRESULT STDMETHODCALLTYPE Hook_GetService(IAudioClient* self, REFIID riid, void** ppv)
{
    if (FakeIsSelf(self)) return FakeGetService(self, riid, ppv);

    HRESULT hr = oGS ? oGS(self, riid, ppv) : E_FAIL;
    BOOL isRC = (riid.Data1 == 0xF294ACFC && riid.Data2 == 0x3146 && riid.Data3 == 0x4483);

    if (isRC)
        Log("[gs] GetService IAudioRenderClient -> hr=0x%08X ptr=%p", (unsigned)hr, (ppv ? *ppv : NULL));

    if (SUCCEEDED(hr) && ppv && *ppv && isRC)
    {
        if (g_asioEn && IsS1Client(self))
        {
            AddS1Rc(*ppv);
            InterlockedExchange(&g_tapOn, 1);
        }

        if (!g_rcPatched && InterlockedCompareExchange(&g_rcPatched, 1, 0) == 0)
        {
            void** vt = *(void***)*ppv;
            const SlotPatch list[] = {
                { 3, (void*)&Hook_RC_GetBuffer,     (void**)&oRCGB },
                { 4, (void*)&Hook_RC_ReleaseBuffer, (void**)&oRCRB },
            };
            if (vt && PatchSlots(vt, list, 2))
                Log("[gs] RC vtable patched (tap + zero armed)");
            else
            {
                InterlockedExchange(&g_rcPatched, 0);
                Log("[gs] !! RC patch failed (any partial slot was rolled back)");
            }
        }
    }
    return hr;
}

/* ------------- SetEventHandle (only used by the fake client) ------------- */

typedef HRESULT (STDMETHODCALLTYPE *PFN_SEH)(IAudioClient* self, HANDLE hEvent);
static PFN_SEH oSEH = NULL;

static HRESULT STDMETHODCALLTYPE Hook_SetEventHandle(IAudioClient* self, HANDLE hEvent)
{
    if (FakeIsSelf(self)) return FakeSetEventHandle(self, hEvent);
    return oSEH ? oSEH(self, hEvent) : E_FAIL;
}

/* ===================================================================== */
/*  v13: fake IAudioClient for stream1                                   */
/*                                                                       */
/*  Instead of letting CRI create a real WASAPI stream (whose engine     */
/*  buffer is 22 ms and which keeps the device busy), we hand CRI a      */
/*  client object we own. CRI mixes into our memory, so                   */
/*    - the 22 ms engine buffer disappears from the path, and             */
/*    - the audio device stays completely free for an exclusive stream.   */
/*  Only vtable slots are touched (data layer) -> Denuvo-safe.            */
/* ===================================================================== */

#define FAKE_FRAMES 441          /* 10 ms @44.1k - exactly what CRI requests */
#define FAKE_CHUNK  441          /* CRI's fill chunk */
#define FAKE_LOW    220          /* refill CRI when the queue drops below ~5 ms */

struct FakeState
{
    IAudioClient* self;
    WAVEFORMATEX  fmt;
    short*        stage;         /* FAKE_FRAMES*2 shorts - GetBuffer() target */
    short*        zero;          /* silent-release source */
    HANDLE        ev;
    volatile LONG on;
    volatile LONG running;
};
static FakeState g_fake;

typedef HRESULT (STDMETHODCALLTYPE *PFN_GBS)(IAudioClient*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GLAT)(IAudioClient*, REFERENCE_TIME*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GPAD)(IAudioClient*, UINT32*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ISF)(IAudioClient*, AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GMF)(IAudioClient*, WAVEFORMATEX**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GDP)(IAudioClient*, REFERENCE_TIME*, REFERENCE_TIME*);
typedef HRESULT (STDMETHODCALLTYPE *PFN_NOP)(IAudioClient*);

static PFN_GBS   oGBS  = NULL;
static PFN_GLAT  oGLat = NULL;
static PFN_GPAD  oGPad = NULL;
static PFN_ISF   oIsF  = NULL;
static PFN_GMF   oGMF  = NULL;
static PFN_GDP   oGDP  = NULL;
static PFN_NOP   oStart = NULL, oStop = NULL, oReset = NULL;
static volatile LONG g_fakePatched = 0;

static int FakeIsSelf(IAudioClient* self) { return g_fake.on && self == g_fake.self; }

/* ---- our IAudioRenderClient ---- */
class FakeRC : public IAudioRenderClient
{
public:
    FakeRC() : m_ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IAudioRenderClient)))
        { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 n, BYTE** pp) override
    {
        if (!pp) return E_POINTER;
        if (n > FAKE_FRAMES) { InterlockedIncrement(&g_fakeGbFail); return (HRESULT)0x88890006; }
        /* Back-pressure: give the consumer a few ms to drain instead of
           answering "buffer too large" (an error with a different meaning that
           only worked because CRI tolerates it). This runs on CRI's mixing
           thread, never on a realtime callback, so a short wait is safe.
           Sleep(1) here is only ~1 ms because PreInit() calls timeBeginPeriod(1)
           - if that is ever removed, each Sleep(1) becomes ~15.6 ms and this
           wait balloons to ~78 ms on the mixing thread (audit R12). */
        /* Sleep(1) is ~1 ms only while timeBeginPeriod(1) is in effect; without
           it each Sleep(1) rounds up to ~15.6 ms and 5 tries = ~78 ms on CRI's
           mixing thread. If the timer was not raised, do a single quick attempt
           and give up instead (the "buffer too large" answer is safe here). */
        int tries = g_timerPeriod ? 5 : 1;
        for (int i = 0; i < tries && RingAvail() + n > FAKE_FRAMES; i++) Sleep(1);
        if (RingAvail() + n > FAKE_FRAMES)
        {
            InterlockedIncrement(&g_fakeGbFail);
            return (HRESULT)0x88890006;   /* AUDCLNT_E_BUFFER_TOO_LARGE */
        }
        *pp = (BYTE*)g_fake.stage;
        LONG k = InterlockedIncrement(&m_calls);
        if (k <= 10) Log("[fake-rc] GetBuffer(%u) avail=%u", n, RingAvail());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 n, DWORD flags) override
    {
        LONG k2 = m_calls;
        if (k2 <= 10) Log("[fake-rc] ReleaseBuffer(%u, flags=0x%X)", n, flags);
        if (n > FAKE_FRAMES) return E_INVALIDARG;
        if (n)
        {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) RingWrite(g_fake.zero, n);
            else                                    RingWrite(g_fake.stage, n);
            InterlockedExchangeAdd64(&g_tapFrames, (LONG64)n);
        }
        return S_OK;
    }
private:
    LONG m_ref;
    LONG m_calls = 0;
};

/* ---- our IAudioClock ---- */
static const GUID kIID_IAudioClock =
{ 0xCD63314F, 0x3FD1, 0x4A1F, { 0xBA, 0x1B, 0x28, 0xED, 0x6E, 0xF1, 0xF6, 0x57 } };

class FakeClock : public IAudioClock
{
public:
    FakeClock() : m_ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, kIID_IAudioClock))
        { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* f) override
    { if (!f) return E_POINTER; *f = 44100; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* pos, UINT64* qpc) override
    {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        if (pos) *pos = (UINT64)g_fakePlayed;      /* frames really consumed */
        if (qpc) *qpc = (UINT64)c.QuadPart;        /* matching QPC timestamp   */
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* c) override
    {
        if (!c) return E_POINTER;
        *c = 0x1;   /* AUDCLNT_AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQUENCY */
        return S_OK;
    }
private:
    LONG m_ref;
};

static FakeRC*    g_fakeRc = NULL;
static FakeClock* g_fakeClk = NULL;

static HRESULT FakeSetEventHandle(IAudioClient* self, HANDLE h)
{
    (void)self;
    g_fake.ev = h;
    Log("[fake] SetEventHandle(%p)", h);
    return S_OK;
}

static HRESULT FakeGetService(IAudioClient* self, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (riid.Data1 == 0xF294ACFC && riid.Data2 == 0x3146 && riid.Data3 == 0x4483)
    {
        *ppv = (void*)g_fakeRc;
        g_fakeRc->AddRef();
        Log("[fake] GetService -> IAudioRenderClient");
        return S_OK;
    }
    if (IsEqualIID(riid, kIID_IAudioClock))
    {
        *ppv = (void*)g_fakeClk;
        g_fakeClk->AddRef();
        Log("[fake] GetService -> IAudioClock");
        return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(IUnknown)))
    {
        /* GetService(IUnknown) is not a service query, and CRI never issues it
           (the log shows only IAudioRenderClient). Return the client's own
           IUnknown so the identity matches what QueryInterface would hand back,
           not the render client's. */
        *ppv = self;
        self->AddRef();
        return S_OK;
    }
    Log("[fake] GetService unsupported riid=%08X-%04X-%04X", riid.Data1, riid.Data2, riid.Data3);
    return E_NOINTERFACE;
}

/* ---- driver: pace CRI from the real output clock ---- */
static void FakePoke(void)
{
    if (!g_fake.on || !g_fake.running || !g_fake.ev) return;
    if (RingAvail() <= FAKE_LOW) SetEvent(g_fake.ev);
}

static DWORD WINAPI FakeDriverThread(LPVOID)
{
    Log("[fake] driver up (frames=%u low=%u)", (unsigned)FAKE_FRAMES, (unsigned)FAKE_LOW);
    while (g_fake.running)
    {
        LONG as = g_asioState, ws = g_woState;
        if (as == 3 || ws == 3)
        {
            /* A real output clock drives us now. ASIO gives us a 1.45 ms callback
               to pace CRI from; the WASAPI stream can have a 20+ ms period, so
               there we must top the queue up here as well (the ring has ~10 ms
               of slack, so 1 ms polling is safe). */
            if (ws == 3) { FakePoke(); Sleep(1); }
            else Sleep(20);
            if (ws != 0)
            {
                static ULONGLONG last = 0;
                ULONGLONG now = GetTickCount64();
                if (now - last >= 5000)
                {
                    last = now;
                    Log("[out] cbs=%ld und=%ld avail=%u taps=%lld gbfail=%ld asio=%ld wo=%ld",
                        g_woCbs, g_woUnder, RingAvail(), (long long)g_tapFrames, g_fakeGbFail,
                        as, ws);
                }
            }
            continue;
        }
        if (g_woReading)
        {
            /* A consumer is already reading the ring. Draining here as well would
               hand the same frames to two consumers (audible repeats). Note that
               "starting" (state 2) is NOT such a case: WoThread only begins to
               read after Start() succeeded, and the drain is what keeps CRI's
               clock alive while ASIO waits for the tap to become steady. */
            Sleep(2);
            continue;
        }
        /* nothing running and nothing starting: drain at real time so CRI keeps
           a sane clock and the game never stalls on a full queue */
        Sleep(5);
        unsigned a = RingAvail();
        unsigned n = a >= FAKE_CHUNK ? FAKE_CHUNK / 2 : a;
        /* tmp must hold `n` stereo frames. RingAvail() is capped at FAKE_FRAMES
           by FakeRC::GetBuffer's back-pressure, so n is at most FAKE_CHUNK-1 =
           440 frames (880 shorts). The old tmp[512] (256 frames) overflowed
           whenever CRI handed us a 257..440-frame chunk (e.g. a partial fill). */
        if (n) { static short tmp[FAKE_CHUNK * 2]; RingRead(tmp, n); }
        FakePoke();
    }
    Log("[fake] driver stopped");
    return 0;
}

/* ---- fake vtable implementations (dispatch: ours vs real) ---- */
static HRESULT STDMETHODCALLTYPE Fake_GetBufferSize(IAudioClient* s, UINT32* p)
{
    if (FakeIsSelf(s)) { if (p) *p = FAKE_FRAMES; return S_OK; }
    return oGBS ? oGBS(s, p) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetStreamLatency(IAudioClient* s, REFERENCE_TIME* p)
{
    if (FakeIsSelf(s)) { if (p) *p = 0; return S_OK; }
    return oGLat ? oGLat(s, p) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetCurrentPadding(IAudioClient* s, UINT32* p)
{
    /* Close enough for CRI's bookkeeping: with a resampler active the ring is
       source frames while the stream counts output frames, so this over-reports
       by up to 10 ms. GetBuffer() uses the same RingAvail() gate, so the two
       stay consistent and the ring cannot overflow (audit N9). */
    if (FakeIsSelf(s)) { if (p) *p = RingAvail(); return S_OK; }
    return oGPad ? oGPad(s, p) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_IsFormatSupported(IAudioClient* s, AUDCLNT_SHAREMODE m,
                                                        const WAVEFORMATEX* f, WAVEFORMATEX** closest)
{
    if (FakeIsSelf(s)) { if (closest) *closest = NULL; return S_OK; }
    return oIsF ? oIsF(s, m, f, closest) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetMixFormat(IAudioClient* s, WAVEFORMATEX** pp)
{
    if (FakeIsSelf(s))
    {
        if (!pp) return E_POINTER;
        WAVEFORMATEX* w = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!w) return E_OUTOFMEMORY;
        *w = g_fake.fmt;
        w->cbSize = 0;
        *pp = w;
        return S_OK;
    }
    return oGMF ? oGMF(s, pp) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetDevicePeriod(IAudioClient* s, REFERENCE_TIME* def, REFERENCE_TIME* mn)
{
    if (FakeIsSelf(s)) { if (def) *def = 100000; if (mn) *mn = 30000; return S_OK; }
    return oGDP ? oGDP(s, def, mn) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_Start(IAudioClient* s)
{
    if (FakeIsSelf(s))
    {
        if (InterlockedExchange(&g_fake.running, 1) == 0)
        {
            HANDLE t = CreateThread(NULL, 0, FakeDriverThread, NULL, 0, NULL);
            if (t) CloseHandle(t); else InterlockedExchange(&g_fake.running, 0);
            Log("[fake] Start() -> driver launched");
        }
        return S_OK;
    }
    return oStart ? oStart(s) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_Stop(IAudioClient* s)
{
    if (FakeIsSelf(s))
    {
        InterlockedExchange(&g_fake.running, 0);
        RingResyncToLatest(0);   /* the next song must not start with old audio */
        RsFlush();               /* ...including the resampler's own 441 frames */
        Log("[fake] Stop()");
        return S_OK;
    }
    return oStop ? oStop(s) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_Reset(IAudioClient* s)
{
    if (FakeIsSelf(s))
    {
        RingResyncToLatest(0);   /* restart/seek: drop up to 10 ms of stale audio */
        RsFlush();
        return S_OK;
    }
    return oReset ? oReset(s) : E_FAIL;
}

/* ---- install: replace stream1's client ---- */
static HRESULT FakeInit(IAudioClient* self, const WAVEFORMATEX* fmt)
{
    if (!g_fake.stage)
    {
        g_fake.stage = (short*)calloc((size_t)FAKE_FRAMES * 2, sizeof(short));
        g_fake.zero  = (short*)calloc((size_t)FAKE_FRAMES * 2, sizeof(short));
    }
    if (!g_fake.stage || !g_fake.zero) return E_OUTOFMEMORY;

    if (!g_fakeRc)  g_fakeRc  = new (std::nothrow) FakeRC();
    if (!g_fakeClk) g_fakeClk = new (std::nothrow) FakeClock();
    if (!g_fakeRc || !g_fakeClk) return E_OUTOFMEMORY;   /* nothrow new, no exception */

    g_fake.self = self;
    memcpy(&g_fake.fmt, fmt, sizeof(WAVEFORMATEX));
    g_fake.fmt.cbSize = 0;
    g_fake.ev = NULL;
    InterlockedExchange(&g_fake.running, 0);

    if (!g_fakePatched)
    {
        if (InterlockedCompareExchange(&g_fakePatched, 1, 0) == 0)
        {
            void** vt = *(void***)self;
            const SlotPatch list[] = {
                {  4, (void*)&Fake_GetBufferSize,     (void**)&oGBS   },
                {  5, (void*)&Fake_GetStreamLatency,  (void**)&oGLat  },
                {  6, (void*)&Fake_GetCurrentPadding, (void**)&oGPad  },
                {  7, (void*)&Fake_IsFormatSupported, (void**)&oIsF   },
                {  8, (void*)&Fake_GetMixFormat,      (void**)&oGMF   },
                {  9, (void*)&Fake_GetDevicePeriod,   (void**)&oGDP   },
                { 10, (void*)&Fake_Start,             (void**)&oStart },
                { 11, (void*)&Fake_Stop,              (void**)&oStop  },
                { 12, (void*)&Fake_Reset,             (void**)&oReset },
            };
            if (!vt || !PatchSlots(vt, list, 9))
            {
                InterlockedExchange(&g_fakePatched, 0);   /* slots already rolled back */
                Log("[fake] !! vtable patch failed (all slots rolled back)");
                return E_FAIL;
            }
            Log("[fake] vtable slots 4..12 patched: OK");
        }
        else
        {
            /* another thread is patching right now */
            for (int i = 0; i < 100 && !g_fakePatched; i++) Sleep(1);
            if (!g_fakePatched) { Log("[fake] !! concurrent patch did not finish"); return E_FAIL; }
        }
    }

    /* only now is it safe for the hooks to treat this client as ours */
    InterlockedExchange(&g_fake.on, 1);

    Log("[fake] stream1 client replaced: %u Hz / %u bit / %u ch, %u frames",
        (unsigned)g_fake.fmt.nSamplesPerSec, (unsigned)g_fake.fmt.wBitsPerSample,
        (unsigned)g_fake.fmt.nChannels, (unsigned)FAKE_FRAMES);
    return S_OK;
}

/* ===================================================================== */
/*  v13: WASAPI output backends  (exclusive / shared)                    */
/*                                                                       */
/*  With stream1 faked, the audio device is free. We can therefore open   */
/*  our own stream on it - exclusive (device default period, 10 ms) or    */
/*  shared (engine period) - and feed it from the same ring the ASIO      */
/*  path uses.                                                            */
/* ===================================================================== */

static IAudioClient*       g_woClient = NULL;
static IAudioRenderClient* g_woRender = NULL;
static HANDLE              g_woEvent  = NULL;
static UINT32              g_woFrames = 0;
static int                 g_woFloat  = 0;
static int                 g_woCh     = 2;   /* output channel count */
static volatile LONG       g_woRun    = 0;
static volatile LONG       g_woAlive  = 0;   /* WoThread is inside its loop */

/* ---- resampler (only active when the endpoint is not 44.1 kHz) ----
 * CRI always hands us 44.1 kHz. A 48 kHz endpoint would otherwise play
 * everything 8.8 % fast (~1.5 semitones up) - which is exactly what v13.0 did,
 * because only the ASIO path had a sample-rate guard. Linear interpolation is
 * plenty for such a small ratio and costs one frame of latency. */
static double g_rsStep = 1.0;          /* source frames per output frame */
static double g_rsFrac = 0.0;
static short  g_rsCur[2] = { 0, 0 };
static short  g_rsNext[2] = { 0, 0 };
static int    g_rsPrimed = 0, g_rsHasNext = 0;
static short  g_rsBuf[1024 * 2];
static unsigned g_rsN = 0, g_rsI = 0;
static short  g_rsOut[2048 * 2];

static void RsReset(void)
{
    g_rsStep = 1.0; g_rsFrac = 0.0; g_rsPrimed = 0; g_rsHasNext = 0;
    g_rsN = g_rsI = 0;
    g_rsCur[0] = g_rsCur[1] = g_rsNext[0] = g_rsNext[1] = 0;
}

/* Drop everything buffered, but KEEP the step: Stop()/Reset() must not replay
   up to 441 frames of the previous song, and they must not silently turn a
   48 kHz endpoint back into "no resampling" either (audit N25). */
static void RsFlush(void)
{
    g_rsFrac = 0.0; g_rsPrimed = 0; g_rsHasNext = 0;
    g_rsN = g_rsI = 0;
    g_rsCur[0] = g_rsCur[1] = g_rsNext[0] = g_rsNext[1] = 0;
}

static BOOL RsNext(short* out)
{
    if (g_rsI >= g_rsN)
    {
        g_rsN = RingRead(g_rsBuf, 1024);
        g_rsI = 0;
        if (!g_rsN) return FALSE;
    }
    out[0] = g_rsBuf[g_rsI * 2];
    out[1] = g_rsBuf[g_rsI * 2 + 1];
    g_rsI++;
    return TRUE;
}

/* pull n output frames as interleaved int16 stereo */
static unsigned RsPull(short* dst, unsigned n)
{
    if (g_rsStep == 1.0) return RingRead(dst, n);   /* fast path: no resampling */

    unsigned produced = 0;
    while (produced < n)
    {
        if (!g_rsPrimed)
        {
            if (!RsNext(g_rsCur)) break;
            g_rsPrimed = 1;
            g_rsFrac = 0.0;
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
            g_rsCur[0] = g_rsNext[0];
            g_rsCur[1] = g_rsNext[1];
            g_rsFrac -= 1.0;
            if (!RsNext(g_rsNext)) { g_rsHasNext = 0; break; }
        }
    }
    return produced;
}

static void WoFill16(BYTE* p, UINT32 n)
{
    short* o = (short*)p;
    UINT32 done = 0, shortage = 0;
    while (done < n)
    {
        UINT32 c = n - done; if (c > 2048) c = 2048;
        unsigned got = RsPull(g_rsOut, c);
        for (UINT32 i = 0; i < c; i++)
        {
            short* d = o + (size_t)(done + i) * g_woCh;
            short l = (i < got) ? g_rsOut[i * 2] : 0;
            short r = (i < got) ? g_rsOut[i * 2 + 1] : 0;
            for (int ch = 0; ch < g_woCh; ch++) d[ch] = (ch == 0) ? l : (ch == 1 ? r : 0);
        }
        if (got < c) shortage += c - got;
        done += c;
    }
    if (shortage) InterlockedIncrement(&g_woUnder);
}

static void WoFillFloat(BYTE* p, UINT32 n)
{
    float* o = (float*)p;
    UINT32 done = 0, shortage = 0;
    const float k = 1.0f / 32768.0f;
    while (done < n)
    {
        UINT32 c = n - done; if (c > 2048) c = 2048;
        unsigned got = RsPull(g_rsOut, c);
        for (UINT32 i = 0; i < c; i++)
        {
            float* d = o + (size_t)(done + i) * g_woCh;
            float l = (i < got) ? (float)g_rsOut[i * 2] * k : 0.0f;
            float r = (i < got) ? (float)g_rsOut[i * 2 + 1] * k : 0.0f;
            for (int ch = 0; ch < g_woCh; ch++) d[ch] = (ch == 0) ? l : (ch == 1 ? r : 0.0f);
        }
        if (got < c) shortage += c - got;
        done += c;
    }
    if (shortage) InterlockedIncrement(&g_woUnder);
}

static DWORD WINAPI WoThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* IAudioClient calls are cross-apartment */
    InterlockedExchange(&g_woAlive, 1);
    Log("[wo] thread up (frames=%u float=%d)", (unsigned)g_woFrames, g_woFloat);
    if (FAILED(g_woClient->Start()))
    {
        Log("[wo] !! Start failed");
        InterlockedExchange(&g_woState, 4);
        InterlockedExchange(&g_woAlive, 0);   /* this path used to leave it set, so
                                                 rollback waited 2 s for a dead thread */
        return 0;
    }
    InterlockedExchange(&g_woState, 3);
    InterlockedExchange(&g_woReading, 1);
    Log("[wo] output running");
    int nopulse = 0, faults = 0;
    while (g_woRun)
    {
        DWORD w = WaitForSingleObject(g_woEvent, 200);
        if (!g_woRun) break;
        if (w != WAIT_OBJECT_0)
        {
            /* No event for 200 ms. One missed event is normal (the engine is
               idle), so this only *reports* it - the failure detection below is
               the one that acts. */
            if (++nopulse == 25) Log("[wo] !! no data event for 5 s (device paused?)");
            continue;
        }
        nopulse = 0;
        UINT32 pad = 0;
        HRESULT hp = g_woClient->GetCurrentPadding(&pad);
        /* This used to be "Sleep(2); continue;" - so a device that was
           unplugged, disabled or taken over by another app left the thread
           spinning forever in silence: state stayed 3, the drain gate stayed
           shut and not a single line was logged (audit N6). */
        if (FAILED(hp))
        {
            Log("[wo] !! GetCurrentPadding hr=0x%08X %s -> backend is dead, restarting it",
                (unsigned)hp, HrBrief(hp));
            InterlockedExchange(&g_woFault, 1);
            break;
        }
        UINT32 free_ = g_woFrames > pad ? g_woFrames - pad : 0;
        /* Only write what the ring can actually produce. Two units are involved:
           free_ is OUTPUT frames (device rate), RingAvail() is SOURCE frames
           (44.1 kHz). When step > 1 (endpoints below 44.1 kHz, e.g. 32 kHz) one
           output frame consumes more than one source frame, so handing `free_`
           straight to RsPull starves the resampler and pads ~27 % of every
           period with silence (music ran at 72.6 % speed). */
        UINT32 avail = RingAvail();
        if (g_rsStep > 1.0) avail = (UINT32)((double)avail / g_rsStep);
        UINT32 n = free_ < avail ? free_ : avail;
        /* During the exclusive self-test the game has not started mixing yet, so
           the ring is empty and `n` would be 0 -> nothing written -> the device
           has no data -> its clock never advances -> the self-test reads 0 % (a
           false negative). Fill silence to keep the clock moving so the test
           measures the real delivery rate (WoFill16/WoFillFloat already pad
           with zeros when the ring is dry). */
        if (!n && g_woSelftest) n = free_;
        if (n)
        {
            BYTE* p = NULL;
            HRESULT hb = g_woRender->GetBuffer(n, &p);
            if (SUCCEEDED(hb) && p)
            {
                faults = 0;
                if (g_woFloat) WoFillFloat(p, n); else WoFill16(p, n);
                g_woRender->ReleaseBuffer(n, 0);
                InterlockedIncrement(&g_woCbs);
                InterlockedExchangeAdd64(&g_woOut, (LONG64)n);
            }
            else if (++faults >= 50)
            {
                Log("[wo] !! GetBuffer failed %d times in a row (hr=0x%08X) -> restarting the backend",
                    faults, (unsigned)hb);
                InterlockedExchange(&g_woFault, 1);
                break;
            }
        }
        FakePoke();
    }
    InterlockedExchange(&g_woReading, 0);
    if (g_woClient) g_woClient->Stop();
    InterlockedExchange(&g_woAlive, 0);
    if (InterlockedExchange(&g_woFault, 0))
        InterlockedExchange(&g_woState, 4);   /* claimable again -> watchdog rebuilds */
    Log("[wo] thread exit");
    return 0;
}

/* Synchronous init: must claim the device before CRI creates stream2. */
static const GUID kSubFloat =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static BOOL WoInit(int exclusive)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* ignore RPC_E_CHANGED_MODE */

    /* Restart after a device loss: the previous stream is dead, but if a stuck
       writer thread still owns it, leave it alone - leak, do not crash. */
    if (!g_woAlive)
    {
        if (g_woRender) { g_woRender->Release(); g_woRender = NULL; }
        if (g_woClient) { g_woClient->Release(); g_woClient = NULL; }
        if (g_woEvent)  { CloseHandle(g_woEvent); g_woEvent = NULL; }
    }

    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    WAVEFORMATEX* wf = NULL;
    BOOL ok = FALSE;
    UINT32 rate = 44100;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { Log("[wo] !! enumerator hr=0x%08X", (unsigned)hr); return FALSE; }
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) { Log("[wo] !! endpoint hr=0x%08X", (unsigned)hr); goto done; }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_woClient);
    if (FAILED(hr) || !g_woClient) { Log("[wo] !! activate hr=0x%08X", (unsigned)hr); goto done; }

    g_woFloat = 0;

    if (exclusive)
    {
        wf = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!wf) goto done;
        memset(wf, 0, sizeof(WAVEFORMATEX));
        wf->wFormatTag      = WAVE_FORMAT_PCM;
        wf->nChannels       = 2;
        wf->nSamplesPerSec  = 44100;
        wf->wBitsPerSample  = 16;
        wf->nBlockAlign     = 4;
        wf->nAvgBytesPerSec = 44100 * 4;
        wf->cbSize          = 0;
        rate = 44100;
        WAVEFORMATEX* closest = NULL;
        HRESULT chk = g_woClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wf, &closest);
        if (closest) CoTaskMemFree(closest);
        if (chk != S_OK) { Log("[wo] !! exclusive 16-bit rejected hr=0x%08X", (unsigned)chk); goto done; }
    }
    else
    {
        if (FAILED(g_woClient->GetMixFormat(&wf)) || !wf) { Log("[wo] !! GetMixFormat failed"); goto done; }
        rate = wf->nSamplesPerSec;
        /* Only treat the stream as float when the sub-format really says so:
           32-bit INTEGER PCM used to be reinterpreted as float -> full-scale noise. */
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
            g_woFloat = IsEqualGUID(((const WAVEFORMATEXTENSIBLE*)wf)->SubFormat, kSubFloat) ? 1 : 0;
        else
            g_woFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ? 1 : 0;
        /* WoFill16() only knows 16-bit integer and WoFillFloat() only 32-bit
           float. A shared mix format of 24-bit packed or 32-bit INTEGER PCM
           would otherwise be written 2 bytes per sample -> full-scale noise
           (the exact reverse of the v13.1 float-misread fix). Refuse and let
           the caller fall back to another backend (audit R2). */
        if (!g_woFloat && wf->wBitsPerSample != 16)
        {
            Log("[wo] !! shared mix format is %u-bit integer PCM (only 16-bit int / 32-bit float supported) - refusing",
                wf->wBitsPerSample);
            goto done;
        }
        if (rate != 44100)
            Log("[wo] endpoint is %u Hz -> resampling 44100 -> %u (linear interpolation)", rate, rate);
    }

    if (rate < 8000 || rate > 384000)
    {
        Log("[wo] !! implausible endpoint rate %u Hz - refusing", rate);
        goto done;
    }

    REFERENCE_TIME dur = 0;
    REFERENCE_TIME per = 0;
    if (exclusive)
    {
        /* Use the device's default period instead of a hard-coded 5 ms. A 5 ms
           EVENTCALLBACK period collapses to ~88 % under any scheduling jitter,
           and even the default 10 ms period only reaches ~94.5 % inside the
           game: exclusive's software event loop is sensitive to system and
           background load (unlike ASIO's driver callback). Exclusive is not a
           recommended path - the default period just makes it as correct as it
           can be before the self-test rolls it back to shared. */
        REFERENCE_TIME def = 0, mn = 0;
        if (FAILED(g_woClient->GetDevicePeriod(&def, &mn)) || def <= 0)
            def = 100000;   /* 10 ms fallback */
        dur = def;
        per = def;
    }
    hr = g_woClient->Initialize(exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, per, wf, NULL);
    if (FAILED(hr))
    {
        Log("[wo] !! Initialize hr=0x%08X %s (tag=0x%X %u Hz %u bit %u ch cbSize=%u)",
            (unsigned)hr, HrBrief(hr), wf->wFormatTag, wf->nSamplesPerSec,
            wf->wBitsPerSample, wf->nChannels, wf->cbSize);
        goto done;
    }
    if (FAILED(g_woClient->GetBufferSize(&g_woFrames)))
    { Log("[wo] !! GetBufferSize failed"); goto done; }

    g_woEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_woEvent || FAILED(g_woClient->SetEventHandle(g_woEvent)))
    { Log("[wo] !! event setup failed"); goto done; }
    if (FAILED(g_woClient->GetService(__uuidof(IAudioRenderClient), (void**)&g_woRender)) || !g_woRender)
    { Log("[wo] !! GetService render failed"); goto done; }

    if (exclusive)
    {
        /* Exclusive + EVENTCALLBACK never signals the event unless the stream is
           pre-filled before Start(): with no data the device has nothing to play,
           so no event fires, the writer never runs, and the self-test reads
           written=0/clock=0. Fill one buffer with silence so the engine starts. */
        BYTE* pre = NULL;
        if (SUCCEEDED(g_woRender->GetBuffer(g_woFrames, &pre)) && pre)
        {
            memset(pre, 0, (size_t)g_woFrames * wf->nBlockAlign);
            g_woRender->ReleaseBuffer(g_woFrames, 0);
            Log("[wo] exclusive pre-filled %u frames of silence", g_woFrames);
        }
    }

    g_woCh = wf->nChannels;
    RsReset();
    g_rsStep = 44100.0 / (double)rate;     /* 1.0 on a 44.1 kHz endpoint */
    Log("[wo] ready: %s %u Hz / %u bit / %u ch tag=0x%X, buffer=%u fr (%.2f ms) resample=%s",
        exclusive ? "EXCLUSIVE" : "SHARED", wf->nSamplesPerSec, wf->wBitsPerSample, wf->nChannels,
        wf->wFormatTag, g_woFrames, g_woFrames * 1000.0 / (double)rate,
        (g_rsStep == 1.0) ? "no" : "yes");
    ok = TRUE;

done:
    if (!ok)
    {
        /* Every failure path releases here. A half-initialised exclusive client
           used to keep the device claimed, so the shared fallback (and every
           other app) saw DEVICE_IN_USE. */
        if (g_woRender) { g_woRender->Release(); g_woRender = NULL; }
        if (g_woClient) { g_woClient->Release(); g_woClient = NULL; }
        if (g_woEvent)  { CloseHandle(g_woEvent); g_woEvent = NULL; }
    }
    if (wf) CoTaskMemFree(wf);
    if (dev) dev->Release();
    if (en) en->Release();
    return ok;
}

/* Stop the writer thread and free the stream - but only once the thread has
   really left its loop. Freeing under it was the v13.0 use-after-free. */
static void WoShutdown(void)
{
    InterlockedExchange(&g_woRun, 0);
    for (int i = 0; i < 400 && g_woAlive; i++) Sleep(5);   /* blocks up to 2 s */
    if (g_woAlive)
    {
        Log("[wo] !! thread did not stop within 2 s - leaking the stream objects instead of freeing them");
        InterlockedExchange(&g_woState, 4);
        return;
    }
    if (g_woClient) g_woClient->Stop();
    if (g_woRender) { g_woRender->Release(); g_woRender = NULL; }
    if (g_woClient) { g_woClient->Release(); g_woClient = NULL; }
    if (g_woEvent)  { CloseHandle(g_woEvent); g_woEvent = NULL; }
    InterlockedExchange(&g_woState, 0);
}

/* Init + start. exclusive==1 -> exclusive, else shared.
   Exclusive runs a self-test (skip the clock ramp-up, then measure 2 s) and
   rolls back to shared when the device delivers under 95 % of real time. That
   threshold matters: exclusive's software event loop is sensitive to system and
   background load, so inside the game it typically delivers ~94.5 % (a 5 ms
   period: ~88 %) - not a driver fault, but the reason exclusive is not the
   recommended path. The self-test cross-checks the device clock (IAudioClock)
   as well, because "frames written" alone can look healthy while the hardware
   is not moving. */
static BOOL WoStart(int exclusive)
{
    /* ASIO and the WASAPI backend must never run at once: both read the same
       ring and each would grab half the frames (audit R1). AsioRollback calls
       us AFTER setting g_asioState=4, so this only rejects an external caller
       that tries to start a WASAPI stream while ASIO is already running. */
    if (g_asioState == 3)
    {
        Log("[wo] !! ASIO is running - refusing to start a WASAPI backend (would double-consume the ring)");
        return FALSE;
    }

    BOOL claimed = FALSE;
    for (int spin = 0; spin < 400; spin++)
    {
        LONG st = g_woState;
        /* A running backend is accepted as-is. This also means that if
           exclusive's self-test failed and the mod fell back to shared, a later
           re-try of exclusive sees state==3 and returns here without testing
           again - exclusive is never re-attempted for the rest of the process
           (audit R4). That is deliberate: the self-test failure is usually a
           hardware rate below 95 % (a property of the device, not a glitch),
           and exclusive is not a recommended path. */
        if (st == 3) return TRUE;
        if (st == 0 || st == 4)     /* 4 = previous failure; allow a retry */
        {
            /* never re-init on top of a writer thread that is still alive:
               WoShutdown() sets 4 after a 2 s timeout precisely because that
               thread could not be stopped. */
            if (!g_woAlive &&
                InterlockedCompareExchange(&g_woState, 2, st) == st) { claimed = TRUE; break; }
        }
        Sleep(5);
    }
    if (!claimed) { Log("[wo] !! could not claim the output backend"); return FALSE; }

    if (!WoInit(exclusive))
    {
        Log("[wo] !! init failed (%s)", exclusive ? "exclusive" : "shared");
        InterlockedExchange(&g_woState, 4);
        return FALSE;
    }

    InterlockedExchange64(&g_woOut, 0);
    InterlockedExchange(&g_woRun, 1);
    HANDLE t = CreateThread(NULL, 0, WoThread, NULL, 0, NULL);
    if (!t) { Log("[wo] !! CreateThread failed"); WoShutdown(); return FALSE; }
    CloseHandle(t);

    /* Wait for WoThread to report success (3) or failure (4): reporting success
       straight after CreateThread hid Start() failures and blocked the fallback. */
    for (int i = 0; i < 200 && g_woState == 2; i++) Sleep(5);
    if (g_woState != 3)
    {
        Log("[wo] !! output did not start (%s)", exclusive ? "exclusive" : "shared");
        WoShutdown();
        return FALSE;
    }

    if (exclusive)
    {
        UINT64 c0 = 0, c1 = 0;
        BOOL haveClk = FALSE;
        IAudioClock* ck = NULL;
        if (SUCCEEDED(g_woClient->GetService(__uuidof(IAudioClock), (void**)&ck)) && ck)
            haveClk = SUCCEEDED(ck->GetPosition(&c0, NULL));

        /* Two false-failure modes to avoid: (1) the clock needs ~70 ms to ramp
           up after Start(), so a 600 ms window under-reports; (2) the game does
           not start mixing until ~2.5 s after the client is installed, so over
           an empty ring nothing is written and the clock never advances -> 0 %.
           Skip the ramp-up, measure over 2 s, and flag the writer to fill
           silence so the clock keeps moving before the game starts mixing. The
           number it then reports is the REAL delivery rate (exclusive typically
           reads ~94.5 % inside the game because system/background load steals
           its software event loop), not a measurement artifact. */
        InterlockedExchange(&g_woSelftest, 1);
        Sleep(200);
        if (haveClk) ck->GetPosition(&c0, NULL);
        LONG64 out0 = (LONG64)g_woOut;

        Sleep(2000);

        LONG64 out = (LONG64)g_woOut - out0;
        double outRate = (double)out / 2.0;
        double clkRate = 0.0;
        if (haveClk && SUCCEEDED(ck->GetPosition(&c1, NULL)))
            clkRate = (double)(c1 - c0) / 2.0;
        if (ck) ck->Release();
        InterlockedExchange(&g_woSelftest, 0);

        double rate = outRate;
        if (haveClk && clkRate > 0.0 && clkRate < rate) rate = clkRate;  /* the honest one */
        Log("[wo] exclusive self-test: written=%.0f fr/s clock=%.0f fr/s -> %.1f%% of 44100",
            outRate, clkRate, 100.0 * rate / 44100.0);

        if (rate < 44100.0 * 0.95)
        {
            Log("[wo] !! exclusive self-test FAILED (%.1f%%) - rolling back",
                100.0 * rate / 44100.0);
            WoShutdown();
            InterlockedExchange(&g_woCbs, 0);
            InterlockedExchange(&g_woUnder, 0);
            return FALSE;
        }
        Log("[wo] exclusive self-test OK (%.1f%%)", 100.0 * rate / 44100.0);
    }
    return TRUE;
}

/* ------------- Initialize: param rewrite ------------- */

static HRESULT STDMETHODCALLTYPE Hook_Initialize(IAudioClient* self, AUDCLNT_SHAREMODE shareMode,
    DWORD flags, REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
    const WAVEFORMATEX* pFormat, LPCGUID audioSessionGuid)
{
    int fixed1 = 0, fixed2 = 0, fake = 0;

    if (g_orig == NULL) return E_FAIL;
    InterlockedIncrement(&g_initCalls);
    Log("[init] call from exe+0x%llX shareMode=%d flags=0x%X dur=%lld",
        (unsigned long long)((UINT_PTR)_ReturnAddress() - (UINT_PTR)GetModuleHandleA(NULL)),
        (int)shareMode, (unsigned)flags, (long long)hnsBufferDuration);

    if (shareMode == AUDCLNT_SHAREMODE_SHARED && pFormat)
    {
        if (flags == 0x00140000u &&
            pFormat->nSamplesPerSec == 44100 && pFormat->wBitsPerSample == 16 && pFormat->nChannels == 2)
        {
            fixed1 = 1;

            if (g_fakeClient)
            {
                fake = 1;                 /* v13: CRI gets our client, no real stream */
            }
            else
            {
                flags = 0x00040000u;
            }
        }
        else if (flags == 0 && pFormat->wBitsPerSample == 32 && hnsBufferDuration >= 600000)
        {
            hnsBufferDuration = 166666;
            fixed2 = 1;
        }
    }

    HRESULT hr;
    if (fake)
    {
        hr = FakeInit(self, pFormat);
        if (FAILED(hr))
        {
            /* The takeover is an enhancement, never a requirement: if it cannot
               be installed, give the client back to the real WASAPI path instead
               of returning a failure that leaves the game silent. */
            Log("[fake] !! takeover failed (hr=0x%08X) -> using the real WASAPI client", (unsigned)hr);
            InterlockedExchange(&g_fake.on, 0);
            fake = 0;
            flags = 0x00040000u;   /* still drop RATEADJUST: 44.5 ms -> 22 ms */
            hr = g_orig(self, shareMode, flags, hnsBufferDuration, hnsPeriodicity, pFormat, audioSessionGuid);
        }
    }
    else
        hr = g_orig(self, shareMode, flags, hnsBufferDuration, hnsPeriodicity, pFormat, audioSessionGuid);

    UINT32 bufFrames = 0;
    REFERENCE_TIME sLat = 0;
    if (self != NULL && SUCCEEDED(hr) && (fixed1 || fixed2))
    {
        if (FAILED(self->GetBufferSize(&bufFrames))) bufFrames = 0;
        if (FAILED(self->GetStreamLatency(&sLat))) sLat = 0;
    }

    if (fake)
        Log("stream1 v13: FAKE client (no engine buffer, no device hold)  hr=0x%08X %s  buffer=%u fr",
            (unsigned)hr, HrBrief(hr), bufFrames);
    else if (fixed1)
        Log("stream1 FIX: flags 0x00140000 -> 0x00040000 (RATEADJUST removed)  hr=0x%08X %s",
            (unsigned)hr, HrBrief(hr));
    if (fixed1) Log("stream1 -> engine granted buffer = %u frames (%.2f ms)  streamLatency=%.2f ms",
                    bufFrames, bufFrames * 1000.0 / 44100.0, sLat / 10000.0);
    if (fixed2) Log("stream2 FIX: duration -> 166666 (was >=600000)  hr=0x%08X %s",
                    (unsigned)hr, HrBrief(hr));
    if (fixed2) Log("stream2 -> engine granted buffer = %u frames (%.2f ms)  streamLatency=%.2f ms",
                    bufFrames, bufFrames * 1000.0 / 44100.0, sLat / 10000.0);
    if (!fixed1 && !fixed2)
        Log("passthrough: shareMode=%d flags=0x%X dur=%lld  hr=0x%08X",
            (int)shareMode, (unsigned)flags, (long long)hnsBufferDuration, (unsigned)hr);

    /* v10: kick off the ASIO bridge once, when stream1 exists */
    if (fixed1 && SUCCEEDED(hr))
    {
        AddS1Client(self);
        if (bufFrames > 0) InterlockedExchange(&g_s1BufFrames, (LONG)bufFrames);

        /* v11.1: clear stale session-mute residue. The v10/v10d builds used
           SetMute(TRUE) on this session and runs that ended in a forced kill
           left the Windows audio engine remembering the game as MUTED forever
           (it persists per app path across restarts) -> "no sound after the
           mod is disabled". Clear it every boot; harmless when already clean.

           With the fake client stream1 has no real session, so this walk finds
           nothing - the deferred cleanup in the watchdog (at 5 s, once the
           output backend / stream2 has created a session) does it instead.
           Skip the pointless session enumeration here. */
        if (!g_fakeClient && MuteSession(FALSE))
            Log("[fix] cleared stale session-mute residue (v10d era)");

        if (fake && g_outMode != 0)
        {
            Log("[out] stream1 ready -> WASAPI %s backend", g_outMode == 1 ? "EXCLUSIVE" : "SHARED");
            if (!WoStart(g_outMode == 1))
            {
                Log("[out] !! %s backend failed -> trying shared", g_outMode == 1 ? "exclusive" : "shared");
                if (!WoStart(0))
                {
                    Log("[out] !! shared failed -> falling back to ASIO");
                    if (g_asioEn && InterlockedCompareExchange(&g_asioStartReq, 1, 0) == 0)
                    {
                        HANDLE t = CreateThread(NULL, 0, AsioStartThread, NULL, 0, NULL);
                        if (t) CloseHandle(t);
                    }
                }
            }
        }
        else if (g_asioEn)
        {
            if (InterlockedCompareExchange(&g_asioStartReq, 1, 0) == 0)
            {
                Log("[asio] stream1 ready -> starting ASIO worker");
                HANDLE t = CreateThread(NULL, 0, AsioStartThread, NULL, 0, NULL);
                if (t) CloseHandle(t);
                else Log("[asio] !! CreateThread failed");
            }
        }
    }
    return hr;
}

/* ------------------------------ config ------------------------------ */

static void ReadConfig(void)
{
    char path[MAX_PATH];
    MakePath(path, sizeof(path), "audio_hook.txt");
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        Log("no audio_hook.txt -> using defaults (fake_client=1, output=asio)");
    else
    {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        /* parse LINE BY LINE: strchr(p,'=') used to jump across a line that had
           no '=', swallowing every key between it and the next real '=' */
        char* p = buf;
        while (p && *p)
        {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            char* line = p;
            p = nl ? nl + 1 : NULL;

            while (*line == ' ' || *line == '\t' || *line == '\r') line++;
            if (!*line || *line == '#' || *line == ';') continue;

            char* eq = strchr(line, '=');
            if (!eq) { Log("config: ignoring malformed line '%s'", line); continue; }
            *eq = '\0';

            char key[64] = { 0 };
            int k = 0;
            for (char* q = line; *q && k < 63; q++)
                if (*q != ' ' && *q != '\t' && *q != '\r') key[k++] = *q;

            char* v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            char* ve = v + strlen(v);
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) *--ve = '\0';

            if (_stricmp(key, "asio_driver") == 0)
            {
                /* tolerate asio_driver = "*" / 'name' - the docs quote the value,
                   so people paste the quotes too (audit R6) */
                char* s = v;
                size_t L = strlen(s);
                if (L >= 2 && ((s[0] == '"' && s[L - 1] == '"') ||
                               (s[0] == '\'' && s[L - 1] == '\'')))
                { s[L - 1] = '\0'; s++; }
                strncpy_s(g_asioMatch, sizeof(g_asioMatch), s, _TRUNCATE);
            }
            else if (_stricmp(key, "output") == 0)
            {
                if (_strnicmp(v, "excl", 4) == 0)        g_outMode = 1;
                else if (_strnicmp(v, "shared", 6) == 0) g_outMode = 2;
                else                                     g_outMode = 0;   /* asio */
            }
            else
            {
                int val = atoi(v);
                if (_stricmp(key, "asio") == 0) g_asioEn = val;
                else if (_stricmp(key, "block") == 0) g_asioBlock = val;
                else if (_stricmp(key, "mute_mode") == 0) g_muteMode = val;
                else if (_stricmp(key, "handoff_ms") == 0) g_handoffMs = val;
                else if (_stricmp(key, "fake_client") == 0) g_fakeClient = val;
                else Log("config: unknown key '%s' ignored", key);
            }
        }
    }

    if (g_asioBlock < 16) g_asioBlock = 16;
    if (g_asioBlock > 1024) g_asioBlock = 1024;
    if (g_handoffMs < 0) g_handoffMs = 0;
    if (g_handoffMs > 150) g_handoffMs = 150;
    g_asioEn = g_asioEn ? 1 : 0;
    g_fakeClient = g_fakeClient ? 1 : 0;

    if (g_outMode != 0 && !g_fakeClient)
    {
        Log("!! output=%s requires fake_client=1 -> falling back to ASIO path",
            g_outMode == 1 ? "excl" : "shared");
        g_outMode = 0;
    }
    if (g_fakeClient && !g_asioEn && g_outMode == 0)
    {
        /* takeover with every backend disabled = guaranteed silence */
        Log("!! fake_client=1 with asio=0 and output=asio -> forcing output=shared");
        g_outMode = 2;
    }
    Log("config: asio=%d block=%d | mute_mode=%d handoff_ms=%d | driver='%s'%s | fake_client=%d | output=%s",
        g_asioEn, g_asioBlock, g_muteMode, g_handoffMs, g_asioMatch,
        AsioMatchAny(g_asioMatch) ? " (auto: first usable driver)" : "",
        g_fakeClient, g_outMode == 0 ? "asio" : (g_outMode == 1 ? "excl" : "shared"));
    Log("MODE: %s", g_fakeClient ? "v13 takeover (no engine buffer)" : "v9 buffer trim only");
}

/* --------------------- boot device watchdog --------------------- */

static void DeviceProbeOnce(const char* phase)
{
    IMMDeviceEnumerator* en = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { Log("[dev] %s enumerator FAIL 0x%08X", phase, (unsigned)hr); return; }

    for (int role = 0; role < 2; role++)
    {
        IMMDevice* dev = NULL;
        hr = en->GetDefaultAudioEndpoint(eRender, role == 0 ? eConsole : eMultimedia, &dev);
        if (SUCCEEDED(hr) && dev)
        {
            LPWSTR id = NULL;
            if (SUCCEEDED(dev->GetId(&id)) && id)
            {
                char ida[512] = { 0 };
                WideCharToMultiByte(CP_ACP, 0, id, -1, ida, sizeof(ida) - 1, NULL, NULL);
                Log("[dev] %s %s = %s", phase, role == 0 ? "console" : "multimedia", ida);
                CoTaskMemFree(id);
            }
            dev->Release();
        }
        else
            Log("[dev] %s %s GetDefaultAudioEndpoint FAIL hr=0x%08X", phase,
                role == 0 ? "console" : "multimedia", (unsigned)hr);
    }

    IMMDeviceCollection* col = NULL;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (SUCCEEDED(hr) && col)
    {
        UINT32 cnt = 0; col->GetCount(&cnt);
        Log("[dev] %s active_render_devices=%u", phase, cnt);
        col->Release();
    }
    en->Release();
}

/* A dead output stream used to mean permanent silence. WoThread now bails out
   with state 4; this brings the stream back - the usual cause is Windows
   switching the default endpoint when headphones are plugged/unplugged. The
   first few attempts are fast (3 s apart); if the device stays gone the retry
   slows to once per 30 s and keeps trying, so audio comes back on its own once
   the endpoint returns - instead of giving up after 5 tries and staying silent
   until the game is restarted. */
static volatile LONG g_woRetry = 0;
static volatile ULONGLONG g_woNextTry = 0;

static void WoWatchdog(void)
{
    if (!g_fakeClient) return;                  /* legacy tap path reclaims WASAPI itself */
    if (g_woState != 4) { g_woRetry = 0; g_woNextTry = 0; return; }
    if (GetTickCount64() < g_woNextTry) return;

    if (g_woRetry < 5) g_woNextTry = GetTickCount64() + 3000;    /* fast phase  */
    else               g_woNextTry = GetTickCount64() + 30000;   /* slow phase  */

    g_woRetry++;
    Log("[wo] watchdog: output backend is down -> restarting shared (attempt %ld)%s",
        (long)g_woRetry, g_woRetry > 5 ? " [slow]" : "");
    if (!WoStart(0)) Log("[wo] !! watchdog restart failed");
}

static DWORD WINAPI WatchdogThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* no hard iteration cap: this used to stop after an hour, after which any
       later anomaly went unrecorded */
    for (long long i = 1; ; i++)
    {
        Sleep(1000);

        if (i <= 3)                 /* boot window only; it used to log for 20 s */
            DeviceProbeOnce("probe");

        /* Deferred stale-mute cleanup. With the fake client there is no real
           stream1 session, so the cleanup at Initialize() time finds nothing;
           by now stream2 (or the output backend) has created this process's
           session, which is the one Windows remembers as muted. */
        if (i == 5 && g_fakeClient && g_sessionActive)
        {
            if (MuteSession(FALSE))
                Log("[fix] cleared stale session-mute residue (deferred)");
        }

        WoWatchdog();

        if (i <= 30 || (i % 60) == 0)
            Log("[hb] up=%llds init=%ld rc=%ld | asio en=%d st=%ld cbs=%ld taps=%lld und=%ld drops=%lld zeroTap=%ld zeroed=%lld kicked=%ld",
                i, g_initCalls, g_rcCalls,
                g_asioEn, g_asioState, g_asoCbs,
                (long long)g_tapFrames, g_underruns, (long long)g_ringDrops,
                g_zeroTap, (long long)g_zeroed, g_kicked);
    }
}

/* ------------------- process-detach trap (diagnostic) ------------------- */

static void Hex64(char* out, ULONGLONG v)
{
    static const char* H = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) { out[i] = H[v & 0xF]; v >>= 4; }
    out[16] = '\0';
}

static void WriteDetachReport(void)
{
    HANDLE f = CreateFileA(g_detachPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st; GetLocalTime(&st);
    char line[768];
    /* wsprintfA does not understand "%lld" (it only knows "%l" and "%I64");
       "%lld" used to read the 64-bit frame counters as 32-bit, so taps/zeroed
       printed garbage. Use the Win32-only "%I64d" (keeps this DllMain path free
       of CRT dependencies). */
    int n = wsprintfA(line,
        "=== DETACH %04u-%02u-%02u %02u:%02u:%02u.%03u  init=%ld rc=%ld | asio st=%ld cbs=%ld taps=%I64d und=%ld zeroTap=%ld zeroed=%I64d kicked=%ld ===\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        g_initCalls, g_rcCalls, g_asioState, g_asoCbs, (long long)g_tapFrames,
        g_underruns, g_zeroTap, (long long)g_zeroed, g_kicked);
    DWORD w = 0;
    if (n > 0) WriteFile(f, line, (DWORD)n, &w, NULL);

    typedef USHORT (WINAPI *PFN_RTLSTACK)(ULONG, ULONG, PVOID*, PULONG);
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    PFN_RTLSTACK pStack = hNtdll ? (PFN_RTLSTACK)GetProcAddress(hNtdll, "RtlCaptureStackBackTrace") : NULL;
    if (pStack)
    {
        void* frames[48];
        USHORT cnt = pStack(0, 48, frames, NULL);
        for (USHORT i = 0; i < cnt; i++)
        {
            MEMORY_BASIC_INFORMATION mbi;
            char mname[MAX_PATH] = "?";
            char offbuf[20] = "0";
            if (VirtualQuery(frames[i], &mbi, sizeof(mbi)) && mbi.AllocationBase)
            {
                if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, mname, MAX_PATH) == 0)
                    wsprintfA(mname, "mod_%p", mbi.AllocationBase);
                Hex64(offbuf, (ULONGLONG)((PBYTE)frames[i] - (PBYTE)mbi.AllocationBase));
            }
            n = wsprintfA(line, "  #%02u %p  %s+0x%s\r\n", i, frames[i], mname, offbuf);
            if (n > 0) WriteFile(f, line, (DWORD)n, &w, NULL);
        }
    }
    CloseHandle(f);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL; (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_DETACH)
    {
        /* timeBeginPeriod(1) raises the timer resolution for the WHOLE system,
           so it has to be given back even when the process is going away
           (audit N11). */
        if (InterlockedExchange(&g_timerPeriod, 0)) timeEndPeriod(1);
        if (g_sessionActive && g_detachPath[0]) WriteDetachReport();
    }
    return TRUE;
}

/* ------------------------------- PreInit ------------------------------- */

extern "C" __declspec(dllexport) void PreInit(void)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&PreInit, &self);
    if (self && GetModuleFileNameA(self, g_dir, MAX_PATH))
    {
        char* slash = strrchr(g_dir, '\\');
        if (slash) *(slash + 1) = '\0';
    }

    char off[MAX_PATH], lp[MAX_PATH];
    MakePath(off, sizeof(off), "audio_hook.off");
    MakePath(lp, sizeof(lp), "audio_hook.log");
    MakePath(g_detachPath, sizeof(g_detachPath), "audio_hook_detach.log");

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpc0);
    if (g_qpcFreq.QuadPart <= 0) g_qpcFreq.QuadPart = 1;   /* MsNow() must never divide by 0 */

    /* keep exactly one previous log: the run that crashed is the one worth
       reading, and "wb" used to wipe it on every start */
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(lp, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeHigh == 0 && fa.nFileSizeLow > (4u << 20))
        {
            char old[MAX_PATH];
            MakePath(old, sizeof(old), "audio_hook.log.1");
            DeleteFileA(old);
            MoveFileA(lp, old);
        }
    }
    fopen_s(&g_log, lp, "ab");
    if (!g_log) return;

    Log("============================================================");
    Log(" audio_hook v13.7 (fake stream1 client: no engine buffer, device free)");
    Log(" build %s %s", __DATE__, __TIME__);
    Log(" hooks: [3]Initialize [14]GetService [13]SetEventHandle(fake only)");
    Log("============================================================");

    if (GetFileAttributesA(off) != INVALID_FILE_ATTRIBUTES)
    {
        Log("audio_hook.off present -> doing nothing.");
        return;
    }

    /* Only when we are really running: this raises the timer resolution for the
       whole system, and DllMain gives it back at detach (audit N11). Set the
       flag only when the call actually succeeded, so a failed request is not
       undone at detach (audit R8). */
    g_timerPeriod = (timeBeginPeriod(1) == TIMERR_NOERROR) ? 1 : 0;

    g_sessionActive = 1;
    ReadConfig();

    /* ring memory (never allocated again; callback path stays allocation-free).
       It must exist whenever a consumer or the fake client is active, NOT only
       when ASIO is enabled: fake_client=1 + asio=0 used to write through a NULL
       ring pointer and crash on the first ReleaseBuffer. */
    g_ring = (short*)malloc((size_t)RING_CAP * 2 * sizeof(short));
    if (!g_ring)
    {
        Log("!! ring alloc failed -> takeover and ASIO disabled (v9 path only)");
        g_fakeClient = 0;
        g_asioEn = 0;
        g_outMode = 0;
    }

    /* prefill the format the fake client reports: GetMixFormat can be called
       before Initialize, and a zeroed WAVEFORMATEX (nSamplesPerSec = 0) makes
       a caller divide by zero */
    memset(&g_fake.fmt, 0, sizeof(g_fake.fmt));
    g_fake.fmt.wFormatTag      = WAVE_FORMAT_PCM;
    g_fake.fmt.nChannels       = 2;
    g_fake.fmt.nSamplesPerSec  = 44100;
    g_fake.fmt.wBitsPerSample  = 16;
    g_fake.fmt.nBlockAlign     = 4;
    g_fake.fmt.nAvgBytesPerSec = 44100 * 4;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioClient* c = NULL;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { Log("!! enumerator failed 0x%08X - NOT hooked", (unsigned)hr); return; }

    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) hr = en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev);
    if (FAILED(hr) || !dev) { Log("!! no default render endpoint - NOT hooked"); en->Release(); return; }

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&c);
    if (FAILED(hr) || !c) { Log("!! activate failed 0x%08X - NOT hooked", (unsigned)hr); dev->Release(); en->Release(); return; }

    void** vt = *(void***)c;
    Log("vtable=0x%llX  [3]=0x%llX  [14]=0x%llX",
        (unsigned long long)(UINT_PTR)vt,
        (unsigned long long)(UINT_PTR)vt[3],
        (unsigned long long)(UINT_PTR)vt[14]);

    BOOL ok3  = PatchSlot(vt,  3, (void*)&Hook_Initialize, (void**)&g_orig);
    BOOL ok14 = PatchSlot(vt, 14, (void*)&Hook_GetService, (void**)&oGS);
    BOOL ok13 = g_fakeClient ? PatchSlot(vt, 13, (void*)&Hook_SetEventHandle, (void**)&oSEH) : FALSE;
    Log("hooked: [3]=%s (orig=0x%llX)  [14]=%s  [13]=%s (fake_client=%d)",
        ok3 ? "OK" : "FAIL", (unsigned long long)(UINT_PTR)g_orig,
        ok14 ? "OK" : "FAIL", ok13 ? "OK" : "skip", g_fakeClient);
    if (!ok3 || !ok14)
        Log("!! core hooks failed - the mod cannot do anything on this process");

    /* diagnostic: listen to device notifications */
    g_notifier = new (std::nothrow) NotifyLogger();
    {
        HRESULT nr = g_notifier ? en->RegisterEndpointNotificationCallback(g_notifier) : E_OUTOFMEMORY;
        Log("notify client registered hr=0x%08X", (unsigned)nr);
    }

    c->Release();
    dev->Release();
    en->Release();

    HANDLE th = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    if (th) CloseHandle(th);

    Log("ready. waiting for the game to create its audio streams...");
}
