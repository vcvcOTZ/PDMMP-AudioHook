/*
 * audio_hook.cpp -- PDMMP audio hook (v14.0 build, 2026-10-20, CURRENT SOURCE)
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
#include <avrt.h>      /* MMCSS: AvSetMmThreadCharacteristics (exclusive writer thread) */
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
#pragma comment(lib, "avrt.lib")

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
static volatile LONG g_woExclusive = 0; /* WoThread drives the exclusive backend (enables MMCSS) */
static int g_exclPeriodMs = 0;          /* excl_period_ms: exclusive buffer request (ms); 0 = 20 ms */
static int g_exclTargetMs = 12;         /* excl_target_ms: exclusive PUSH fill target (ms) */
static int g_exclTargetCfg = 12;        /* the configured value: the runtime adaptation
                                           may only wander ABOVE it, never below */
static volatile LONG g_exclTargetFr = 0; /* the same in OUTPUT frames (set in WoInit) */
static volatile LONG g_exclFloorFr  = 0; /* device-fill floor: never leave the device thin */
static volatile LONG g_woRate = 44100;   /* negotiated device rate (output frames/s) */
static volatile LONG g_woUnder = 0;
static volatile LONG   g_woCbs   = 0;
static volatile LONG64 g_woOut = 0;   /* frames handed to the output stream */
static volatile LONG   g_woPad = 0;   /* last GetCurrentPadding() reading: frames the
                                         device still holds and has not played yet.
                                         Read by FakeClock::GetPosition (P1-1) - via a
                                         counter, never by touching g_woClient from
                                         CRI's thread while a teardown may be freeing it */
/* Source frames per OUTPUT frame (44100 / negotiated device rate); 1.0 on a
   44.1 kHz endpoint. Declared up here because FakeClock::GetPosition (P1-1)
   converts the device fill into source frames with it. */
static double g_rsStep = 1.0;
static volatile LONG g_fakeGbFail = 0;   /* CRI GetBuffer() rejected (queue full) */
static volatile LONG g_timerPeriod = 0;  /* timeBeginPeriod(1) is in effect */

static volatile LONG g_initCalls = 0;
static volatile LONG g_rcCalls   = 0;
static volatile LONG g_rcFail    = 0;   /* throttles the render-client failure logs */

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

static volatile LONG   g_logPending = 0;   /* lines written since the last flush (P2-1) */
static volatile LONG64 g_logFlushAt = 0;   /* tick of the last flush */

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
    if (n > 0)
    {
        fwrite(line, 1, (size_t)n, g_log);
        /* Flush at most every 200 ms or 32 lines (P2-1). fflush used to run on
           EVERY line - more cost than the write itself - and a device-
           notification storm (23 PROPCHANGED lines per event) turned that into
           thousands of flushes a second, on disk, contending with the mixing
           thread. Buffered like this a crash loses at most ~200 ms of lines. */
        LONG64 lnow = (LONG64)GetTickCount64();
        if (InterlockedIncrement(&g_logPending) >= 32 || lnow - g_logFlushAt >= 200)
        {
            InterlockedExchange(&g_logPending, 0);
            InterlockedExchange64(&g_logFlushAt, lnow);
            fflush(g_log);
        }
    }
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

/* ---- patch journal (P0-2) ---------------------------------------------------
   Every successful forward patch is recorded so DllMain can put the ORIGINAL
   pointer back when the module is unloaded (DMM hot-reload, "close mod", an
   anti-cheat tear-down). Without it the game keeps three IAudioClient slots and
   two IAudioRenderClient slots pointing into memory that is about to be
   unmapped: the next Initialize / GetService / device event jumps into nothing.
   Rollback calls (outOrig == NULL) are not recorded - they are already writing
   the original back. */
struct PatchRec { void** vt; int idx; void* orig; };
#define PATCHREC_MAX 32
static PatchRec g_patchRec[PATCHREC_MAX];
static volatile LONG g_patchRecN = 0;

static void RecordPatch(void** vt, int idx, void* orig)
{
    LONG k = g_patchRecN;
    if (k >= PATCHREC_MAX) return;
    g_patchRec[k].vt = vt;
    g_patchRec[k].idx = idx;
    g_patchRec[k].orig = orig;
    InterlockedExchange(&g_patchRecN, k + 1);
}

static BOOL PatchSlot(void** vtbl, int idx, void* newFn, void** outOrig)
{
    if (!vtbl || !IsExecPtr(vtbl[idx])) return FALSE;
    if (vtbl[idx] == newFn) return FALSE;   /* already ours -> never re-hook ourselves */
    void* prev = vtbl[idx];
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &old)) return FALSE;
    if (outOrig) *outOrig = prev;
    vtbl[idx] = newFn;
    DWORD tmp = 0;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[idx], sizeof(void*));
    if (outOrig) RecordPatch(vtbl, idx, prev);   /* forward patch: remember how to undo */
    return TRUE;
}

/* Put every recorded slot back. Safe to call from DllMain: VirtualProtect and a
   pointer store need no loader lock and no COM. */
static void RestorePatches(void)
{
    LONG n = g_patchRecN;
    if (n > PATCHREC_MAX) n = PATCHREC_MAX;
    for (int i = 0; i < n; i++)
    {
        void** vt = g_patchRec[i].vt;
        int idx = g_patchRec[i].idx;
        if (!vt) continue;
        DWORD old = 0;
        if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) continue;
        vt[idx] = g_patchRec[i].orig;
        DWORD tmp = 0;
        VirtualProtect(&vt[idx], sizeof(void*), old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), &vt[idx], sizeof(void*));
    }
    InterlockedExchange(&g_patchRecN, 0);
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
/* Tick of the last fake-driver start/stop. ASIO's runaway-underrun rollback must
   ignore the window around a stream transition: Fake_Stop discards the ring
   (RingResyncToLatest(0)) and the game pauses its producer around song/stream
   changes, so the ring is legitimately empty for a moment - the resulting flood
   of underruns is NOT the ASIO driver failing (field data 12:32/14:19/14:31: the
   ASIO callback itself was healthy, und=0, for 60 s right up to the transition). */
#define ASIO_UND_GRACE_MS 3000
static volatile LONG64  g_fakeStateTick = 0;
/* The other half of that false positive: the game pauses its producer for a moment
   around a song/stream transition, so the ring drains BEFORE any Fake_Stop is even
   called (see AsioFill). An empty ring is a real underrun only while the producer is
   actually writing, so this tick is bumped on every successful RingWrite and a stale
   one suppresses the count (fake-client path only - see AsioFill's guard). */
#define ASIO_PRODUCER_IDLE_MS 1000
static volatile LONG64  g_lastWriteTick = 0;
static volatile LONG   g_overloads = 0;   /* kAsioOverload messages (counted, not logged) */
static volatile LONG   g_muted     = 0;   /* legacy SetMute path state */
static volatile LONG   g_zeroTap   = 0;   /* v11: zero the game buffer after copy */
static volatile LONG   g_kicked    = 0;   /* stream1 died while bridging */
static volatile LONG   g_asioStartReq = 0;
static volatile LONG   g_s1BufFrames  = 0; /* measured stream1 buffer (frames) */
static volatile LONG   g_asioBusy     = 0; /* inside AsioFill (callback in flight) */

/* fake-client state — declared here (not with the fake code further down) so
   the ASIO realtime callback can test g_fake.running */
/* Fake-client geometry. These used to live down in the fake-client section, but
   RingRead() - far above it - has to know when the ring has room again (the
   room event that wakes a blocked GetBuffer), so they are defined here. */
#define FAKE_FRAMES 441          /* 10 ms @44.1k - exactly what CRI requests */
#define FAKE_CHUNK  441          /* CRI's fill chunk */
#define FAKE_LOW    220          /* refill CRI when the queue drops below ~5 ms */
#define FAKE_RING   882          /* the LARGEST ring depth - used by the WASAPI
                                    backends. One chunk (441) made the producer
                                    wait for a nearly-empty ring (the GetBuffer
                                    gate became "avail must be ~0"), so two chunks
                                    give the consumer a chunk of slack without the
                                    producer ever dropping a beat.
                                    The depth actually in force is g_fakeRingCap
                                    below: ASIO keeps ONE chunk, because two raised
                                    its average backlog from ~220 to ~440 frames -
                                    i.e. +5 ms of latency on the path whose whole
                                    point is 4 ms (N-P1-3). GetBufferSize() reports
                                    that same number, which restores the invariant
                                    GetCurrentPadding() <= GetBufferSize() that
                                    CRI's `space = size - padding` depends on
                                    (N-P0-2). */

struct FakeState
{
    IAudioClient* self;
    WAVEFORMATEX  fmt;
    short*        stage;         /* FAKE_FRAMES*2 shorts - GetBuffer() target */
    short*        zero;          /* silent-release source */
    HANDLE        ev;            /* CRI's refill event (FakePoke -> SetEvent) */
    HANDLE        room;          /* auto-reset: set by the consumer when the ring
                                    has room for another chunk again */
    volatile LONG on;
    volatile LONG running;
};
static FakeState g_fake;

/* Ring depth in force. Fixed at FAKE_RING: making it per-backend was pointless,
   because CRI queues at most ONE chunk - every GetBuffer in the log is
   `n = 441 - padding` - so the second chunk is never reachable, and switching the
   depth only added a transition state where padding could briefly exceed the
   reported buffer size (R-P2-1). GetCurrentPadding() is clamped to FAKE_FRAMES,
   so padding <= bufferSize holds whatever this is. */
static volatile LONG g_fakeRingCap = FAKE_RING;
/* Set while a GetBuffer() is parked on g_fake.room. RingRead only signals the
   event when somebody is actually waiting: signalling on EVERY read put one
   SetEvent per ASIO callback (689/s) and per push write (1000/s) on realtime /
   timing-critical threads, ~1700 kernel calls a second for nothing (N-P1-2). */
static volatile LONG g_gbWaiters = 0;

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
    MemoryBarrier();   /* pair with the InterlockedExchangePointer in AddS1Client (P2-5) */
    for (int i = 0; i < S1_MAX; i++) if (g_s1Clients[i] == client) return 1;
    return 0;
}

static void AddS1Client(void* client)
{
    LONG n = InterlockedIncrement(&g_s1ClientN);
    int slot = (int)((n - 1) % S1_MAX);
    /* Published with an interlocked store (P2-5): the plain write used to race a
       reader looping the table (the ASIO callback asks IsS1Client every time it
       builds a stream), so a freshly registered client could be missed and the
       tap would look dead. */
    InterlockedExchangePointer((PVOID volatile*)&g_s1Clients[slot], (PVOID)client);
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
        /* Log transitions only (P2-1). During a device event the endpoint list
           reports the same state over and over; that is what made a 20 s session
           produce a 506 KB log of which most lines were notification noise. */
        if (id == m_lastStateId && st == m_lastState) return S_OK;
        m_lastStateId = id; m_lastState = st;
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
        /* Throttled (P2-1): one device event emits ~23 of these, all carrying
           the same handful of keys. At most two a second is plenty. */
        ULONGLONG pnow = GetTickCount64();
        if (pnow - m_lastProp < 500) return S_OK;
        m_lastProp = pnow;
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
private:
    const void* m_lastStateId = NULL;
    DWORD       m_lastState   = 0xFFFFFFFFu;
    ULONGLONG   m_lastProp    = 0;
};

static NotifyLogger* g_notifier = NULL;
/* The enumerator the notifier is registered on is kept referenced (P0-2): the
   callback has to be unregistered before this module is unmapped, and that call
   needs a live enumerator. Released by the watchdog, never by DllMain - see
   WatchdogThread. g_wdEvent lets DllMain wake the watchdog out of its 1 s sleep
   so the unregister happens immediately, not up to a second later. */
static IMMDeviceEnumerator* g_notifyEnum  = NULL;
static HANDLE               g_wdEvent     = NULL;
static volatile LONG        g_detachReq   = 0;
static volatile LONG        g_detachDone  = 0;

/* ---------------- lock-free ring (int16 stereo frames) ---------------- */

#define RING_CAP 65536            /* frames (1.49s @44.1k); power of two */
static short* g_ring = NULL;
static volatile LONG64 g_rw = 0, g_rr = 0;
/* frames actually consumed by an output backend - this is the play position we
   report through the fake IAudioClock (and it was hard-wired to 0 before) */
