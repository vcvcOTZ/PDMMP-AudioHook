/*
 * audio_hook.cpp -- PDMMP audio hook (v11.3 build, 2026-10-17, CURRENT SOURCE)
 * ---------------------------------------------------------------------------
 * Base behaviour (always active):
 *   stream1 (game audio)  : drop RATEADJUST -> 44.5ms buffer becomes 22.0ms
 *   stream2 (movie audio) : duration 666664 -> 166666 (66.7ms -> 22.0ms)
 *
 * ASIO output bridge (enabled with "asio = 1" in audio_hook.txt):
 *   The game keeps its normal WASAPI shared streams; we TAP the PCM that CRI
 *   writes into stream1 (at IAudioRenderClient::ReleaseBuffer), push it
 *   through a lock-free ring buffer, and play it out through the ASIO driver.
 *
 *   Key fixes (v11 -> v11.2):
 *     - de-interleave fix: each ASIO channel takes src[i*2 + ch] (v10d wrote
 *       the interleaved stream into every channel -> octave down + shredded)
 *     - handoff redesign "tap-and-zero": copy then zero the game buffer so the
 *       WASAPI path goes silent naturally, then start ASIO at the exact frame
 *       the game stopped on (no SetMute, no double audio, seamless)
 *     - kick self-healing: DEVICE_INVALIDATED -> rollback, WASAPI audible again
 *     - stale session-mute cleanup on every boot (v10d's SetMute residue)
 *     - v11.2: EXCLUSIVE-mode experiment code fully removed (it was proven
 *       non-realtime on this device and permanently rejected).
 *
 *   v11.3 (robustness):
 *     - tap-steady wait now has a 30 s timeout (was an infinite loop): if the
 *       game audio never reaches real-time, the bridge gives up and stays on
 *       the WASAPI path instead of leaking a stuck worker thread
 *     - removed all logging from the ASIO realtime callback (file I/O on the
 *       callback thread could stall it and worsen underruns)
 *
 *   Portability: asio_driver = name substring ("*" = first driver);
 *   sample-rate guard refuses non-44100 (never pitch-shifts).
 *
 * Patched vtable slots (data layer only, no code bytes anywhere):
 *   IAudioClient [3] Initialize, [14] GetService
 *   IAudioRenderClient [3]/[4] (armed on first GetService)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <intrin.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

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

/* ---- v11 config ---- */
static int g_asioEn     = 0;    /* 1 = ASIO bridge enabled            */
static int g_asioBlock  = 64;   /* ASIO block size (16..1024 step 16) */
static int g_muteMode   = 1;    /* 1 = tap-and-zero (v11), 0 = SetMute */
static int g_handoffMs  = 0;    /* 0 = auto (measured buffer + 2ms)   */
static char g_asioMatch[96] = "*";               /* "" or "*" = any driver */

static volatile LONG g_initCalls = 0;
static volatile LONG g_rcCalls   = 0;

static LARGE_INTEGER g_qpcFreq, g_qpc0;

static double MsNow(void)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
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
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
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
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &old)) return FALSE;
    if (outOrig) *outOrig = vtbl[idx];
    vtbl[idx] = newFn;
    DWORD tmp = 0;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[idx], sizeof(void*));
    return TRUE;
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
static volatile LONG   g_muted     = 0;   /* legacy SetMute path state */
static volatile LONG   g_zeroTap   = 0;   /* v11: zero the game buffer after copy */
static volatile LONG   g_kicked    = 0;   /* stream1 died while bridging */
static volatile LONG   g_asioStartReq = 0;
static volatile LONG   g_s1BufFrames  = 0; /* measured stream1 buffer (frames) */

#define S1_MAX 8
static volatile LONG g_s1ClientN = 0;
static void* g_s1Clients[S1_MAX] = { 0 };

struct RcSlot { void* rc; BYTE* ptr; UINT32 frames; };
#define S1RC_MAX 16
static volatile LONG g_s1RcN = 0;
static RcSlot g_rcSlots[S1RC_MAX] = { 0 };

