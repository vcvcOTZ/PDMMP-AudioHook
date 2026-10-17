/*
 * session_tool.cpp -- list audio sessions + mute state; unmute/mute by name.
 * Build: same MSVC x64 setup as audio_probe. Pure ASCII output + report file.
 * Usage:
 *   session_tool                 -> list all render sessions (pid, name, state, muted, volume)
 *   session_tool --unmute <sub>  -> SetMute(FALSE) on every session whose process name contains <sub>
 *   session_tool --mute <sub>    -> SetMute(TRUE)  (diagnostic only)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
/* IAudioMeterInformation {C02216F6-8C67-4B5B-9D00-D008E73E0064} -- declared locally to
   avoid pulling endpointvolume.h (it clashes with the MinGW/winsdk mix used here). */
static const GUID kIID_IAudioMeterInformation =
{ 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };
struct IAudioMeterMin : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
};
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "psapi.lib")

static FILE* g_report = NULL;

static void Out(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_report) { va_start(ap, fmt); vfprintf(g_report, fmt, ap); va_end(ap); }
}

static void GetProcName(DWORD pid, char* out, size_t cch)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { strncpy_s(out, cch, "(gone)", _TRUNCATE); return; }
    DWORD n = (DWORD)cch - 1;
    if (!QueryFullProcessImageNameA(h, 0, out, &n))
        strncpy_s(out, cch, "?", _TRUNCATE);
    CloseHandle(h);
    const char* bs = strrchr(out, '\\');
    if (bs) memmove(out, bs + 1, strlen(bs + 1) + 1);
}

int main(int argc, char** argv)
{
    const char* action = NULL;      /* NULL=list, "unmute", "mute" */
    const char* match  = NULL;
    for (int i = 1; i < argc; i++)
    {
        if ((!strcmp(argv[i], "--unmute") || !strcmp(argv[i], "--mute")) && i + 1 < argc)
        {
            action = argv[i] + 2;   /* "unmute"/"mute" */
            match = argv[++i];
        }
    }

    fopen_s(&g_report, "session_tool_report.txt", "wb");
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { Out("!! enumerator failed\n"); if (g_report) fclose(g_report); return 1; }

    IMMDeviceCollection* col = NULL;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT32 devCnt = 0;
    if (col) col->GetCount(&devCnt);

    int acted = 0;
    for (UINT32 di = 0; di < devCnt; di++)
    {
        IMMDevice* dev = NULL;
        if (FAILED(col->Item(di, &dev)) || !dev) continue;

        LPWSTR did = NULL;
        char devName[256] = "?";
        IPropertyStore* ps = NULL;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps)
        {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            {
                WideCharToMultiByte(CP_ACP, 0, pv.pwszVal, -1, devName, sizeof(devName) - 1, NULL, NULL);
                PropVariantClear(&pv);
            }
            ps->Release();
        }
        if (SUCCEEDED(dev->GetId(&did)) && did)
        {
            char ida[64] = { 0 };
            WideCharToMultiByte(CP_ACP, 0, did, -1, ida, sizeof(ida) - 1, NULL, NULL);
            const char* brace = strrchr(ida, '{');
            Out("== device '%s' [%s]\n", devName, brace ? brace : ida);
            CoTaskMemFree(did);
        }

        IAudioSessionManager2* mgr = NULL;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&mgr)) || !mgr)
        {
            dev->Release();
            continue;
        }
        IAudioSessionEnumerator* se = NULL;
        if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se)
        {
            int n = 0;
            se->GetCount(&n);
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

                if (action && match && strstr(proc, match))
                {
                    if (vol)
                    {
                        HRESULT hr2 = vol->SetMute(action[0] == 'u' ? FALSE : TRUE, NULL);
                        Out("    >>> %s '%s' -> hr=0x%08X\n", action, proc, (unsigned)hr2);
                        if (SUCCEEDED(hr2)) acted++;
                    }
                }
                if (vol) vol->Release();
                ctl->Release();
            }
            se->Release();
        }
        mgr->Release();
        dev->Release();
    }
    if (col) col->Release();
    en->Release();
    CoUninitialize();

    if (action)
        Out("=== %s done: %d session(s) matched '%s' ===\n", action, acted, match);
    if (g_report) fclose(g_report);
    return 0;
}