static volatile LONG64 g_fakePlayed = 0;
/* Who may consume the ring right now (P1-2): 0 = nobody, so FakeDriverThread may
   drain; 2 = a real output backend. It is claimed BEFORE that backend's
   Start()/start(), because the consumer starts running inside those calls - the
   old code only flipped its own flag afterwards, and in that window the ASIO
   callback and FakeDriverThread were both reading the same frames (a burst of
   repeats and omissions right at the start of playback). */
static volatile LONG g_ringConsumer = 0;

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
    InterlockedExchange64(&g_lastWriteTick, (LONG64)GetTickCount64());   /* producer alive */
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
    /* Publish the new read cursor only if nobody discarded in the meantime. While
       these frames were being copied, RingResyncToLatest() (Fake_Stop/Fake_Reset,
       i.e. CRI's thread) may have moved g_rr forward to drop stale audio; a plain
       `g_rr = r + n` then put the cursor BACK and re-handed the frames the discard
       had just thrown away - a burst of the previous song. A lost CAS only means
       this read is not published; the frames copied above were legitimately read
       and are still given to the device. */
    InterlockedCompareExchange64(&g_rr, r + (LONG64)n, r);
    InterlockedExchangeAdd64(&g_fakePlayed, (LONG64)n);   /* consumed == played */
    /* Wake a parked GetBuffer as soon as ANY room exists (R-P2-2). The waiter's
       gate is `RingAvail() + n > cap` with the n IT asked for, and CRI's fills are
       usually partial (221/263/306 in the log), so requiring a whole free chunk
       here made those waits sleep longer than needed. The wake can therefore
       arrive before that particular n fits - which is why GetBuffer LOOPS on it
       instead of failing: a failed GetBuffer means "skip this beat" to CRI, so
       turning a premature wake into a return value would drop the audio. */
    if (g_gbWaiters && g_fake.room) SetEvent(g_fake.room);
    return n;
}

static void RingResyncToLatest(unsigned keep)
{
    /* Runs on CRI's thread while a consumer may be inside RingRead() - which is
       safe only because RingRead() publishes g_rr with a compare-exchange, so a
       discard can never be undone by an in-flight read. */
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

/* ---- drift instrumentation: the driver's own frame counter (ASIO only) ----
 * timeInfo.samplePosition is where the driver says it is in its own sample
 * timeline; it advances by one block per callback, i.e. it counts what the
 * hardware really played. Comparing it against what we handed over splits the
 * two failure modes the ring counters cannot see:
 *   posDelta - delivered : frames the device played while we gave it nothing
 *                          (the ring was short and WoFill padded silence) - a
 *                          real audible gap, invisible to RingAvail() because
 *                          the ring is legitimately empty either way,
 *   posTotal / wall time : the device's true rate, i.e. its crystal against the
 *                          wall clock - what shifts the game's whole timeline
 *                          (fake clock, chart) against real time even when
 *                          nothing is ever starved.
 * Only the realtime callback accumulates (integers, no logging - see AsioFill);
 * the monitor thread prints and never writes. */
static LONG64 g_asioPosLast  = 0;          /* touched only by the realtime callback */
static volatile LONG   g_asioHavePos  = 0; /* a baseline exists (2nd callback onwards) */
static volatile LONG64 g_asioPosTotal = 0; /* S(posDelta) = device frames played */
static volatile LONG64 g_asioShort    = 0; /* S(posDelta - delivered) = silence we fed */
static volatile LONG   g_asioTimeInfo = 0; /* driver really provides samplePosition */
static volatile LONG64 g_asioPlay0    = 0; /* g_fakePlayed at the baseline callback */
static volatile LONG64 g_asioT0Qpc    = 0; /* QPC at the baseline callback */

/* asiosys.h sets NATIVE_INT64 to 0, so ASIOSamples is the {hi,lo} struct and has
   to be pasted back together by hand (a plain cast would read only `hi`). */
static LONG64 AsioSamples64(const ASIOSamples& s)
{
    return ((LONG64)s.hi << 32) | (LONG64)s.lo;
}

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
        /* SSE2 fast path (P2-4), four frames per iteration. The cost here is the
           deinterleave - every output sample comes from every *other* input
           sample - so the point is doing the gather inside registers instead of
           four scalar loads. SSE2 is baseline on x64, so no CPUID gate. The
           sign-extension + packs is what makes it exact: masking without it would
           keep the value unsigned and saturate at 32767. */
        i = 0;
        for (; i + 4 <= got; i += 4)
        {
            __m128i v  = _mm_loadu_si128((const __m128i*)(src + (size_t)i * 2));
            __m128i ev = _mm_srai_epi32(_mm_slli_epi32(v, 16), 16);
            __m128i od = _mm_srai_epi32(_mm_slli_epi32(_mm_srli_epi32(v, 16), 16), 16);
            _mm_storel_epi64((__m128i*)(o + i),
                             _mm_packs_epi32(ch == 0 ? ev : od, ch == 0 ? ev : od));
        }
        for (; i < got; i++) o[i] = src[i * 2 + ch];
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
        const __m128 k = _mm_set1_ps(1.0f / 32768.0f);
        i = 0;
        for (; i + 4 <= got; i += 4)
        {
            __m128i v  = _mm_loadu_si128((const __m128i*)(src + (size_t)i * 2));
            __m128i ev = _mm_srai_epi32(_mm_slli_epi32(v, 16), 16);
            __m128i od = _mm_srai_epi32(_mm_slli_epi32(_mm_srli_epi32(v, 16), 16), 16);
            _mm_storeu_ps(o + i, _mm_mul_ps(_mm_cvtepi32_ps(ch == 0 ? ev : od), k));
        }
        for (; i < got; i++) o[i] = src[i * 2 + ch] / 32768.0f;
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
static BOOL WoShutdown(void);         /* v13, defined in the WASAPI-output section.
                                         TRUE = this caller owned the teardown (N-P1-6) */
static void RsFlush(void);            /* v13.3, drop buffered resampler frames */
/* CRI's thread only REQUESTS a resampler flush; WoThread - the single owner of the
   resampler state - applies it at the top of its loop. Calling RsFlush() straight
   from Fake_Stop/Fake_Reset ran concurrently with RsPull and could emit a click. */
static volatile LONG g_rsFlushReq = 0;

static void AsioFill(long index, const ASIOTime* ti)
{
    InterlockedIncrement(&g_asioBusy);   /* teardown waits for this to reach 0 */
    InterlockedIncrement(&g_asoCbs);
    long blk = g_ablock;
    unsigned got = RingRead(g_tmp, (unsigned)blk);
    /* robustness: this runs on the ASIO realtime callback thread. Never do
       file I/O, allocation, or logging here — it can stall the callback and
       worsen underruns. The monitor thread reports the counter once a second. */
    /* When the fake driver is stopped (song end / game exit) the ring is empty
       by design, so that is NOT an underrun: counting it floods the counter
       (500+ per second) and makes the monitor roll ASIO back to a shared stream
       the game no longer needs, which stalls the exit. The `g_fake.running`
       guard alone was not enough (12:32/14:19/14:31): the game pauses its
       producer for a moment around a stream transition, so the ring drains
       BEFORE Fake_Stop is even called, and a rebuilt stream re-fills it late.
       So ignore underruns for ASIO_UND_GRACE_MS after any fake-driver start/stop,
       AND while the producer has written nothing for ASIO_PRODUCER_IDLE_MS - an
       idle producer is not a fault, only an empty ring with a *busy* producer is a
       real underrun. A real ASIO stall lasts far longer than either guard and is
       still caught. In the legacy tap path there is no fake driver, so an empty
       ring is a real underrun there (both guards are off: g_fakeStateTick is 0 and
       g_lastWriteTick only gates the fake-client path). */
    LONG64 gt = (LONG64)GetTickCount64();
    if (got < (unsigned)blk && (!g_fakeClient || g_fake.running) &&
        gt - g_fakeStateTick > ASIO_UND_GRACE_MS &&
        (!g_fakeClient || gt - g_lastWriteTick < ASIO_PRODUCER_IDLE_MS))
        InterlockedIncrement(&g_underruns);
    for (int c = 0; c < 2; c++)
        WriteSamples(g_abufs[c].buffers[index], g_aci[c].type, c, g_tmp, got, (unsigned)blk);
    /* Drift, from the driver's own frame counter (see g_asioPosTotal). Integer
       math only, no logging: this is the realtime callback. A delta outside a
       plausible buffer length means the driver resynced, so it is not
       accumulated - but the baseline is still moved to the new position. */
    if (ti && (ti->timeInfo.flags & kSamplePositionValid))
    {
        InterlockedExchange(&g_asioTimeInfo, 1);
        LONG64 pos = AsioSamples64(ti->timeInfo.samplePosition);
        if (g_asioHavePos)
        {
            LONG64 d = pos - g_asioPosLast;
            if (d >= 0 && d <= (LONG64)blk * 8)
            {
                InterlockedExchangeAdd64(&g_asioPosTotal, d);
                InterlockedExchangeAdd64(&g_asioShort, d - (LONG64)got);
            }
        }
        else
        {
            /* Baseline. Both counters have to start counting from THIS callback,
               or the two rates and the gap are not measured over the same window:
               the first version timed the wall clock from before start(), which
               added a fixed ~100-frame offset that read as -780 ppm at 3 s and
               only faded as 1/t, and it took g_fakePlayed as-is, which still
               carried the frames the pre-ASIO drain had thrown away (~13.5k
               frames -> a bogus +105,000 ppm "game" rate, field data 20:57).
               QueryPerformanceCounter is lock-free and RT-safe (no I/O, no
               allocation, no logging), so it is fine here. */
            LARGE_INTEGER c; QueryPerformanceCounter(&c);
            g_asioPlay0 = (LONG64)g_fakePlayed;
            g_asioT0Qpc = (LONG64)c.QuadPart;
            InterlockedExchange(&g_asioHavePos, 1);
        }
        g_asioPosLast = pos;
    }
    if (g_apostOut && g_asio) g_asio->outputReady();
    FakePoke();   /* v13: top up CRI's fake client from the real output clock */
    InterlockedDecrement(&g_asioBusy);
}

static ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct)
{
    (void)direct;
    /* params carries the driver's samplePosition - the only device-side truth
       available on this path (see g_asioPosTotal). */
    AsioFill(index, params);
    return params;
}

static void cbBufferSwitch(long index, ASIOBool direct) { (void)direct; AsioFill(index, NULL); }
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
    InterlockedExchange(&g_ringConsumer, 0);   /* the callback cannot run now (P1-2) */
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

