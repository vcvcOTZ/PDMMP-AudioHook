/*
 * session_tool.cpp -- list audio sessions + mute state; unmute/mute by name.
 *
 * Usage:
 *   session_tool                  list all render sessions
 *                                 (device, pid, process, state, muted, volume, peak)
 *   session_tool --unmute <sub>   clear mute on every session whose process name
 *                                 contains <sub> (case-insensitive)
 *   session_tool --mute <sub>     set mute (diagnostic only)
 *   session_tool --list           explicit list (same as no arguments)
 *   session_tool --help
 *
 * Exit codes:
 *   0  listed OK / at least one session was changed
 *   1  COM or endpoint enumeration failure
 *   2  --unmute/--mute matched no session
 *   3  bad command line
 *
 * Why the extra checks: an earlier build silently did nothing when the process
 * name case did not match (Windows names are case-insensitive), and returned 0
 * either way, which made "the game is still muted" impossible to diagnose.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <psapi.h>

/* IAudioMeterInformation {C02216F6-8C67-4B5B-9D00-D008E73E0064} -- declared locally to
   avoid pulling endpointvolume.h (it clashes with the MinGW/winsdk mix used here). */
static const GUID kIID_IAudioMeterInformation =
{ 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };
struct IAudioMeterMin : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
};

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "psapi.lib")

static FILE* g_report = NULL;

static void Out(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_report) { va_start(ap, fmt); vfprintf(g_report, fmt, ap); va_end(ap); }
    fflush(stdout);
}

static int IsConsole(void)
{
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) ? 1 : 0;
}

/* Device names are UTF-8 in the report and on a UTF-8 console; CP_ACP turned
   every non-ASCII name (e.g. Chinese "扬声器") into question marks. */
static void WideToUtf8(LPCWSTR src, char* out, size_t cch)
{
    out[0] = 0;
    if (src && cch) WideCharToMultiByte(CP_UTF8, 0, src, -1, out, (int)cch - 1, NULL, NULL);
}

/* ASCII case-insensitive substring search (process names are case-insensitive
   on Windows, so "divamegamix" must match "DivaMegaMix.exe"). */
static int ContainsNoCase(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return 0;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return 1;
    return 0;
}

static void GetProcName(DWORD pid, char* out, size_t cch)
{
    if (!cch) return;
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
    {
        DWORD e = GetLastError();
        strncpy_s(out, cch, (e == ERROR_ACCESS_DENIED) ? "(denied)" : "(gone)", _TRUNCATE);
        return;
    }
    /* W + UTF-8: the ANSI variant turned a non-ASCII exe name into '?', which
       made --unmute <name> impossible to match on such installs */
    wchar_t wpath[MAX_PATH] = { 0 };
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(h, 0, wpath, &n) || n == 0)
        strncpy_s(out, cch, "?", _TRUNCATE);
    else
        WideToUtf8(wpath, out, cch);
    CloseHandle(h);
    const char* bs = strrchr(out, '\\');
    if (bs) memmove(out, bs + 1, strlen(bs + 1) + 1);
}

/* The report goes next to the exe, not into whatever directory the user
   happened to start the tool from (Program Files is read-only for non-admins).
   Append + 4 MB rotation, same rule as audio_hook.log: the run that matters is
   usually the one that just failed. */
static void OpenReport(void)
{
    char path[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    else path[0] = '\0';

    char file[MAX_PATH] = { 0 };
    strncpy_s(file, sizeof(file), path, _TRUNCATE);
    strncat_s(file, sizeof(file), "session_tool_report.txt", _TRUNCATE);

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(file, GetFileExInfoStandard, &fa) &&
        fa.nFileSizeHigh == 0 && fa.nFileSizeLow > (4u << 20))
    {
        char old[MAX_PATH] = { 0 };
        strncpy_s(old, sizeof(old), path, _TRUNCATE);
        strncat_s(old, sizeof(old), "session_tool_report.1.txt", _TRUNCATE);
        DeleteFileA(old);
        MoveFileA(file, old);
    }

    if (fopen_s(&g_report, file, "ab") != 0 || !g_report)
    {
        g_report = NULL;
        fprintf(stderr, "!! cannot write %s (read-only folder?) - console output only\n", file);
    }
}

static void Usage(void)
{
    printf("session_tool [--list] [--unmute <name> | --mute <name>] [--help]\n"
           "  <name> is a case-insensitive substring of the process name, e.g. DivaMegaMix\n");
}