static int IsS1Client(void* client)
{
    LONG n = g_s1ClientN; if (n > S1_MAX) n = S1_MAX;
    for (int i = 0; i < n; i++) if (g_s1Clients[i] == client) return 1;
    return 0;
}

static void AddS1Client(void* client)
{
    LONG n = InterlockedIncrement(&g_s1ClientN);
    if (n <= S1_MAX) g_s1Clients[n - 1] = client;
    Log("[asio] stream1 client registered: %p (total %ld)", client, n);
}

static int RcSlotFind(void* rc)
{
    LONG n = g_s1RcN; if (n > S1RC_MAX) n = S1RC_MAX;
    for (int i = 0; i < n; i++) if (g_rcSlots[i].rc == rc) return i;
    return -1;
}

static void AddS1Rc(void* rc)
{
    if (RcSlotFind(rc) >= 0) return;
    LONG n = InterlockedIncrement(&g_s1RcN);
    if (n <= S1RC_MAX) { g_rcSlots[n - 1].rc = rc; g_rcSlots[n - 1].ptr = NULL; g_rcSlots[n - 1].frames = 0; }
    Log("[asio] stream1 render-client registered: %p (total %ld)", rc, n);
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
        Log("[notify] t=%.2fs DEFAULT_CHANGED flow=%d role=%d", MsNow(), (int)f, (int)r);
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
        Log("[notify] t=%.2fs PROPCHANGED key={%s,%lu} dev='%s'", MsNow(), gb, (unsigned long)k.pid, brace ? brace : b);
        return S_OK;
    }
};

static NotifyLogger* g_notifier = NULL;

/* ---------------- lock-free ring (int16 stereo frames) ---------------- */

#define RING_CAP 65536            /* frames (1.49s @44.1k); power of two */
static short* g_ring = NULL;
static volatile LONG64 g_rw = 0, g_rr = 0;

static unsigned RingAvail(void)
{
    LONG64 w = g_rw, r = g_rr;
    if (w < r) return 0;
    return (unsigned)(w - r);
}

static void RingWrite(const short* src, unsigned frames)
{
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

/*  v11 FIX: src is INTERLEAVED stereo (L0,R0,L1,R1,...). Each ASIO channel
 *  must take src[i*2 + ch], not src[i]. The v10 code copied the interleaved
 *  stream straight into each channel: every 32 frames got stretched into 64
 *  samples (one octave down + half the content dropped) = the reported
 *  "shredded + pitch down" audio.  */
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
            int32_t v = (i < got) ? (((int32_t)src[i * 2 + ch]) << 16) : 0;
            o[i * 3] = (unsigned char)v; o[i * 3 + 1] = (unsigned char)(v >> 8); o[i * 3 + 2] = (unsigned char)(v >> 16);
        }
    } break;
    case ASIOSTFloat32LSB: {
        float* o = (float*)p;
        for (i = 0; i < got; i++) o[i] = src[i * 2 + ch] / 32768.0f;
        for (; i < total; i++) o[i] = 0.0f;
    } break;
    default:
        memset(p, 0, (size_t)total * 4);
        break;
    }
}

static void AsioFill(long index)
{
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
        if (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo ||
            value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
            value == kAsioResetRequest)
            return 1L;
        return 0L;
    case kAsioEngineVersion:    return 2L;
    case kAsioSupportsTimeInfo: return 1L;
    case kAsioSupportsTimeCode: return 0L;
    case kAsioOverload:         Log("[asio] !! driver overload message"); return 0L;
    default:                    return 0L;
    }
}

/* ---------------- legacy session mute (mute_mode = 0) ---------------- */

static BOOL MuteSession(BOOL mute)
{
    if (g_s1ClientN < 1) return FALSE;
    IAudioClient* c = (IAudioClient*)g_s1Clients[0];
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = oGS ? oGS(c, __uuidof(ISimpleAudioVolume), (void**)&vol) : E_FAIL;
    if (FAILED(hr) || !vol) { Log("[asio] GetService(ISimpleAudioVolume) hr=0x%08X", (unsigned)hr); return FALSE; }
    hr = vol->SetMute(mute, NULL);
    vol->Release();
    if (FAILED(hr)) { Log("[asio] SetMute(%d) hr=0x%08X", (int)mute, (unsigned)hr); return FALSE; }
    return TRUE;
}