/* Wait until the game's tap is producing at all.
 *
 * This used to demand ~88 % of real time for three consecutive 100 ms windows
 * and to wait the full 30 s otherwise, so a title screen without BGM - or any
 * silent stretch - meant up to 30 s with NO audio backend running at all: the
 * ring was drained by FakeDriverThread and nothing was ever played (P1-3).
 * "Producing at all" is the honest requirement. If the tap stays quiet we start
 * ASIO anyway: an empty ring plays silence exactly as harmlessly as the drain
 * did, and real audio starts the moment the game writes. (The producer-idle
 * guard in AsioFill keeps that silence from being counted as runaway underruns.) */
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
        if (d > 0) { good++; if (good >= 2) return TRUE; }   /* ~200 ms of production */
        else good = 0;
        if (GetTickCount64() - wt0 > 3000)
        {
            Log("[asio] tap quiet for 3 s - starting ASIO anyway (silence costs nothing)");
            return TRUE;
        }
        if ((iters % 30) == 0)
            Log("[asio] still waiting for the tap (taps=%lld, last100ms=%lld)",
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
    /* Fresh attempt: clear the "stream died while bridging" latch HERE, at the very
       start, not later (audit). CRI keeps calling stream1 through the whole attempt
       - WaitForTapSteady waits up to 3 s and the driver is prepared in that window -
       and those calls set the latch if the device reports DEVICE_INVALIDATED. A
       reset placed after that window would erase a genuine "the stream we are about
       to bridge is dead", leaving ASIO bridging a dead stream with no rollback. */
    InterlockedExchange(&g_kicked, 0);

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
    /* If WoShutdown's CAS loses a race - the watchdog may have moved the state in
       between - the WASAPI writer is still alive, and starting ASIO anyway would
       put two consumers on one ring (R-P2-3). Give up this attempt instead; the
       watchdog retries on its next tick. */
    /* 2 counts too (audit): that is the watchdog mid-WoInit, holding the ring for a
       shared stream it has not started yet. Only checking 3 would let this call
       claim the ring and start ASIO alongside a writer that is about to come up -
       two consumers on one ring. 3 is the running case we can actually take over. */
    LONG wos = g_woState;   /* snapshot: WoShutdown moves it, and the log must not lie */
    if (wos == 2 || (wos == 3 && !WoShutdown()))
    {
        Log("[asio] !! the WASAPI writer holds the ring (state %ld) - aborting this ASIO start", wos);
        /* The driver was prepared before this point (CoCreateInstance, init() and
           createBuffers all ran in AsioPrepare), so its teardown belongs here too
           (audit): skipping it leaks the instance AND leaves the device claimed by
           our own process, which makes the NEXT init() fail - ASIO would be dead for
           the rest of the session. */
        if (g_asio) AsioTeardown();
        /* ...but hand the ring claim back. AsioTeardown clears g_ringConsumer on the
           assumption that it is ASIO's; in the state-2 case it is the claim of the
           shared writer that is starting right now (WoThread sets it before Start()
           and only sets g_woReading once it enters its loop), so clearing it would
           re-open the double-drain window this branch exists to avoid: the fake
           driver thread only checks `g_ringConsumer != 0 || g_woReading`. This
           branch only ever runs with a WASAPI writer starting (2) or running (3). */
        if (wos == 2 || wos == 3) InterlockedExchange(&g_ringConsumer, 2);
        /* Undo the handoff if it ran (it only does with fake_client = 0, but then
           leaving the session muted or tap-zeroed with no ASIO stream behind it
           would be plain silence). The shared writer is still consuming, so the
           game's audio is safe to restore. */
        if (InterlockedExchange(&g_zeroTap, 0)) Log("[handoff] abort -> tap-and-zero released");
        if (InterlockedExchange(&g_muted, 0)) { MuteSession(FALSE); Log("[handoff] abort -> session unmuted"); }
        /* State must go to 4, not stay at the "starting" 2 set at the top of this
           function: the watchdog and WoInit key off the state (WoInit refuses to
           claim the ring only while ASIO is at 3), so a stale 2 leaves the machine
           describing a backend that is not running - the same trap R-P0-1 was.
           4 is what every other failure path in here does, and it is retryable. */
        InterlockedExchange(&g_asioState, 4);
        /* Release the start request as well (audit): it is what Hook_Initialize
           and the watchdog CAS 1 -> 0 to re-arm ASIO, so leaving it at 1 means
           every later stream1 rebuild fails that CAS and ASIO never comes back -
           silent loss of the default backend with no self-recovery. Both other
           failure paths here and AsioRollback already clear it. */
        InterlockedExchange(&g_asioStartReq, 0);
        return FALSE;
    }

    /* Drift baselines (see g_asioPosTotal): reset per session so the averages the
       monitor prints start with THIS stream, not with an earlier one. */
    InterlockedExchange(&g_asioHavePos, 0);
    InterlockedExchange64(&g_asioPosTotal, 0);
    InterlockedExchange64(&g_asioShort, 0);
    InterlockedExchange(&g_asioTimeInfo, 0);
    /* (g_kicked is cleared at the top of this function, before the tap wait - see
       the note there: clearing it here instead would erase a kick raised during
       that wait, i.e. the "we are about to bridge a dead stream" case.) */

    /* Claim the ring BEFORE start(): the driver begins calling us back inside it,
       and until now only FakeDriverThread's own flag stopped the double drain
       (P1-2). */
    InterlockedExchange(&g_ringConsumer, 2);

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
        {
            Log("[asio] t=%llds cbs=%ld und=%ld avail=%u zeroTap=%ld muted=%ld",
                i, g_asoCbs, g_underruns, RingAvail(), g_zeroTap, g_muted);
            /* Drift (see g_asioPosTotal). Cumulative averages, not per-second
               deltas: a crystal error only shows up over a long window. `dev` is
               the hardware rate against the wall clock, `game` is the rate the
               game's own clock (fake clock == frames we delivered) advances at,
               and `gap` is the frames the device played while we handed it
               nothing. dev == game + gap, so a non-zero gap is audible dropout
               and dev != game is the timeline drift against the chart. */
            /* Both counters are measured from the baseline callback the callback
               thread stamped (see g_asioT0Qpc), so dev - game == gap exactly and
               the ppm is honest from the first second - not just after a minute. */
            LARGE_INTEGER nowc; QueryPerformanceCounter(&nowc);
            double el = (double)(nowc.QuadPart - g_asioT0Qpc) / (double)g_qpcFreq.QuadPart;
            LONG64 pos = (LONG64)g_asioPosTotal;
            LONG64 play = (LONG64)g_fakePlayed - g_asioPlay0;
            LONG64 gap = (LONG64)g_asioShort;
            if (!g_asioTimeInfo)
            {
                if (i == 20)
                    Log("[asio] drift: this driver reports no samplePosition - device-side "
                        "drift unavailable, the ring counters are all there is");
            }
            else if (g_asioHavePos && el >= 1.0)
                Log("[asio] drift: dev=%.1f fr/s (%+.0f ppm) game=%.1f fr/s (%+.0f ppm) gap=%lld fr (%.2f ms/s avg)",
                    (double)pos / el, ((double)pos / el - 44100.0) * 1e6 / 44100.0,
                    (double)play / el, ((double)play / el - 44100.0) * 1e6 / 44100.0,
                    (long long)gap, (double)gap / el * 1000.0 / 44100.0);
        }
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
        /* Throttled: once the device is gone EVERY call fails, and Log() fflushes
           each line - that is a lot of disk I/O on CRI's mixing thread exactly
           when the pipe is already in trouble (it used to write the same line
           thousands of times a second). */
        LONG f = InterlockedIncrement(&g_rcFail);
        if (f <= 8 || (f % 500) == 0)
            Log("[rc] !! GetBuffer(%u fr) FAILED hr=0x%08X from exe+0x%llX (failure #%ld)",
                frames, (unsigned)hr,
                (unsigned long long)((UINT_PTR)ra - (UINT_PTR)GetModuleHandleA(NULL)), f);
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
            /* Empty the slot again. `ptr` is the gate the next ReleaseBuffer checks,
               so a stale address left in it would let a release with no matching
               GetBuffer (a seek/restart, or a GetBuffer that failed) re-inject the
               previous block into the ring - an audible repeat. Length first,
               pointer last, mirroring Hook_RC_GetBuffer. */
            g_rcSlots[si].frames = 0;
            MemoryBarrier();
            g_rcSlots[si].ptr = NULL;
        }
    }

    HRESULT hr = oRCRB ? oRCRB(self, frames, flags) : E_FAIL;
    LONG n = InterlockedIncrement(&g_rcCalls);
    if (n <= 8)
        Log("[rc] ReleaseBuffer(%u fr, flags=0x%X) hr=0x%08X", frames, (unsigned)flags, (unsigned)hr);
    if (FAILED(hr))
    {
        LONG f = InterlockedIncrement(&g_rcFail);   /* throttled, see GetBuffer */
        if (f <= 8 || (f % 500) == 0)
            Log("[rc] !! ReleaseBuffer(%u fr) FAILED hr=0x%08X from exe+0x%llX (failure #%ld)",
                frames, (unsigned)hr,
                (unsigned long long)((UINT_PTR)ra - (UINT_PTR)GetModuleHandleA(NULL)), f);
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

    /* Only the first few (P2-2): a stream rebuild asks for this every time, and
       the answer is always the same, so the rest is pure log volume. */
    static volatile LONG gsLogged = 0;
    if (isRC && InterlockedIncrement(&gsLogged) <= 4)
        Log("[gs] GetService IAudioRenderClient -> hr=0x%08X ptr=%p", (unsigned)hr, (ppv ? *ppv : NULL));

    if (SUCCEEDED(hr) && ppv && *ppv && isRC)
    {
        /* Only stream1's own render client is ever tapped. Our exclusive/shared
           OUTPUT stream's render client shares the same vtable, so patching it
           here used to wrap WoThread's own GetBuffer/ReleaseBuffer in the legacy
           tap hooks - extra work plus file I/O on the output thread at startup. */
        if (g_asioEn && IsS1Client(self))
        {
            AddS1Rc(*ppv);
            InterlockedExchange(&g_tapOn, 1);

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

/* Stream1 can be rebuilt on a NEW client object (device switch, unplug). The
   vtable is shared, so the new object is already ours - but g_fake.self can only
   name one of them. Remember the superseded ones (P2-6): their calls used to fall
   through to the real implementation, which was never Initialize()d, so every
   call failed with NOT_INITIALIZED and CRI got a confusing error where
   AUDCLNT_E_DEVICE_INVALIDATED is what makes it rebuild the stream cleanly. */
#define STALE_MAX 4
static void* g_staleSelf[STALE_MAX] = { 0 };
static volatile LONG g_staleN = 0;

static BOOL FakeIsStale(IAudioClient* self)
{
    if (!self || self == g_fake.self) return FALSE;
    MemoryBarrier();
    for (int i = 0; i < STALE_MAX; i++) if (g_staleSelf[i] == self) return TRUE;
    return FALSE;
}

static void MarkStale(IAudioClient* self)
{
    if (!self) return;
    LONG n = InterlockedIncrement(&g_staleN);
    int slot = (int)((n - 1) % STALE_MAX);      /* ring: the oldest entry is replaced */
    InterlockedExchangePointer((PVOID volatile*)&g_staleSelf[slot], (PVOID)self);
    Log("[fake] previous stream1 client %p superseded -> its calls now report DEVICE_INVALIDATED", self);
}

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
        if (n > (UINT32)g_fakeRingCap) { InterlockedIncrement(&g_fakeGbFail); return (HRESULT)0x88890006; }
        /* Back-pressure (P0-1). This runs on CRI's mixing thread, never on a
           realtime callback, so a bounded wait is safe - and it is the right
           answer here: CRI treats AUDCLNT_E_BUFFER_TOO_LARGE as "skip this beat",
           so the audio in that chunk is dropped outright (the probe drops to 67 %
           when the gate is one chunk deep). The consumer signals g_fake.room from
           RingRead the instant the ring has drained back to a free chunk, so the
           wait ends as soon as there is room instead of after a Sleep quantum.
           The old loop polled 5 x Sleep(1): ~5 ms at best, ~78 ms on CRI's
           mixing thread whenever timeBeginPeriod(1) was not in effect, because
           Sleep(1) then rounds up to the ~15.6 ms system tick. The 30 ms cap is
           a genuine device fault (the consumer has stopped), not back-pressure. */
        if (RingAvail() + n > (unsigned)g_fakeRingCap)
        {
            /* Announce the wait first, so RingRead knows to signal (N-P1-2); if
               room appeared in between, the loop's re-check catches it.
               LOOP rather than fail on the first wake: RingRead reports "some
               room", which may still be short of THIS n, and returning
               AUDCLNT_E_BUFFER_TOO_LARGE makes CRI drop the whole beat - i.e. a
               premature wake would be an audible hole. The 30 ms deadline is the
               genuine "the consumer is gone" test, not a back-pressure timeout.
               It cannot spin: the producer is the one waiting, so the ring only
               shrinks until the condition is met. */
            if (!g_fake.room)
            {
                InterlockedIncrement(&g_fakeGbFail);
                return (HRESULT)0x88890006;
            }
            ULONGLONG wt0 = GetTickCount64();
            InterlockedIncrement(&g_gbWaiters);
            for (;;)
            {
                DWORD w = WaitForSingleObject(g_fake.room, 30);
                if (RingAvail() + n <= (unsigned)g_fakeRingCap) break;
                if (w == WAIT_TIMEOUT || (ULONGLONG)GetTickCount64() - wt0 >= 30) break;
            }
            InterlockedDecrement(&g_gbWaiters);
            if (RingAvail() + n > (unsigned)g_fakeRingCap)
            {
                InterlockedIncrement(&g_fakeGbFail);
                return (HRESULT)0x88890006;   /* AUDCLNT_E_BUFFER_TOO_LARGE */
            }
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
        if (n > (UINT32)g_fakeRingCap) return E_INVALIDARG;
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

/* ---- our IAudioClock (and IAudioClock2) ---- */
static const GUID kIID_IAudioClock =
{ 0xCD63314F, 0x3FD1, 0x4A1F, { 0xBA, 0x1B, 0x28, 0xED, 0x6E, 0xF1, 0xF6, 0x57 } };
/* P1-5: a CRI build that asks for IAudioClock2 / IAudioStreamVolume used to get
   E_NOINTERFACE and could then treat the whole fake client as unusable. Answering
   them costs almost nothing. The IIDs come from __uuidof so they cannot drift from
   the SDK's own definitions. Note that IAudioClock2 does NOT derive from
   IAudioClock - it is a separate IUnknown-based interface - so the clock exposes it
   as a second object, exactly as a real endpoint does. */
static void FakeClockNow(UINT64* pos)
{
    /* Report what has been HEARD (P1-1): g_fakePlayed counts a frame as the
       consumer reads it off the ring, and it is still in the device buffer at that
       moment. Only the *device* fill comes off - frames still in the ring were
       never counted into g_fakePlayed, so subtracting RingAvail() as well would
       double-count and drag the clock behind the audio. pad is in OUTPUT frames
       and g_fakePlayed in source frames, so convert with g_rsStep. */
    LONG64 inflight = 0;
    if (g_woState == 3 && g_woPad > 0)
        inflight = (LONG64)((double)g_woPad * g_rsStep);
    LONG64 played = (LONG64)g_fakePlayed - inflight;
    *pos = (UINT64)(played > 0 ? played : 0);
}

class FakeClock2 : public IAudioClock2
{
public:
    FakeClock2() : m_ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IAudioClock2)))
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
    HRESULT STDMETHODCALLTYPE GetDevicePosition(UINT64* pos, UINT64* qpc) override
    {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        if (pos) FakeClockNow(pos);
        if (qpc) *qpc = (UINT64)c.QuadPart;
        return S_OK;
    }
private:
    LONG m_ref;
};
static FakeClock2* g_fakeClk2 = NULL;

class FakeClock : public IAudioClock
{
public:
    FakeClock() : m_ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, kIID_IAudioClock))
        { *ppv = this; AddRef(); return S_OK; }
        if (IsEqualIID(riid, __uuidof(IAudioClock2)) && g_fakeClk2)
        { *ppv = (void*)g_fakeClk2; g_fakeClk2->AddRef(); return S_OK; }
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
        /* "now": what is still in the device buffer is what is still unheard, so
           the timestamp pairs with the returned position. See FakeClockNow. */
        if (pos) FakeClockNow(pos);
        if (qpc) *qpc = (UINT64)c.QuadPart;
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