int main(int argc, char** argv)
{
    const char* action = NULL;      /* NULL=list, "unmute", "mute" */
    const char* match  = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "--help") || !_stricmp(argv[i], "-h")) { Usage(); return 0; }
        else if (!_stricmp(argv[i], "--list")) { /* default action */ }
        else if (!_stricmp(argv[i], "--unmute") || !_stricmp(argv[i], "--mute"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "!! %s needs a process-name substring\n", argv[i]);
                Usage();
                return 3;
            }
            action = (argv[i][2] == 'u') ? "unmute" : "mute";
            match  = argv[++i];
        }
        else
        {
            fprintf(stderr, "!! unknown argument '%s'\n", argv[i]);
            Usage();
            return 3;
        }
    }

    if (IsConsole()) SetConsoleOutputCP(CP_UTF8);

    OpenReport();
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
    {
        Out("!! CoInitializeEx failed\n");
        if (g_report) fclose(g_report);
        return 1;
    }

    IMMDeviceEnumerator* en = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en)
    {
        Out("!! enumerator failed hr=0x%08X\n", (unsigned)hr);
        CoUninitialize();
        if (g_report) fclose(g_report);
        return 1;
    }

    IMMDeviceCollection* col = NULL;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr) || !col)
    {
        Out("!! EnumAudioEndpoints failed hr=0x%08X\n", (unsigned)hr);
        en->Release();
        CoUninitialize();
        if (g_report) fclose(g_report);
        return 1;
    }

    UINT32 devCnt = 0;
    if (FAILED(col->GetCount(&devCnt))) devCnt = 0;

    int matched = 0, acted = 0;
    for (UINT32 di = 0; di < devCnt; di++)
    {
        IMMDevice* dev = NULL;
        if (FAILED(col->Item(di, &dev)) || !dev) continue;

        char devName[256] = "?";
        char devId[128] = "";
        IPropertyStore* ps = NULL;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps)
        {
            PROPVARIANT pv; PropVariantInit(&pv);
            HRESULT gv = ps->GetValue(PKEY_Device_FriendlyName, &pv);
            if (SUCCEEDED(gv) && pv.vt == VT_LPWSTR)
                WideToUtf8(pv.pwszVal, devName, sizeof(devName));
            if (SUCCEEDED(gv)) PropVariantClear(&pv);   /* clear for ANY variant type */
            ps->Release();
        }
        LPWSTR did = NULL;
        if (SUCCEEDED(dev->GetId(&did)) && did)
        {
            char tmp[512] = { 0 };
            WideToUtf8(did, tmp, sizeof(tmp));
            const char* brace = strrchr(tmp, '{');
            strncpy_s(devId, sizeof(devId), brace ? brace : tmp, _TRUNCATE);
            CoTaskMemFree(did);
        }

        /* Always print a header: a failed GetId used to leave the sessions of
           this device listed under the previous device's name. */
        if (devId[0]) Out("== device '%s' [%s]\n", devName, devId);
        else          Out("== device '%s'\n", devName);

        IAudioSessionManager2* mgr = NULL;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&mgr)) || !mgr)
        {
            Out("    (cannot activate the session manager)\n");
            dev->Release();
            continue;
        }

        IAudioSessionEnumerator* se = NULL;
        if (FAILED(mgr->GetSessionEnumerator(&se)) || !se)
        {
            Out("    (no session enumerator)\n");
            mgr->Release();
            dev->Release();
            continue;
        }

        int n = 0;
        if (FAILED(se->GetCount(&n))) n = 0;
        for (int i = 0; i < n; i++)
        {
            IAudioSessionControl* ctl = NULL;
            if (FAILED(se->GetSession(i, &ctl)) || !ctl) continue;

            DWORD pid = 0;
            char proc[256] = "?";
            IAudioSessionControl2* ctl2 = NULL;
            if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&ctl2)) && ctl2)
            {
                ctl2->GetProcessId(&pid);
                if (pid) GetProcName(pid, proc, sizeof(proc));
                ctl2->Release();
            }

            AudioSessionState st = AudioSessionStateInactive;
            ctl->GetState(&st);
            const char* stn = (st == AudioSessionStateActive) ? "ACTIVE" :
                              (st == AudioSessionStateInactive) ? "Inactive" : "Expired";

            ISimpleAudioVolume* vol = NULL;
            BOOL muted = FALSE;
            float lvl = -1.0f;
            if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol)) && vol)
            {
                vol->GetMute(&muted);
                vol->GetMasterVolume(&lvl);
            }

            float peak = -1.0f;
            IAudioMeterMin* meter = NULL;
            if (SUCCEEDED(ctl->QueryInterface(kIID_IAudioMeterInformation, (void**)&meter)) && meter)
            {
                meter->GetPeakValue(&peak);
                meter->Release();
            }

            Out("    session pid=%-6lu %-28s [%s] muted=%s volume=%.2f peak=%.4f\n",
                (unsigned long)pid, proc, stn, muted ? "YES" : "no", lvl, peak);

            /* pid 0 is the system-sounds session; never touch it by name */
            if (action && match && pid && ContainsNoCase(proc, match))
            {
                matched++;
                if (vol)
                {
                    HRESULT hr2 = vol->SetMute((action[0] == 'u') ? FALSE : TRUE, NULL);
                    Out("    >>> %s '%s' -> hr=0x%08X\n", action, proc, (unsigned)hr2);
                    if (SUCCEEDED(hr2)) acted++;
                }
                else
                    Out("    >>> %s '%s' -> no ISimpleAudioVolume\n", action, proc);
            }

            if (vol) vol->Release();
            ctl->Release();
        }
        se->Release();
        mgr->Release();
        dev->Release();
    }

    col->Release();
    en->Release();
    CoUninitialize();

    if (action)
    {
        Out("=== %s done: %d matched, %d changed (pattern '%s') ===\n", action, matched, acted, match);
        if (!matched)
            Out("    hint: the game must be RUNNING and playing something; run with no\n"
                "          arguments to see every session name.\n");
    }
    else
        Out("=== %u device(s) listed ===\n", devCnt);

    if (g_report) fclose(g_report);
    if (action && !acted) return 2;
    return 0;
}