/* ---------------- driver load / prepare / teardown ---------------- */

static BOOL AsioLoadDriver(const char* match)
{
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hk) != ERROR_SUCCESS)
    {
        Log("[asio] HKLM\\SOFTWARE\\ASIO not found");
        return FALSE;
    }
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
        if (!clsid[0]) continue;
        if (match && match[0] && strcmp(match, "*") != 0 && !strstr(nm, match)) continue;
        wchar_t wc[256];
        MultiByteToWideChar(CP_ACP, 0, clsid, -1, wc, 256);
        CLSID id;
        if (FAILED(CLSIDFromString(wc, &id))) continue;
        HRESULT hr = CoCreateInstance(id, NULL, CLSCTX_INPROC_SERVER, id, (void**)&g_asio);
        Log("[asio] CoCreateInstance('%s') -> hr=0x%08X", nm, (unsigned)hr);
        if (SUCCEEDED(hr) && g_asio) { RegCloseKey(hk); return TRUE; }
    }
    RegCloseKey(hk);
    return FALSE;
}

static long AsioChooseBlock(long want, long mn, long mx, long pr, long gr)
{
    long block = want > 0 ? want : pr;
    if (gr < 0) { long b = mn; while (b < block && b < mx) b <<= 1; block = b; }
    else if (gr > 0) block = mn + ((block - mn + gr - 1) / gr) * gr;
    if (block < mn) block = mn;
    if (block > mx) block = mx;
    return block;
}

/* Create the ASIO engine but DO NOT start it yet.
 * Returns TRUE when ready to be started (buffers created, silence pre-filled). */
static BOOL AsioPrepare(void)
{
    if (!AsioLoadDriver(g_asioMatch)) return FALSE;

    if (g_asio->init((void*)GetDesktopWindow()) != ASIOTrue)
    {
        char e[124] = { 0 };
        g_asio->getErrorMessage(e);
        Log("[asio] init failed: %s", e);
        return FALSE;
    }
    char nm[64] = { 0 };
    g_asio->getDriverName(nm);
    long nin = 0, nout = 0;
    if (g_asio->getChannels(&nin, &nout) != ASE_OK || nout < 2)
    {
        Log("[asio] channels in=%ld out=%ld - need >=2 outputs", nin, nout);
        return FALSE;
    }
    long mn = 0, mx = 0, pr = 0, gr = 0;
    if (g_asio->getBufferSize(&mn, &mx, &pr, &gr) != ASE_OK) { Log("[asio] getBufferSize failed"); return FALSE; }
    long block = AsioChooseBlock(g_asioBlock, mn, mx, pr, gr);

    ASIOSampleRate sr = 0;
    g_asio->getSampleRate(&sr);
    Log("[asio] driver='%s' block=%ld rate=%g (want block=%d mute_mode=%d driver_match='%s')",
        nm, block, sr, g_asioBlock, g_muteMode, g_asioMatch);

    /* v11 portability guard: never feed 44.1k data into a non-44.1k engine */
    if (fabs(sr - 44100.0) > 1.0)
    {
        Log("[asio] !! sample rate %g != 44100 - refusing to bridge (avoids pitch shift)", sr);
        return FALSE;
    }

    if (g_asio->outputReady() == ASE_OK) g_apostOut = 1;

    memset(g_abufs, 0, sizeof(g_abufs));
    for (int c = 0; c < 2; c++) { g_abufs[c].isInput = ASIOFalse; g_abufs[c].channelNum = c; }

    memset(&g_acbs, 0, sizeof(g_acbs));
    g_acbs.bufferSwitch = cbBufferSwitch;
    g_acbs.sampleRateDidChange = cbSampleRateDidChange;
    g_acbs.asioMessage = cbAsioMessage;
    g_acbs.bufferSwitchTimeInfo = cbBufferSwitchTimeInfo;

    if (g_asio->createBuffers(g_abufs, 2, block, &g_acbs) != ASE_OK) { Log("[asio] createBuffers failed"); return FALSE; }
    g_ablock = block;

    for (int c = 0; c < 2; c++)
    {
        memset(&g_aci[c], 0, sizeof(ASIOChannelInfo));
        g_aci[c].channel = c;
        g_aci[c].isInput = ASIOFalse;
        g_asio->getChannelInfo(&g_aci[c]);
    }
    Log("[asio] out ch0 type=%ld '%s' | ch1 type=%ld '%s'", g_aci[0].type, g_aci[0].name, g_aci[1].type, g_aci[1].name);

    long il = 0, ol = 0;
    if (g_asio->getLatencies(&il, &ol) == ASE_OK)
        Log("[asio] latencies in=%ld out=%ld frames (%.2f / %.2f ms)", il, ol, il * 1000.0 / 44100.0, ol * 1000.0 / 44100.0);

    if (!g_tmp) g_tmp = (short*)malloc((size_t)block * 2 * sizeof(short));
    if (!g_tmp) { Log("[asio] staging alloc failed"); return FALSE; }

    /* pre-fill both halves with silence (spec: at least index 1) */
    for (long h = 0; h < 2; h++)
        for (int c = 0; c < 2; c++)
            WriteSamples(g_abufs[c].buffers[h], g_aci[c].type, c, g_tmp, 0, (unsigned)block);

    return TRUE;
}