/* ---- our IAudioStreamVolume (P1-5) ----
   There is no mixer to talk to - we are the only writer - but a client that
   asks for volume control should get a working object rather than E_NOINTERFACE.
   The value is remembered so a read-back is consistent; it is never applied to
   the audio (CRI's own mixer already did that before it handed us the PCM). */
class FakeVol : public IAudioStreamVolume
{
public:
    FakeVol() : m_ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IAudioStreamVolume)))
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
    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* c) override
    { if (!c) return E_POINTER; *c = 2; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 i, const float v) override
    { if (i >= 2) return E_INVALIDARG; m_vol[i] = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 i, float* v) override
    { if (i >= 2 || !v) return E_INVALIDARG; *v = m_vol[i]; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 n, const float* v) override
    { if (n > 2 || (n && !v)) return E_INVALIDARG; for (UINT32 i = 0; i < n; i++) m_vol[i] = v[i]; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 n, float* v) override
    { if (n > 2 || (n && !v)) return E_INVALIDARG; for (UINT32 i = 0; i < n; i++) v[i] = m_vol[i]; return S_OK; }
private:
    LONG  m_ref;
    float m_vol[2] = { 1.0f, 1.0f };
};

static FakeRC*    g_fakeRc = NULL;
static FakeClock* g_fakeClk = NULL;
static FakeVol*   g_fakeVol = NULL;

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
    if (FakeIsStale(self)) return AUDCLNT_E_DEVICE_INVALIDATED;   /* P2-6 */
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
    if (IsEqualIID(riid, __uuidof(IAudioStreamVolume)))
    {
        if (!g_fakeVol) return E_NOINTERFACE;
        *ppv = (void*)g_fakeVol;
        g_fakeVol->AddRef();
        Log("[fake] GetService -> IAudioStreamVolume");
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
               to pace CRI from; the WASAPI shared stream can have a 20+ ms
               period, so there it still needs a poke from here. The exclusive
               PUSH writer already pokes every millisecond itself, so doing it
               again here is pure duplication (P2-3). */
            if (ws == 3) { if (!g_woExclusive) FakePoke(); Sleep(1); }
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
        if (g_ringConsumer != 0 || g_woReading)
        {
            /* A consumer owns the ring right now. Draining here as well would hand
               the same frames to two consumers (audible repeats). g_ringConsumer is
               claimed BEFORE the backend starts (P1-2), which closes the startup
               window the old g_woReading-only check left open - WoThread set that
               only *after* Start() had already begun reading. Note that "starting"
               (state 2) is still not such a case, so the drain that keeps CRI's
               clock alive while ASIO waits for a steady tap is untouched. */
            Sleep(2);
            continue;
        }
        /* Nothing running and nothing starting: drain at real time so CRI keeps a
           sane clock and the game never stalls on a full queue.

           How much to drain MUST come from the elapsed wall clock, not from a
           fixed chunk. RingRead() advances the fake clock (g_fakePlayed) that CRI
           reads as "played" and the game syncs its chart to, so the old fixed
           220-frame-per-Sleep(5) loop ran ~9 % slow: the whole timeline - music
           and chart alike - dragged behind real time for as long as no output
           backend was up, and for the rest of the session whenever every backend
           had failed. Consuming exactly what elapsed time owes pins the drain
           rate, and with it the clock, at 44,100 fr/s. */
        double t0 = MsNow();
        Sleep(5);
        unsigned want = (unsigned)((MsNow() - t0) * 44.1);   /* elapsed ms -> 44,100 fr/s */
        if (want > FAKE_CHUNK) want = FAKE_CHUNK;            /* tmp[] holds FAKE_CHUNK frames */
        unsigned a = RingAvail();
        unsigned n = a < want ? a : want;
        /* tmp must hold `n` stereo frames. FakeRC::GetBuffer's back-pressure caps
           RingAvail() at FAKE_RING (882), and the clamp above caps `want` at
           FAKE_CHUNK (441), so n <= 441 frames = 882 shorts: tmp[FAKE_CHUNK*2] is
           exactly enough. The old tmp[512] (256 frames) overflowed whenever CRI
           handed us a 257..440-frame chunk (e.g. a partial fill). */
        if (n) { static short tmp[FAKE_CHUNK * 2]; RingRead(tmp, n); }
        FakePoke();
    }
    Log("[fake] driver stopped");
    return 0;
}

/* ---- fake vtable implementations (dispatch: ours vs real) ---- */
static HRESULT STDMETHODCALLTYPE Fake_GetBufferSize(IAudioClient* s, UINT32* p)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    /* Report 441 - the buffer CRI actually asked for - and keep GetCurrentPadding
       clamped to it (R-P2-1). CRI sizes its fills as `period - padding` from
       Initialize(hnsBufferDuration), never from this call: the log shows
       n = 441 - padding on every GetBuffer. Reporting the ring depth instead was
       a lie that only an implementation computing `space = GetBufferSize - padding`
       would have tripped over. 441 plus the clamp keeps padding <= bufferSize. */
    if (FakeIsSelf(s)) { if (p) *p = FAKE_FRAMES; return S_OK; }
    return oGBS ? oGBS(s, p) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetStreamLatency(IAudioClient* s, REFERENCE_TIME* p)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FakeIsSelf(s)) { if (p) *p = 0; return S_OK; }
    return oGLat ? oGLat(s, p) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetCurrentPadding(IAudioClient* s, UINT32* p)
{
    /* Close enough for CRI's bookkeeping: with a resampler active the ring is
       source frames while the stream counts output frames, so this over-reports
       by up to 10 ms. GetBuffer() uses the same RingAvail() gate, so the two
       stay consistent and the ring cannot overflow (audit N9). */
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    /* Clamped (R-P2-1): the ring can hold two chunks, this client's buffer is one,
       and CRI's arithmetic assumes padding <= bufferSize. The extra chunk stays
       invisible, which is what a one-chunk device would report anyway. */
    if (FakeIsSelf(s))
    {
        if (p) { UINT32 a = RingAvail(); *p = a > FAKE_FRAMES ? FAKE_FRAMES : a; }
        return S_OK;
    }
    return oGPad ? oGPad(s, p) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_IsFormatSupported(IAudioClient* s, AUDCLNT_SHAREMODE m,
                                                        const WAVEFORMATEX* f, WAVEFORMATEX** closest)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FakeIsSelf(s)) { if (closest) *closest = NULL; return S_OK; }
    return oIsF ? oIsF(s, m, f, closest) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_GetMixFormat(IAudioClient* s, WAVEFORMATEX** pp)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
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
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FakeIsSelf(s)) { if (def) *def = 100000; if (mn) *mn = 30000; return S_OK; }
    return oGDP ? oGDP(s, def, mn) : E_FAIL;
}
/* The fake driver thread exits only after it observes g_fake.running == 0, and
   it can be asleep in up to 20 ms of Sleep() when Fake_Stop clears that flag.
   Keeping its handle (instead of closing it inside Fake_Start) is what makes a
   fast Stop -> Start safe: the old code set the flag straight back to 1, the
   sleeping thread then woke up, saw 1 and kept looping, and TWO drain threads
   consumed the same ring at once (chopped, half-speed audio). Now the previous
   thread is joined first; if it is still inside its loop it simply gets the flag
   back - it *is* the driver thread this Start wants, so no second one is made. */
static HANDLE g_fakeThread = NULL;

static HRESULT STDMETHODCALLTYPE Fake_Start(IAudioClient* s)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FakeIsSelf(s))
    {
        BOOL reuse = FALSE;
        if (g_fakeThread)
        {
            if (WaitForSingleObject(g_fakeThread, 200) == WAIT_OBJECT_0)
            {
                CloseHandle(g_fakeThread);   /* it left its loop: reap it */
                g_fakeThread = NULL;
            }
            else
                reuse = TRUE;                /* still looping: it will pick the flag up */
        }
        InterlockedExchange(&g_fake.running, 1);
        InterlockedExchange64(&g_fakeStateTick, (LONG64)GetTickCount64());
        if (g_fake.room) ResetEvent(g_fake.room);   /* a fresh start re-arms the gate */
        /* The thread may have left between that timeout and the flag flip; a
           thread handle stays signalled once its thread ends, so this re-check
           is what keeps "reuse" from meaning "no driver thread at all". */
        if (reuse && WaitForSingleObject(g_fakeThread, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(g_fakeThread);
            g_fakeThread = NULL;
            reuse = FALSE;
        }
        if (reuse)
            Log("[fake] Start() -> previous driver thread still alive, reusing it");
        else
        {
            HANDLE t = CreateThread(NULL, 0, FakeDriverThread, NULL, 0, NULL);
            if (t) { g_fakeThread = t; Log("[fake] Start() -> driver launched"); }
            else { InterlockedExchange(&g_fake.running, 0); Log("[fake] !! driver thread create failed"); }
        }
        return S_OK;
    }
    return oStart ? oStart(s) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_Stop(IAudioClient* s)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FakeIsSelf(s))
    {
        InterlockedExchange(&g_fake.running, 0);
        InterlockedExchange64(&g_fakeStateTick, (LONG64)GetTickCount64());
        RingResyncToLatest(0);   /* the next song must not start with old audio */
        InterlockedExchange(&g_rsFlushReq, 1);   /* ...including the resampler's own
                                                    441 frames (WoThread applies it) */
        /* The discard left the ring empty, so the gate is open again - release a
           GetBuffer that was waiting on the room event. */
        if (g_fake.room) SetEvent(g_fake.room);
        Log("[fake] Stop()");
        return S_OK;
    }
    return oStop ? oStop(s) : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE Fake_Reset(IAudioClient* s)
{
    if (FakeIsStale(s)) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FakeIsSelf(s))
    {
        /* Any ring discard must also stamp the grace, or the ASIO callback sees the
           freshly emptied ring as an underrun flood and rolls back (the one gap the
           Start/Stop stamps left open: a seek / restart discards without Stop). */
        InterlockedExchange64(&g_fakeStateTick, (LONG64)GetTickCount64());
        RingResyncToLatest(0);   /* restart/seek: drop up to 10 ms of stale audio */
        InterlockedExchange(&g_rsFlushReq, 1);   /* ...and the resampler's buffer */
        if (g_fake.room) SetEvent(g_fake.room);  /* the empty ring re-opens the gate */
        return S_OK;
    }
    return oReset ? oReset(s) : E_FAIL;
}

/* ---- install: replace stream1's client ---- */
static HRESULT FakeInit(IAudioClient* self, const WAVEFORMATEX* fmt)
{
    if (!g_fake.stage)
    {
        /* Sized for the LARGEST depth: the cap can change with the backend, and
           these two are the GetBuffer()/silent-release targets. */
        g_fake.stage = (short*)calloc((size_t)FAKE_RING * 2, sizeof(short));
        g_fake.zero  = (short*)calloc((size_t)FAKE_RING * 2, sizeof(short));
    }
    if (!g_fake.stage || !g_fake.zero) return E_OUTOFMEMORY;
    /* Auto-reset: the consumer sets it when the ring has room again (RingRead),
       a waiting GetBuffer consumes the signal. Created once per process. */
    if (!g_fake.room) g_fake.room = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!g_fakeRc)   g_fakeRc   = new (std::nothrow) FakeRC();
    if (!g_fakeClk)  g_fakeClk  = new (std::nothrow) FakeClock();
    if (!g_fakeClk2) g_fakeClk2 = new (std::nothrow) FakeClock2();   /* P1-5 */
    if (!g_fakeVol)  g_fakeVol  = new (std::nothrow) FakeVol();      /* P1-5 */
    if (!g_fakeRc || !g_fakeClk || !g_fakeClk2 || !g_fakeVol) return E_OUTOFMEMORY;

    if (g_fake.self && g_fake.self != self) MarkStale(g_fake.self);   /* P2-6 */
    g_fake.self = self;
    memcpy(&g_fake.fmt, fmt, sizeof(WAVEFORMATEX));
    g_fake.fmt.cbSize = 0;
    /* WAVE_FORMAT_EXTENSIBLE with cbSize = 0 is an INVALID WAVEFORMATEX (the tag
       promises a 22-byte WAVEFORMATEXTENSIBLE with a SubFormat that is no longer
       there). Forcing cbSize to 0 was meant to make the struct self-contained,
       but it left the tag behind: a GetMixFormat caller would read "EXTENSIBLE"
       and look for a SubFormat that does not exist. The stream1 gate already
       guarantees 16-bit PCM stereo, so report WAVE_FORMAT_PCM. */
    if (g_fake.fmt.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        g_fake.fmt.wFormatTag = WAVE_FORMAT_PCM;
    g_fake.ev = NULL;
    InterlockedExchange(&g_fake.running, 0);
    if (g_fake.room) ResetEvent(g_fake.room);   /* drop any stale room signal */

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
static IAudioClock*        g_woClock  = NULL; /* exclusive: real device clock (self-test only) */
static UINT64              g_woClockFreq = 0; /* IAudioClock::GetFrequency() (clock units/s) */

/* ---- resampler (only active when the endpoint is not 44.1 kHz) ----
 * CRI always hands us 44.1 kHz. A 48 kHz endpoint would otherwise play
 * everything 8.8 % fast (~1.5 semitones up) - which is exactly what v13.0 did,
 * because only the ASIO path had a sample-rate guard. Linear interpolation is
 * plenty for such a small ratio and costs one frame of latency. */
static double g_rsLastMs = 0.0;        /* tempo monitor: last sample time (MsNow) */
static UINT64 g_rsLastPos = 0;         /* tempo monitor: last fake-clock position */
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
            /* Starved: leave g_rsFrac < 1 so the next call interpolates instead of
               extrapolating past g_rsNext (an f >= 1 sample is a click). */
            if (!RsNext(g_rsNext)) { g_rsHasNext = 0; while (g_rsFrac >= 1.0) g_rsFrac -= 1.0; break; }
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

    /* MMCSS: exclusive's EVENTCALLBACK loop is a software event loop, so the wake-up
       latency after each event is what starves the device (measured 10 ms ~94.5 % in
       game vs 99.5 % standalone). THREAD_PRIORITY_TIME_CRITICAL only sets a number;
       MMCSS lets the kernel reserve CPU for this thread, which is what actually closes
       the gap (this is how pro-audio / ASIO host threads run). Exclusive only: the
       shared backend is clocked by the system mixer and does not need it. avrt.dll is
       a system DLL, so the no-VC++-runtime distribution stays intact; if the request
       is refused we simply run without it. */
    HANDLE mm = NULL;
    if (g_woExclusive)
    {
        DWORD mmIdx = 0;
        mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);
        if (mm)
        {
            if (AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL))
                Log("[wo] MMCSS 'Pro Audio' + AVRT_PRIORITY_CRITICAL engaged");
            else
                Log("[wo] !! MMCSS registered but AvSetMmThreadPriority failed (err=%lu)", GetLastError());
        }
        else
            Log("[wo] !! AvSetMmThreadCharacteristics('Pro Audio') failed (err=%lu) - running without MMCSS", GetLastError());
    }

    Log("[wo] thread up (frames=%u float=%d)", (unsigned)g_woFrames, g_woFloat);
    InterlockedExchange(&g_ringConsumer, 2);   /* claim the ring before Start() (P1-2) */
    if (FAILED(g_woClient->Start()))
    {
        Log("[wo] !! Start failed");
        InterlockedExchange(&g_woState, 4);
        InterlockedExchange(&g_woAlive, 0);   /* this path used to leave it set, so
                                                 rollback waited 2 s for a dead thread */
        /* Give the ring back (audit): the claim above has no consumer behind it
           now, and leaving it set closes FakeDriverThread's gate for good - the
           ring would stop being drained, g_fakePlayed would freeze and CRI's queue
           would back up. The push writer's Start-failure path already does this. */
        InterlockedExchange(&g_ringConsumer, 0);
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return 0;
    }
    InterlockedExchange(&g_woState, 3);
    InterlockedExchange(&g_woReading, 1);
    Log("[wo] output running");
    int nopulse = 0, faults = 0;
    while (g_woRun)
    {
        /* A flush requested by Fake_Stop/Fake_Reset is applied HERE, on the only
           thread that owns the resampler state: RsFlush() wipes g_rsCur/g_rsNext/
           g_rsBuf/g_rsI/g_rsN, and calling it from CRI's thread while RsPull below
           was mid-sample tore that state (an audible click). One loop iteration of
           delay is invisible - all that matters is that the previous song's tail is
           gone before the next one starts playing. */
        if (InterlockedExchange(&g_rsFlushReq, 0)) RsFlush();
        DWORD w = WaitForSingleObject(g_woEvent, 200);
        if (!g_woRun) break;
        if (w != WAIT_OBJECT_0)
        {
            /* No event for 200 ms. One missed event is normal (the engine is
               idle), so this only *reports* it - the failure detection below is
               the one that acts. The exclusive backend must NOT `continue` here:
               once its single period runs dry the device stops firing events for
               good, and skipping the top-up deadlocked the whole pipe (silent
               game, hung exit - the "silent on the 2nd launch" report). Fall
               through so the never-starve fill below can run. */
            if (++nopulse == 25) Log("[wo] !! no data event for 5 s (device paused?)");
            if (!g_woExclusive) continue;
        }
        else
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
        InterlockedExchange(&g_woPad, (LONG)pad);   /* P1-1: the clock reads this */
        UINT32 free_ = g_woFrames > pad ? g_woFrames - pad : 0;
        /* Only write what the ring can actually produce. Two units are involved:
           free_ is OUTPUT frames (device rate), RingAvail() is SOURCE frames
           (44.1 kHz). When step > 1 (endpoints below 44.1 kHz, e.g. 32 kHz) one
           output frame consumes more than one source frame, so handing `free_`
           straight to RsPull starves the resampler and pads ~27 % of every
           period with silence (music ran at 72.6 % speed). */
        UINT32 avail = RingAvail();
        if (g_rsStep != 1.0)
        {
            avail = (UINT32)((double)avail / g_rsStep);
            /* The interpolator keeps one frame of look-ahead (g_rsNext), so leave a
               small margin or RsPull comes back one frame short every period. The
               conversion is needed in BOTH directions: step > 1 (endpoints below
               44.1 kHz) consumes more than one source frame per output frame, and
               step < 1 (48 / 96 kHz) consumes less than one. Gating on
               `g_rsStep > 1.0` only converted the former, so a 48 kHz endpoint used
               just 1/step (94 %) of the ring's capacity every period and padded the
               rest with silence. */
            avail = avail > 2 ? avail - 2 : 0;
        }
        UINT32 n = free_ < avail ? free_ : avail;
        /* Exclusive + EVENTCALLBACK has no engine buffer of its own: its single
           period must be filled COMPLETELY on every event. Handing it a partial
           fill - what `min(free_, avail)` above does, with a full period only
           when n happened to be exactly 0 - leaves the rest of the period holding
           frames the device already played and lets the queue drain until the
           device stops firing events; the writer then waits forever and the whole
           pipe deadlocks: the ring fills, CRI blocks, the game stays silent and
           never exits (the "silent on the second launch / alt+F4 hangs" report).
           A ring that is merely *short* (a CRI partial fill around a song change)
           triggered exactly that, once per transition. Pad the shortfall with
           silence instead - WoFill16/WoFillFloat already zero the tail - so the
           device always has a whole period to chew on. That also covers the
           startup window: before CRI starts mixing the ring is empty, n would be 0
           and nothing at all would reach the device (the false 0 % self-test
           reading, and a device that stops firing events). */
        if (g_woExclusive) n = free_;
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

        /* Music-tempo monitor (diagnostic only, both backends). The fake clock is
           what CRI reads as "played", so the rate g_fakePlayed advances at IS the
           tempo the game syncs its chart to - 44,100 fr/s means true real time.
           It deliberately does NOT nudge the resample step; g_rsStep is derived
           from the negotiated device format and is exact by construction. Two
           field lessons behind this: (1) nudging from this rate is blind to the
           underfeed failure mode - with step > 1 the ring is production limited,
           so the clock keeps reading 44,100/s while the starved device replays
           stale frames (cumulative chart lag + clicks); (2) the old [1.0, 1.05]
           clamp would destroy a legitimate step < 1 (48 kHz shared mixer).
           If this line ever reads far from 44,100 the ring is starving - fix the
           feed, never the step. */
        {
            double tnow = MsNow();
            UINT64 pos = (UINT64)g_fakePlayed;
            if (g_rsLastMs == 0.0) { g_rsLastPos = pos; g_rsLastMs = tnow; }
            else if (tnow - g_rsLastMs >= 10000.0)
            {
                double rate = (double)(pos - g_rsLastPos) / ((tnow - g_rsLastMs) / 1000.0);
                if (rate > 20000.0 && rate < 60000.0)
                    Log("[wo] tempo: fake-clock=%.0f fr/s (%.2f%% of real time) step=%.5f",
                        rate, 100.0 * rate / 44100.0, g_rsStep);
                g_rsLastPos = pos; g_rsLastMs = tnow;
            }
        }
        FakePoke();
    }
    InterlockedExchange(&g_woReading, 0);
    InterlockedExchange(&g_ringConsumer, 0);   /* the fake driver may drain again (P1-2) */
    if (g_woClient) g_woClient->Stop();
    InterlockedExchange(&g_woAlive, 0);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (InterlockedExchange(&g_woFault, 0))
        InterlockedExchange(&g_woState, 4);   /* claimable again -> watchdog rebuilds */
    Log("[wo] thread exit");
    return 0;
}