static void AsioTeardown(void)
{
    if (!g_asio) return;
    g_asio->stop();
    g_asio->disposeBuffers();
    g_asio->Release();
    g_asio = NULL;
    g_apostOut = 0;
}

/* rollback: make the WASAPI path audible again, drop ASIO, stop trying */
static void AsioRollback(const char* why)
{
    Log("[asio] ROLLBACK: %s", why);
    InterlockedExchange(&g_zeroTap, 0);              /* game audio returns */
    InterlockedExchange(&g_tapOn, 0);                /* stop copying to the ring */
    if (g_muted) { MuteSession(FALSE); InterlockedExchange(&g_muted, 0); }
    AsioTeardown();
    InterlockedExchange(&g_asioState, 4);
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
        if (d >= 3900) { good++; if (good >= 3) return TRUE; }   /* >=88% real-time */
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
    /* IMPORTANT: the Focusrite ASIO driver's COM server only answers the
       driver-clsid IID from an STA apartment (MTA returns E_NOINTERFACE). */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InterlockedExchange(&g_asioState, 2);

    Log("[asio] v11 worker up - waiting for the game's audio tap to run steadily");
    if (!WaitForTapSteady())
    {
        Log("[asio] !! tap never reached steady real-time (30s) - staying on WASAPI path");
        InterlockedExchange(&g_asioState, 4);
        return 0;
    }
    Log("[handoff] tap steady (taps=%lld)", (long long)g_tapFrames);

    if (!AsioPrepare())
    {
        Log("[asio] !! prepare failed - staying on the WASAPI path (v9 behaviour)");
        InterlockedExchange(&g_asioState, 4);
        return 0;
    }

    /* handoff: point the reader at the newest write (the frame the game will
       stop on), then silence the game side and let its engine queue drain. */
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

    if (g_asio->start() != ASE_OK)
    {
        Log("[asio] !! start failed");
        AsioRollback("start failed");
        return 0;
    }

    InterlockedExchange(&g_asioState, 3);
    Log("[handoff] ASIO started - output resumed seamlessly (block=%ld). monitoring...", g_ablock);

    /* monitor: kick / stall / runaway underruns */
    int stall = 0, undFail = 0;
    LONG last = g_asoCbs, lastUnd = g_underruns;
    for (int i = 1; i <= 86400; i++)
    {
        Sleep(1000);
        if (g_kicked)
        {
            AsioRollback("stream1 was invalidated (kick) - game audio returns via WASAPI");
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
        if (i <= 20 || (i % 60) == 0)
            Log("[asio] t=%ds cbs=%ld und=%ld avail=%u zeroTap=%ld muted=%ld",
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
        if (si >= 0) { g_rcSlots[si].ptr = *ppData; g_rcSlots[si].frames = frames; }
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
    if (g_tapOn && frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
    {
        int si = RcSlotFind(self);
        if (si >= 0 && g_rcSlots[si].ptr && g_rcSlots[si].frames > 0)
        {
            unsigned n = frames;
            if (n > g_rcSlots[si].frames) n = g_rcSlots[si].frames;
            RingWrite((const short*)g_rcSlots[si].ptr, n);
            InterlockedExchangeAdd64(&g_tapFrames, (LONG64)n);
            if (g_zeroTap)
            {
                memset(g_rcSlots[si].ptr, 0, (size_t)n * 4);
                InterlockedExchangeAdd64(&g_zeroed, (LONG64)n);
            }
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

static HRESULT STDMETHODCALLTYPE Hook_GetService(IAudioClient* self, REFIID riid, void** ppv)
{
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

        if (!g_rcPatched)
        {
            void** vt = *(void***)*ppv;
            if (vt && IsExecPtr(vt[3]) && IsExecPtr(vt[4]))
            {
                if (PatchSlot(vt, 3, (void*)&Hook_RC_GetBuffer, (void**)&oRCGB) &&
                    PatchSlot(vt, 4, (void*)&Hook_RC_ReleaseBuffer, (void**)&oRCRB))
                {
                    g_rcPatched = 1;
                    Log("[gs] RC vtable patched (tap + zero armed)");
                }
                else Log("[gs] !! RC patch failed");
            }
            else Log("[gs] !! RC slots not executable - skipped");
        }
    }
    return hr;
}

/* ------------- Initialize: param rewrite ------------- */

static HRESULT STDMETHODCALLTYPE Hook_Initialize(IAudioClient* self, AUDCLNT_SHAREMODE shareMode,
    DWORD flags, REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
    const WAVEFORMATEX* pFormat, LPCGUID audioSessionGuid)
{
    int fixed1 = 0, fixed2 = 0;

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

            flags = 0x00040000u;
        }
        else if (flags == 0 && pFormat->wBitsPerSample == 32 && hnsBufferDuration >= 600000)
        {
            hnsBufferDuration = 166666;
            fixed2 = 1;
        }
    }

    HRESULT hr = g_orig(self, shareMode, flags, hnsBufferDuration, hnsPeriodicity, pFormat, audioSessionGuid);

    UINT32 bufFrames = 0;
    REFERENCE_TIME sLat = 0;
    if (self != NULL && SUCCEEDED(hr) && (fixed1 || fixed2))
    {
        if (FAILED(self->GetBufferSize(&bufFrames))) bufFrames = 0;
        if (FAILED(self->GetStreamLatency(&sLat))) sLat = 0;
    }

    if (fixed1) Log("stream1 FIX: flags 0x00140000 -> 0x00040000 (RATEADJUST removed)  hr=0x%08X %s",
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
           mod is disabled". Clear it every boot; harmless when already clean. */
        if (MuteSession(FALSE))
            Log("[fix] cleared stale session-mute residue (v10d era)");

        if (g_asioEn)
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
    if (fopen_s(&f, path, "rb") != 0 || !f) { Log("no audio_hook.txt -> asio=0"); return; }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    const char* p = buf;
    while (p && *p)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;
        if (*p == '#') { const char* nl = strchr(p, '\n'); p = nl ? nl + 1 : NULL; continue; }
        const char* eq = strchr(p, '=');
        if (!eq) break;
        char key[64] = { 0 };
        int k = 0;
        for (const char* q = p; q < eq && k < 63; q++)
            if (*q != ' ' && *q != '\t') key[k++] = *q;
        const char* v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (_stricmp(key, "asio_driver") == 0)
        {
            int j = 0;
            while (v[j] && v[j] != '\r' && v[j] != '\n' && j < 95) { g_asioMatch[j] = v[j]; j++; }
            while (j > 0 && (g_asioMatch[j - 1] == ' ' || g_asioMatch[j - 1] == '\t')) j--;
            g_asioMatch[j] = '\0';
        }
        else
        {
            int val = atoi(v);
            if (_stricmp(key, "asio") == 0) g_asioEn = val;
            else if (_stricmp(key, "block") == 0) g_asioBlock = val;
            else if (_stricmp(key, "mute_mode") == 0) g_muteMode = val;
            else if (_stricmp(key, "handoff_ms") == 0) g_handoffMs = val;
        }
        const char* nl = strchr(eq, '\n');
        p = nl ? nl + 1 : NULL;
    }
    if (g_asioBlock < 16) g_asioBlock = 16;
    if (g_asioBlock > 1024) g_asioBlock = 1024;
    if (g_handoffMs < 0) g_handoffMs = 0;
    if (g_handoffMs > 150) g_handoffMs = 150;
    Log("config: asio=%d block=%d | mute_mode=%d handoff_ms=%d | driver='%s'",
        g_asioEn, g_asioBlock, g_muteMode, g_handoffMs, g_asioMatch);
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

static DWORD WINAPI WatchdogThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    for (int i = 1; i <= 3600; i++)
    {
        Sleep(1000);

        if (i <= 20)
            DeviceProbeOnce("probe");

        if (i <= 30 || (i % 60) == 0)
            Log("[hb] up=%ds init=%ld rc=%ld | asio en=%d st=%ld cbs=%ld taps=%lld und=%ld drops=%lld zeroTap=%ld zeroed=%lld kicked=%ld",
                i, g_initCalls, g_rcCalls,
                g_asioEn, g_asioState, g_asoCbs,
                (long long)g_tapFrames, g_underruns, (long long)g_ringDrops,
                g_zeroTap, (long long)g_zeroed, g_kicked);
    }
    return 0;
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
    int n = wsprintfA(line,
        "=== DETACH %04u-%02u-%02u %02u:%02u:%02u.%03u  init=%ld rc=%ld | asio st=%ld cbs=%ld taps=%lld und=%ld zeroTap=%ld zeroed=%lld kicked=%ld ===\r\n",
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
    if (fdwReason == DLL_PROCESS_DETACH && g_sessionActive && g_detachPath[0])
        WriteDetachReport();
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

    fopen_s(&g_log, lp, "wb");
    if (!g_log) return;

    Log("============================================================");
    Log(" audio_hook v11.3 (v11.2 + robustness: tap-steady timeout, no I/O in realtime cb)");
    Log(" build %s %s", __DATE__, __TIME__);
    Log(" hooks: [3]Initialize [14]GetService (+RC lazy)");
    Log("============================================================");

    if (GetFileAttributesA(off) != INVALID_FILE_ATTRIBUTES)
    {
        Log("audio_hook.off present -> doing nothing.");
        return;
    }

    g_sessionActive = 1;
    ReadConfig();

    /* ring memory (never allocated again; callback path stays allocation-free) */
    if (g_asioEn)
    {
        g_ring = (short*)malloc((size_t)RING_CAP * 2 * sizeof(short));
        if (!g_ring) { Log("[asio] !! ring alloc failed - ASIO disabled"); g_asioEn = 0; }
    }

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
    Log("vtable=%p  [3]=%p  [14]=%p", vt, vt[3], vt[14]);

    BOOL ok3  = PatchSlot(vt,  3, (void*)&Hook_Initialize, (void**)&g_orig);
    BOOL ok14 = PatchSlot(vt, 14, (void*)&Hook_GetService, (void**)&oGS);
    Log("hooked: [3]=%s (orig=%p)  [14]=%s",
        ok3 ? "OK" : "FAIL", g_orig, ok14 ? "OK" : "FAIL");

    /* diagnostic: listen to device notifications */
    g_notifier = new NotifyLogger();
    {
        HRESULT nr = en->RegisterEndpointNotificationCallback(g_notifier);
        Log("notify client registered hr=0x%08X", (unsigned)nr);
    }

    c->Release();
    dev->Release();
    en->Release();

    HANDLE th = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    if (th) CloseHandle(th);

    Log("ready. waiting for the game to create its audio streams...");
}