/* ---- exclusive PUSH writer (the "exclusive plan") -------------------------------
 * EVENTCALLBACK gives exclusive a buffer exactly one period deep, so the deadline
 * after every event is zero and any wake-up that misses the period loses audio:
 * measured 96.5 % while completely idle, with the device clock itself 3.5 % slow
 * (the engine skips the beat rather than substituting silence). WoInit therefore
 * opens exclusive WITHOUT EVENTCALLBACK - the buffer is then real (20 ms requested,
 * 80 ms / 3528 frames granted here) - and this thread tops it up every millisecond.
 *
 * Control variable: the audio not yet heard = device fill + ring backlog, held at
 * g_exclTargetFr. A device-fill floor (g_exclFloorFr) is added on top of it, and it
 * is not optional: with the total alone, a ring that is backed up past the target
 * makes `need` negative forever, so the writer stops pulling, the device fill falls
 * to zero and the output goes silent with a perfectly full ring - the plan's
 * controller deadlocks there. The floor guarantees the device is always pulled
 * from first; the ring backlog is latency, not safety.
 *
 * Its own thread, so the shared backend's event loop (WoThread) is left exactly as
 * it was - the two backends no longer share a writer.
 */
static DWORD WINAPI WoPushThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    InterlockedExchange(&g_woAlive, 1);

    /* MMCSS + TIME_CRITICAL: worth ~2 points of delivery in the plan's data
       (95.6 % -> 98.5 % under six interfering threads). It is not what makes push
       work - the buffer depth is - but it is free. */
    DWORD mmIdx = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmIdx);
    if (mm)
    {
        if (AvSetMmThreadPriority(mm, AVRT_PRIORITY_CRITICAL))
            Log("[wo] MMCSS 'Pro Audio' + AVRT_PRIORITY_CRITICAL engaged");
        else
            Log("[wo] !! MMCSS registered but AvSetMmThreadPriority failed (err=%lu)", GetLastError());
    }
    else
        Log("[wo] !! AvSetMmThreadCharacteristics('Pro Audio') failed (err=%lu)", GetLastError());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    Log("[wo] push thread up (frames=%u float=%d target=%ld fr floor=%ld fr)",
        (unsigned)g_woFrames, g_woFloat, (long)g_exclTargetFr, (long)g_exclFloorFr);

    InterlockedExchange(&g_ringConsumer, 2);   /* claim the ring before Start() (P1-2) */
    if (FAILED(g_woClient->Start()))
    {
        Log("[wo] !! Start failed");
        InterlockedExchange(&g_woState, 4);
        InterlockedExchange(&g_woAlive, 0);
        InterlockedExchange(&g_ringConsumer, 0);
        if (mm) AvRevertMmThreadCharacteristics(mm);
        return 0;
    }
    InterlockedExchange(&g_woState, 3);
    InterlockedExchange(&g_woReading, 1);
    Log("[wo] output running (exclusive push)");

    int faults = 0;
    while (g_woRun)
    {
        if (InterlockedExchange(&g_rsFlushReq, 0)) RsFlush();
        Sleep(1);                       /* timeBeginPeriod(1) is in effect: ~1 ms */
        if (!g_woRun) break;

        UINT32 pad = 0;
        HRESULT hp = g_woClient->GetCurrentPadding(&pad);
        if (FAILED(hp))
        {
            Log("[wo] !! GetCurrentPadding hr=0x%08X %s -> backend is dead, restarting it",
                (unsigned)hp, HrBrief(hp));
            InterlockedExchange(&g_woFault, 1);
            break;
        }
        InterlockedExchange(&g_woPad, (LONG)pad);   /* the fake clock reads this (P1-1) */

        LONG64 inflight  = (LONG64)pad + (LONG64)((double)RingAvail() / g_rsStep);
        LONG64 need      = (LONG64)g_exclTargetFr - inflight;
        LONG64 floorNeed = (LONG64)g_exclFloorFr - (LONG64)pad;
        if (need < floorNeed) need = floorNeed;
        if (need <= 0) { FakePoke(); continue; }

        /* Write the whole `need` - not "only as much as the ring can produce".
           WoFill16/WoFillFloat zero the tail, so a short ring becomes silence
           instead of a short write, and that is exactly what keeps the DEVICE
           clock moving while the producer is quiet (song change, menu,
           shutdown). Capping `n` by the ring content looked harmless but let the
           device drain to zero the instant CRI stopped producing: the device
           clock then read 0 %, the monitor rolled the backend back to `shared`,
           and a SECOND audio path started mid-transition - audible, and
           pointless at exit. (RsPull resamples internally, so `n` here is output
           frames either way and no separate unit conversion is needed.) */
        UINT32 free_ = g_woFrames > pad ? g_woFrames - pad : 0;
        UINT32 n = (UINT32)need;
        if (n > free_) n = free_;
        if (!n) { FakePoke(); continue; }

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

        {
            static ULONGLONG last = 0;
            ULONGLONG now = GetTickCount64();
            if (now - last >= 5000)
            {
                last = now;
                Log("[out] push: pad=%ld fr ring=%u fr target=%ld fr -> in-flight %.1f ms (cbs=%ld und=%ld)",
                    (long)pad, RingAvail(), (long)g_exclTargetFr,
                    ((double)pad + (double)RingAvail() / g_rsStep) * 1000.0 / (double)g_woRate,
                    g_woCbs, g_woUnder);
            }
        }
        FakePoke();
    }
    InterlockedExchange(&g_woReading, 0);
    InterlockedExchange(&g_ringConsumer, 0);   /* the fake driver may drain again (P1-2) */
    if (g_woClient) g_woClient->Stop();
    InterlockedExchange(&g_woAlive, 0);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    if (InterlockedExchange(&g_woFault, 0))
        InterlockedExchange(&g_woState, 4);   /* claimable again -> watchdog rebuilds */
    Log("[wo] push thread exit");
    return 0;
}

/* Synchronous init: must claim the device before CRI creates stream2. */
static const GUID kSubFloat =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static BOOL WoInit(int exclusive)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* ignore RPC_E_CHANGED_MODE */
    InterlockedExchange(&g_woExclusive, exclusive ? 1 : 0);   /* WoThread: MMCSS only for exclusive */

    /* Restart after a device loss: the previous stream is dead, but if a stuck
       writer thread still owns it, leave it alone - leak, do not crash. */
    if (!g_woAlive)
    {
        if (g_woRender) { g_woRender->Release(); g_woRender = NULL; }
        if (g_woClock)  { g_woClock->Release();  g_woClock = NULL; }
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
        /* Try more than one exclusive format (P1-6). Only PCM16/44.1 k was ever
           offered, so every endpoint that answers 48 kHz or float in exclusive
           mode made `output = excl` fail for good - which read as "exclusive does
           not work on this machine" when it was only the format we asked for.
           The writer understands exactly two sample formats (16-bit integer and
           32-bit float), so those are what we offer, at 44.1 kHz first (no
           resampling) and then at the device's own mix rate (g_rsStep already
           handles the rate difference). */
        UINT32 mixRate = 44100;
        {
            WAVEFORMATEX* mf = NULL;
            if (SUCCEEDED(g_woClient->GetMixFormat(&mf)) && mf)
            { mixRate = mf->nSamplesPerSec; CoTaskMemFree(mf); }
        }
        struct ExclFmt { UINT32 rate; WORD bits; BOOL flt; };
        const ExclFmt cand[4] = {
            { 44100,   16, FALSE },
            { 44100,   32, TRUE  },
            { mixRate, 16, FALSE },
            { mixRate, 32, TRUE  },
        };
        for (int ci = 0; ci < 4 && !wf; ci++)
        {
            wf = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
            if (!wf) goto done;
            memset(wf, 0, sizeof(WAVEFORMATEX));
            wf->wFormatTag      = (WORD)(cand[ci].flt ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
            wf->nChannels       = 2;
            wf->nSamplesPerSec  = cand[ci].rate;
            wf->wBitsPerSample  = cand[ci].bits;
            wf->nBlockAlign     = (WORD)(2 * (cand[ci].bits / 8));
            wf->nAvgBytesPerSec = cand[ci].rate * wf->nBlockAlign;
            wf->cbSize          = 0;
            WAVEFORMATEX* closest = NULL;
            HRESULT chk = g_woClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wf, &closest);
            if (closest) CoTaskMemFree(closest);
            if (chk == S_OK)
            {
                rate      = cand[ci].rate;
                g_woFloat = cand[ci].flt ? 1 : 0;
                Log("[wo] exclusive format accepted: %u Hz / %u-bit %s",
                    rate, (unsigned)cand[ci].bits, cand[ci].flt ? "float" : "int PCM");
                break;
            }
            Log("[wo] exclusive %u Hz / %u-bit %s rejected hr=0x%08X - trying the next format",
                cand[ci].rate, (unsigned)cand[ci].bits,
                cand[ci].flt ? "float" : "int PCM", (unsigned)chk);
            CoTaskMemFree(wf);
            wf = NULL;
        }
        if (!wf)
        {
            Log("[wo] !! no exclusive format accepted (tried 44.1k and mix rate, int16 and float32)");
            goto done;
        }
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
    DWORD initFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (exclusive)
    {
        /* PUSH mode, not EVENTCALLBACK (the exclusive plan). Event mode forces
           hnsBufferDuration == hnsPeriodicity - a one-period buffer - and that is
           exactly why exclusive could never reach real time: the deadline after
           each event is zero, so every wake-up that misses the period loses audio.
           Measured 96.5 % while completely idle, with the device clock itself
           3.5 % slow (the engine skips the beat rather than substituting silence).
           Without EVENTCALLBACK the buffer is real - the engine rounds the request
           up (20 ms lands at 80 ms / 3528 frames here) - and the push writer
           (WoPushThread) tops it up to g_exclTargetMs every millisecond.
           excl_period_ms, if set, is the requested depth: a real knob now that the
           period no longer describes a deadline we have to hit. */
        REFERENCE_TIME want = 200000;                        /* 20 ms default */
        if (g_exclPeriodMs > 0)
        {
            want = (REFERENCE_TIME)g_exclPeriodMs * 10000;
            Log("[wo] excl_period_ms=%d -> requested exclusive buffer depth %d ms",
                g_exclPeriodMs, g_exclPeriodMs);
        }
        if (want < 100000) want = 100000;
        if (want > 1000000) want = 1000000;                   /* cap at 100 ms */
        dur = want;
        per = 0;                                             /* ignored without EVENTCALLBACK */
        initFlags = 0;
    }
    else
    {
        REFERENCE_TIME def = 0, mn = 0;
        if (FAILED(g_woClient->GetDevicePeriod(&def, &mn)) || def <= 0)
            def = 100000;   /* 10 ms fallback */
        dur = def;
        per = def;
    }
    hr = g_woClient->Initialize(exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
                                initFlags, dur, per, wf, NULL);
    if (FAILED(hr))
    {
        Log("[wo] !! Initialize hr=0x%08X %s (tag=0x%X %u Hz %u bit %u ch cbSize=%u)",
            (unsigned)hr, HrBrief(hr), wf->wFormatTag, wf->nSamplesPerSec,
            wf->wBitsPerSample, wf->nChannels, wf->cbSize);
        goto done;
    }
    if (FAILED(g_woClient->GetBufferSize(&g_woFrames)))
    { Log("[wo] !! GetBufferSize failed"); goto done; }

    /* The event is only meaningful for the EVENTCALLBACK (shared) stream; the
       exclusive push stream is polled, and SetEventHandle on a non-event stream
       would fail with AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED. */
    if (!exclusive)
    {
        g_woEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!g_woEvent || FAILED(g_woClient->SetEventHandle(g_woEvent)))
        { Log("[wo] !! event setup failed"); goto done; }
    }
    if (FAILED(g_woClient->GetService(__uuidof(IAudioRenderClient), (void**)&g_woRender)) || !g_woRender)
    { Log("[wo] !! GetService render failed"); goto done; }

    if (exclusive)
    {
        /* Push-mode fill targets, in OUTPUT frames. g_exclTargetMs is the total
           not-yet-heard audio the writer aims at; the floor keeps the device
           itself from ever going thin when the ring is backed up (ring backlog
           counts as latency, not as safety). Both are capped to the granted
           buffer, leaving ~10 ms of head room so the pad can always grow. */
        g_woRate = (LONG)rate;
        LONG tgt = (LONG)((double)g_exclTargetMs * (double)rate / 1000.0);
        LONG flo = (LONG)(6.0 * (double)rate / 1000.0);
        LONG cap = (LONG)g_woFrames - (LONG)rate / 100;
        if (cap < 1) cap = (LONG)g_woFrames;
        if (tgt > cap) tgt = cap;
        if (flo > cap) flo = cap;
        InterlockedExchange(&g_exclTargetFr, tgt);
        InterlockedExchange(&g_exclFloorFr, flo);
    }

    if (exclusive)
    {
        /* Opened for the exclusive self-test, which reads the device's own position
           as a second opinion on the delivered rate; the steady-state tempo monitor
           in WoThread follows the fake clock instead, so nothing else reads it.
           GetFrequency() normalises its units, so the rate comes out in frames/s
           even on a driver whose clock does not tick in samples. */
        g_woClockFreq = 0;
        if (SUCCEEDED(g_woClient->GetService(__uuidof(IAudioClock), (void**)&g_woClock)) && g_woClock)
        {
            g_woClock->GetFrequency(&g_woClockFreq);
            Log("[wo] device clock: freq=%llu units/s", (unsigned long long)g_woClockFreq);
        }
        else
        {
            g_woClock = NULL;
            Log("[wo] !! no IAudioClock - the exclusive self-test falls back to the written rate");
        }
    }

    if (exclusive)
    {
        /* Push mode has no event to wait for, so the engine does not need a full
           buffer to start - but without any data the first milliseconds would be
           a real underrun while the writer ramps up. Prime exactly the target
           fill: the plan's data shows a partial prefill is enough, and it keeps
           the fixed startup offset small (~12 ms, inaudible). */
        UINT32 pre0 = (UINT32)g_exclTargetFr;
        if (pre0 < 1) pre0 = 1;
        if (pre0 > g_woFrames) pre0 = g_woFrames;
        BYTE* pre = NULL;
        if (SUCCEEDED(g_woRender->GetBuffer(pre0, &pre)) && pre)
        {
            memset(pre, 0, (size_t)pre0 * wf->nBlockAlign);
            g_woRender->ReleaseBuffer(pre0, 0);
            Log("[wo] exclusive pre-filled %u frames of silence (push target fill)", pre0);
        }
    }

    g_woCh = wf->nChannels;
    RsReset();
    g_rsStep = 44100.0 / (double)rate;     /* 1.0 on a 44.1 kHz endpoint */
    g_rsLastMs = 0.0;                      /* tempo monitor re-baselines on the next loop */
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
        if (g_woClock)  { g_woClock->Release();  g_woClock = NULL; }
        if (g_woClient) { g_woClient->Release(); g_woClient = NULL; }
        if (g_woEvent)  { CloseHandle(g_woEvent); g_woEvent = NULL; }
    }
    if (wf) CoTaskMemFree(wf);
    if (dev) dev->Release();
    if (en) en->Release();
    return ok;
}

/* Free the stream objects; stop the writer thread first and only once it has
   really left its loop (freeing under it was the v13.0 use-after-free). No
   ownership check in here - every caller must already own the stream (it won
   the 3 -> 4 claim, or it claimed "starting" in WoStart and saw the start
   fail). WoShutdown() below is the ownership-checked entry point. */
static void WoFreeStream(void)
{
    InterlockedExchange(&g_woRun, 0);
    for (int i = 0; i < 400 && g_woAlive; i++) Sleep(5);   /* blocks up to 2 s */
    if (g_woAlive)
    {
        Log("[wo] !! thread did not stop within 2 s - leaking the stream objects instead of freeing them");
        InterlockedExchange(&g_woState, 4);
        return;                       /* keep g_ringConsumer set: the leaked thread
                                         is still reading the ring (N-P1-5) */
    }
    /* Released only now that the writer really stopped (N-P1-5). Clearing this
       before the check above let FakeDriverThread start draining the same ring
       the leaked thread was still reading - two consumers on one ring. */
    InterlockedExchange(&g_ringConsumer, 0);
    if (g_woClient) g_woClient->Stop();
    if (g_woRender) { g_woRender->Release(); g_woRender = NULL; }
    if (g_woClock)  { g_woClock->Release();  g_woClock = NULL; }
    if (g_woClient) { g_woClient->Release(); g_woClient = NULL; }
    if (g_woEvent)  { CloseHandle(g_woEvent); g_woEvent = NULL; }
    InterlockedExchange(&g_woState, 0);
}

/* Tear the stream down, but only for the caller that owns it (P0-4). The
   objects used to have no single owner: WoStart's failure paths, AsioStartThread
   and the exclusive self-test could all run the teardown at once, and a tear-down
   racing WoInit - state 2, g_woAlive still 0 - saw "the thread never started" and
   went straight to Stop() + Release() on the g_woClient WoInit was still building,
   handing a dangling pointer to the Initialize() call that followed. Claiming the
   running stream with a 3 -> 4 compare-exchange serialises it: only the caller that
   actually wins may free. Any other caller has nothing to own and returns. */
static BOOL WoShutdown(void)
{
    LONG prev = InterlockedCompareExchange(&g_woState, 4, 3);
    if (prev != 3)
    {
        Log("[wo] shutdown skipped: not the owner (state was %ld)", prev);
        return FALSE;
    }
    WoFreeStream();
    return TRUE;
}

/* Init + start. exclusive==1 -> exclusive, else shared. Returns as soon as the
   stream is actually running; the exclusive gate (a ~2 s self-test, rollback to
   shared when the device turns out to be dead) runs asynchronously from
   WatchdogThread - see ExclSelfTestTick. Never calibrate the resample step from
   that reading: step stays at its format-derived value (44100 / device rate). */
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
           (audit R4). That is deliberate: a session that fails the gate is a
           genuinely dead/paused device (0 %), and exclusive is not a
           recommended path. */
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
    /* Clear the device-fill reading too (audit): the fake clock subtracts it (P1-1)
       and a value left over from the previous stream would drag the reported
       position backwards until the new writer publishes its first padding sample. */
    InterlockedExchange(&g_woPad, 0);
    InterlockedExchange(&g_woRun, 1);
    HANDLE t = CreateThread(NULL, 0, exclusive ? WoPushThread : WoThread, NULL, 0, NULL);
    /* WoFreeStream leaves the state at 0, and WoWatchdog only rebuilds from 4 - so
       every failure exit from here must land on 4 or the process is left with no
       audio backend and no retry (the same trap R-P0-1 was). Compare the WoInit
       failure path just above, which does exactly this. */
    if (!t) { Log("[wo] !! CreateThread failed"); WoFreeStream(); InterlockedExchange(&g_woState, 4); return FALSE; }
    CloseHandle(t);

    /* Wait for WoThread to report success (3) or failure (4): reporting success
       straight after CreateThread hid Start() failures and blocked the fallback. */
    for (int i = 0; i < 200 && g_woState == 2; i++) Sleep(5);
    if (g_woState != 3)
    {
        Log("[wo] !! output did not start (%s)", exclusive ? "exclusive" : "shared");
        WoFreeStream();   /* this call still owns the "starting" claim */
        InterlockedExchange(&g_woState, 4);   /* ...and 4 is what tells WoWatchdog to retry */
        return FALSE;
    }

    /* The exclusive self-test used to run right here - 200 ms of ramp plus 2 s of
       measurement, on the thread Hook_Initialize calls us from. That blocked the
       game's audio init and left the title screen silent for 2.2 s on every launch
       with output=excl. It now runs from WatchdogThread, overlapped with playback:
       see ExclSelfTestTick(). WoStart() only has to bring the stream up. */
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

    /* Our OWN output stream is never the game's stream1 (N-P0-1). WoInit stores
       the client in g_woClient BEFORE calling Initialize, so identity is exact -
       and it has to be tested, because the format rule below cannot tell our
       shared stream from CRI's: on a 16-bit / 44.1 kHz endpoint mix format,
       WoInit(shared) used to take over its OWN client (GetBufferSize became our
       fake 441, GetService(IAudioRenderClient) returned g_fakeRc), so the shared
       backend fed CRI's PCM into g_fake.stage and no real stream existed at all -
       silence, no error, and it happens on every excl -> shared rollback. */
    if (self == g_woClient)
        return g_orig(self, shareMode, flags, hnsBufferDuration, hnsPeriodicity,
                      pFormat, audioSessionGuid);

    if (shareMode == AUDCLNT_SHAREMODE_SHARED && pFormat)
    {
        /* stream1 vs stream2 is decided by the FORMAT, not by the exact flags
           word (P1-4). Matching `flags == 0x00140000` exactly meant the takeover
           silently stopped working the day CRI sent one more flag: no error, no
           fake client, just a passthrough line and the 22 ms engine buffer back.
           The two streams are unambiguous by format - stream1 is the only
           44.1 kHz / 16-bit / stereo one, stream2 is 32-bit with a long duration.
           The flags word is still logged (above) so a regression is visible. */
        if (pFormat->nSamplesPerSec == 44100 && pFormat->wBitsPerSample == 16 && pFormat->nChannels == 2)
        {
            fixed1 = 1;

            if (g_fakeClient)
            {
                fake = 1;                 /* v13: CRI gets our client, no real stream */
            }
            else
            {
                /* Buffer trim only: drop the flag that made the engine grant
                   44.5 ms (44.5 -> 22.0 ms). */
                flags = 0x00040000u;
            }
        }
        else if (pFormat->wBitsPerSample == 32 && hnsBufferDuration >= 600000)
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
    if (fixed2) Log("stream2 FIX: duration -> 166666 (was >=600000)  hr=0x%08X %s%s",
                    (unsigned)hr, HrBrief(hr),
                    hr == AUDCLNT_E_DEVICE_IN_USE
                        ? " (expected while an exclusive stream holds the device - not a failure)"
                        : "");
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
                else if (_stricmp(key, "excl_period_ms") == 0) g_exclPeriodMs = val;
                else if (_stricmp(key, "excl_target_ms") == 0) g_exclTargetMs = val;
                else Log("config: unknown key '%s' ignored", key);
            }
        }
    }

    if (g_asioBlock < 16) g_asioBlock = 16;
    if (g_asioBlock > 1024) g_asioBlock = 1024;
    if (g_handoffMs < 0) g_handoffMs = 0;
    if (g_handoffMs > 150) g_handoffMs = 150;
    if (g_exclPeriodMs < 0) g_exclPeriodMs = 0;
    if (g_exclPeriodMs > 100) g_exclPeriodMs = 100;
    if (g_exclTargetMs < 6) g_exclTargetMs = 6;      /* below ~6 ms the floor is gone */
    if (g_exclTargetMs > 30) g_exclTargetMs = 30;
    g_exclTargetCfg = g_exclTargetMs;                /* remember what the user asked for */
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
    if (g_outMode == 1)
        Log("config: exclusive period = %d ms (%s)", g_exclPeriodMs,
            g_exclPeriodMs > 0 ? "override" : "device default");
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

/* Move the exclusive fill target. Called from the watchdog, read by WoPushThread. */
static void SetExclTarget(int ms)
{
    /* Never below what was configured: the adaptation is a rescue for a starving
       writer, not a way to quietly make the user's setting lower. */
    if (ms < g_exclTargetCfg) ms = g_exclTargetCfg;
    if (ms > 30) ms = 30;
    if (ms == g_exclTargetMs) return;
    g_exclTargetMs = ms;
    LONG rate = g_woRate; if (rate <= 0) rate = 44100;
    LONG tgt = (LONG)((double)ms * (double)rate / 1000.0);
    LONG cap = (LONG)g_woFrames - rate / 100;
    if (cap < 1) cap = (LONG)g_woFrames;
    if (tgt > cap) tgt = cap;
    InterlockedExchange(&g_exclTargetFr, tgt);
    Log("[wo] exclusive fill target -> %d ms (%ld fr)", ms, tgt);
}

/* --------------- exclusive monitor, run by the watchdog ------------------------
 * The 2 s window with a 90 % bar judged the EVENTCALLBACK backend, and that bar
 * was calibrated against a readout that does not mean what it looks like: 96 %
 * there is not jitter, the device clock itself runs 3.5 % slow because the engine
 * skips the beat it is not fed, so the whole timeline (music and chart together)
 * drifts against real time. A 90 % gate happily passes exactly that.
 *
 * Push mode is built to sit at ~99.9 %, so the gate is now the device clock against
 * the wall clock at 99.5 %, sampled once a second and only acted on after three
 * consecutive bad samples (one hitched second is not a broken device). The same
 * reading drives the fill target: below 99.9 % the writer is being starved, so the
 * target climbs 2 ms (capped at 30); thirty clean seconds let it come back down.
 *
 * Teardown happens on this thread, the same one WoWatchdog rebuilds a shared stream
 * on the next tick, so the backend is never reconfigured from two threads at once.
 * Never feed this number to the resample step: step stays at its format-derived
 * value (44100 / negotiated device rate). */
static void ExclSelfTestTick(void)
{
    static int       bad = 0, clean = 0, starve = 0, n = 0;
    static UINT64    p0 = 0;
    static LARGE_INTEGER t0q = { 0 };   /* QPC, not GetTickCount64 - see below */
    static LONG      lastUnder = 0;     /* g_woUnder at the previous sample (N-P1-4) */
    static int       armed = 0;

    if (!g_woExclusive) { armed = 0; bad = 0; clean = 0; starve = 0; return; }
    /* Leaving state 3 must reset the ADAPTATION counters too, not just the
       judge's. A `starve` left at 1 when the backend dropped out made the first
       starving sample after a restart reach 2 immediately, so SetExclTarget()
       raised the fill target on a single sample - bypassing the "two in a row"
       guard. `clean`/`n` leaked the same way (a stale count could shrink the
       target, or just shift the log cadence). This branch is reached on the tick
       BETWEEN a fault and WoWatchdog's rebuild, i.e. while g_woExclusive is still
       set, so the full reset in the branches above does not cover it. */
    if (g_woState != 3) { armed = 0; bad = 0; clean = 0; starve = 0; n = 0; return; }
    if (!g_woClock) return;              /* no device clock: nothing honest to judge on */
    /* CRI has stopped its stream (Fake_Stop: song change, exit, shutdown), so the
       producer is gone and the device is legitimately quiet for a moment. That is
       not a dead device - judging it as one rolled the backend back to `shared`
       mid-transition, and at exit it built a whole second audio path (which is
       what made the exit audible twice). Skip the sample and re-baseline, so the
       first window after a restart is clean. Same reasoning as the ASIO path's
       underrun-grace window. */
    if (!g_fake.running) { armed = 0; bad = 0; clean = 0; starve = 0; return; }

    if (!armed)
    {
        UINT64 pos = 0;
        if (FAILED(g_woClock->GetPosition(&pos, NULL))) return;
        QueryPerformanceCounter(&t0q);
        p0 = pos;
        lastUnder = (LONG)g_woUnder;   /* the silence count is baselined here (N-P1-4) */
        armed = 1;
        Log("[wo] exclusive monitor armed (device clock vs wall clock, gate 99.5%%)");
        return;
    }

    /* Time the window with QPC, NOT GetTickCount64. GetTickCount64 runs at the
       system timer resolution (~15.6 ms), so a ~1 s window carries up to ±1.6 %
       of quantisation error - far wider than the deviation being measured. That
       is what made the fill target RATCHET: a sample that read 99.99 % instead of
       100.00 % looked like starvation and added 2 ms, while only thirty clean
       seconds gave 1 ms back. Over a couple of minutes the target - and with it
       the whole audio latency - walked from 12 ms to 24 ms, i.e. the music fell
       progressively further behind the chart. */
    LARGE_INTEGER nowc; QueryPerformanceCounter(&nowc);
    double secs = (double)(nowc.QuadPart - t0q.QuadPart) / (double)g_qpcFreq.QuadPart;
    if (secs < 1.0) return;              /* one sample a second; the tick may be longer */

    UINT64 p1 = 0;
    if (FAILED(g_woClock->GetPosition(&p1, NULL))) { armed = 0; return; }
    const double scale = (g_woClockFreq > 0) ? (44100.0 / (double)g_woClockFreq) : 1.0;
    double ratio = (double)(p1 - p0) * scale / (secs * 44100.0);   /* clock units -> frames */
    p0 = p1; t0q = nowc;

    /* Starvation is a two-signal test (N-P1-4). The device clock alone cannot see
       a ring that ran dry: the push writer fills the shortfall with SILENCE, so
       the device keeps consuming and `ratio` sits at 100 % even while the producer
       is late (a long load, a decoding stall, CRI's thread being starved). What
       records it is g_woUnder, the count of those silence top-ups. */
    LONG du = (LONG)(g_woUnder - lastUnder);
    lastUnder = (LONG)g_woUnder;
    /* ...but only while the producer is really WRITING (R-P1-1). g_woUnder counts
       top-ups per CALL, and the push writer iterates every millisecond, so a
       producer that merely PAUSED - a load, a menu, a scene change, CRI not mixing
       - drove du to ~1000/s and looked starved; three such samples rolled the
       backend back. `producerBusy` is the same guard the ASIO path has always
       used, and it subsumes the old g_fake.running test (after Fake_Stop nothing
       is written, so nothing is starved either). */
    LONG64 lw = g_lastWriteTick;
    int producerBusy = ((ULONGLONG)GetTickCount64() - (ULONGLONG)lw) < 200;
    int starving = (ratio < 0.995) || (producerBusy && du > 20);

    /* Adapt the fill target first: a starved writer is fixable without tearing the
       backend down, and that is the whole point of a target instead of a fixed fill.
       Only a clearly starving clock may raise the target, and only after two
       samples in a row agree; anything else decays back toward the configured
       value after ten clean seconds. The previous pair of thresholds (grow below
       0.999, shrink after thirty clean seconds) could only ever walk the target
       UP, and 0.999 sits inside the measurement noise. */
    if (starving)
    {
        clean = 0;
        if (++starve >= 2 && g_exclTargetMs < 30) { starve = 0; SetExclTarget(g_exclTargetMs + 2); }
    }
    else
    {
        starve = 0;
        if (++clean >= 10 && g_exclTargetMs > g_exclTargetCfg) { clean = 0; SetExclTarget(g_exclTargetMs - 1); }
    }

    if (++n >= 5)                        /* not every second - keep the log readable */
    {
        n = 0;
        Log("[wo] exclusive clock: %.2f%% of real time (target %d ms, %ld silence top-ups/s)%s",
            ratio * 100.0, g_exclTargetMs, du, starving ? " [starving]" : "");
    }

    if (starving)
    {
        if (++bad >= 3)
        {
            Log("[wo] !! exclusive writer starved for %d s (device clock %.2f%%, %ld silence top-ups/s) - rolling back to shared",
                bad, ratio * 100.0, du);
            bad = 0; armed = 0;
            /* Only touch the state if the CAS actually gave us the teardown
               (N-P1-6): writing 4 unconditionally would cut short somebody else's
               claim - they may be in state 2, still building the stream. */
            if (WoShutdown())
            {
                /* This write is NOT redundant (R-P0-1): WoFreeStream leaves the
                   state at 0 and WoWatchdog only rebuilds from 4, so this IS the
                   "rebuild shared" request. Without it the process had no audio
                   backend at all after a rollback - and CRI only re-runs
                   Initialize when it creates a NEW stream1, while song changes
                   reuse Stop()/Start() on the same client - so the session stayed
                   silent until restart. Conditional, so a caller that lost the
                   CAS still cannot clobber somebody else's claim (N-P1-6). */
                InterlockedExchange(&g_woState, 4);
                InterlockedExchange(&g_woCbs, 0);
                InterlockedExchange(&g_woUnder, 0);
                lastUnder = 0;
            }
            return;
        }
    }
    else bad = 0;
}

static DWORD WINAPI WatchdogThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    /* no hard iteration cap: this used to stop after an hour, after which any
       later anomaly went unrecorded */
    for (long long i = 1; ; i++)
    {
        /* Woken by the event (DllMain sets it on detach) or after 1 s. */
        if (g_wdEvent) WaitForSingleObject(g_wdEvent, 1000);
        else           Sleep(1000);

        if (g_detachReq)
        {
            /* The module is being unloaded (P0-2). Unregistering the endpoint
               notification client is a COM call, so it happens HERE and not in
               DllMain: DllMain runs under the loader lock, and a COM call from
               there can deadlock the unload. This thread is already COM
               apartment-initialised and belongs to us, so it is the safe place. */
            if (g_notifier && g_notifyEnum)
            {
                g_notifyEnum->UnregisterEndpointNotificationCallback(g_notifier);
                g_notifier->Release();
                g_notifier = NULL;
            }
            if (g_notifyEnum) { g_notifyEnum->Release(); g_notifyEnum = NULL; }
            InterlockedExchange(&g_detachDone, 1);
            return 0;
        }

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

        /* The exclusive self-test comes first: if it fails it sets state 4, which
           WoWatchdog on this very tick would otherwise have to be woken for. */
        ExclSelfTestTick();
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
    (void)hinstDLL;
    if (fdwReason == DLL_PROCESS_DETACH)
    {
        /* P0-2: give the game its vtables back before this module is unmapped.
           Every hook slot we installed (IAudioClient [3][13][14], the fake
           client [4..12], the render client [3][4]) is written back to its
           original. Without this, an unload leaves the game calling into freed
           memory on the next Initialize / GetService / device event. Only
           VirtualProtect + a pointer store, so it is loader-lock-safe. */
        RestorePatches();

        /* The endpoint-notification client is a COM object living in this module;
           it has to be unregistered before the unload. That is a COM call, which
           is why the watchdog does it instead of us - see WatchdogThread. Ask it
           to, and give it a moment on a real FreeLibrary (a process exit has
           already gone as far as the threads are concerned). */
        InterlockedExchange(&g_detachReq, 1);
        if (g_wdEvent) SetEvent(g_wdEvent);

        /* Signal the worker threads to stop. Fake_Stop cannot do it (CRI reuses
           stream1 across songs, so WoThread has to stay up), so this is the place.
           Never call into COM from the process-exit path: with the loader lock
           held and a writer that may be stuck, waiting or Stop() can deadlock the
           exit itself. The OS kills the threads and reclaims the WASAPI stream
           once the process handle closes. */
        InterlockedExchange(&g_woRun, 0);
        InterlockedExchange(&g_fake.running, 0);
        if (!lpvReserved)
        {
            /* Wait for the watchdog to confirm the unregister: returning before
               it is done would unmap the code it is about to run. Bounded, but
               the watchdog wakes on g_wdEvent and does nothing else first. */
            for (int i = 0; i < 400 && !g_detachDone; i++) Sleep(5);
            Sleep(20);   /* give the other threads a moment */
        }

        /* timeBeginPeriod(1) raises the timer resolution for the WHOLE system,
           so it has to be given back even when the process is going away
           (audit N11). */
        /* The log is buffered now (P2-1), so flush what is still pending before
           the process goes away - otherwise the last ~200 ms are lost, which is
           exactly the part that describes an exit or a crash. */
        if (g_log) fflush(g_log);

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
    Log(" audio_hook v14.0 (fake stream1 client: no engine buffer, device free)");
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
        if (FAILED(nr) && g_notifier) { g_notifier->Release(); g_notifier = NULL; }
    }

    /* Wakes the watchdog out of its 1 s sleep when DllMain asks it to unregister
       the notification client on unload (P0-2). */
    if (!g_wdEvent) g_wdEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    c->Release();
    dev->Release();
    /* Keep the enumerator referenced (P0-2): the notification callback lives in
       this module and has to be unregistered before it is unmapped - that call
       needs a live enumerator. The watchdog releases it on detach, not here. */
    g_notifyEnum = en;

    /* Pin this module for the life of the process (N-P1-1). RestorePatches() only
       guarantees that the NEXT call reaches the original - it cannot help a thread
       that is already inside our fake methods, and CRI's mixing thread sits in
       FakeRC::ReleaseBuffer about 100 times a second. A FreeLibrary() would unmap
       the code out from under it, and the 2 s wait in DllMain runs under the
       loader lock, where the watchdog may itself be blocked acquiring that lock to
       load a COM module. Pinning makes the module's refcount never reach zero, so
       detach never runs at all; RestorePatches stays as a second line of defence. */
    HMODULE pinned = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)&g_log, &pinned))
        Log("[init] !! module pin failed (err=%lu) - unload safety rests on RestorePatches",
            (unsigned long)GetLastError());

    HANDLE th = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    if (th) CloseHandle(th);

    Log("ready. waiting for the game to create its audio streams...");
}
