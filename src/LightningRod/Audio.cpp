#include "Audio.h"
#include "StrUtil.h"

#include <objbase.h>
#include <mmsystem.h>
#include <cstring>

namespace tlr {

namespace {

// Core Audio, declared here rather than through mmdeviceapi.h and
// endpointvolume.h: the Windows 7.1A SDK headers that the XP toolset uses
// leave parts of them out when the target is XP, and the agent decides at run
// time whether Core Audio exists. Method order follows the SDK's vtables;
// only the methods called have real signatures.
struct __declspec(uuid("BCDE0395-E52F-467C-8E3D-C4579291692E")) MMDeviceEnumeratorClass;

struct IMMDevice_ : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD clsCtx, void* params, void** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState(DWORD*) = 0;
};

struct __declspec(uuid("A95664D2-9614-4F35-A746-DE8DB63617E6")) IMMDeviceEnumerator_ : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(int, DWORD, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(int dataFlow, int role, IMMDevice_** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice_**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(void*) = 0;
};

struct __declspec(uuid("5CDF2C82-841E-4546-9722-0CF74078229A")) IAudioEndpointVolume_ : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterControlChangeNotify(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterControlChangeNotify(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMasterVolumeLevel(float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMasterVolumeLevelScalar(float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMasterVolumeLevel(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMasterVolumeLevelScalar(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumeLevel(UINT, float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolumeLevelScalar(UINT, float, LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelVolumeLevel(UINT, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelVolumeLevelScalar(UINT, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMute(BOOL mute, LPCGUID context) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMute(BOOL* mute) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeStepInfo(UINT*, UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VolumeStepUp(LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE VolumeStepDown(LPCGUID) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeRange(float*, float*, float*) = 0;
};

const int kDataFlowRender = 0;   // EDataFlow eRender
const int kRoleConsole = 0;      // ERole eConsole

std::string hresultText(HRESULT hr)
{
    return format("0x%08X", (unsigned)hr);
}

// Vista and later. unavailable is set when the Core Audio classes are not
// registered at all, which is the case on XP, so that the mixer API is tried.
bool coreAudioMute(bool on, bool& mutedNow, std::string& error, bool& unavailable)
{
    unavailable = false;
    IMMDeviceEnumerator_* en = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumeratorClass), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator_), (void**)&en);
    if (FAILED(hr)) {
        unavailable = hr == REGDB_E_CLASSNOTREG || hr == CLASS_E_CLASSNOTAVAILABLE;
        error = "MMDeviceEnumerator: " + hresultText(hr);
        return false;
    }
    IMMDevice_* dev = NULL;
    hr = en->GetDefaultAudioEndpoint(kDataFlowRender, kRoleConsole, &dev);
    en->Release();
    if (FAILED(hr)) { error = "no default playback device: " + hresultText(hr); return false; }
    IAudioEndpointVolume_* vol = NULL;
    hr = dev->Activate(__uuidof(IAudioEndpointVolume_), CLSCTX_ALL, NULL, (void**)&vol);
    dev->Release();
    if (FAILED(hr)) { error = "IAudioEndpointVolume: " + hresultText(hr); return false; }
    hr = vol->SetMute(on ? TRUE : FALSE, NULL);
    BOOL muted = FALSE;
    HRESULT hrGet = vol->GetMute(&muted);
    vol->Release();
    if (FAILED(hr)) { error = "SetMute: " + hresultText(hr); return false; }
    if (FAILED(hrGet)) { error = "GetMute: " + hresultText(hrGet); return false; }
    mutedNow = muted != FALSE;
    return true;
}

// XP: the mute control of the speaker destination line of the first mixer.
bool mixerMute(bool on, bool& mutedNow, std::string& error)
{
    HMIXER mixer = NULL;
    MMRESULT r = mixerOpen(&mixer, 0, 0, 0, MIXER_OBJECTF_MIXER);
    if (r != MMSYSERR_NOERROR) { error = format("mixerOpen: error %u (no mixer device?)", (unsigned)r); return false; }

    MIXERLINE line;
    memset(&line, 0, sizeof(line));
    line.cbStruct = sizeof(line);
    line.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    r = mixerGetLineInfo((HMIXEROBJ)mixer, &line, MIXER_OBJECTF_HMIXER | MIXER_GETLINEINFOF_COMPONENTTYPE);
    if (r != MMSYSERR_NOERROR) { mixerClose(mixer); error = format("mixerGetLineInfo (speakers): error %u", (unsigned)r); return false; }

    MIXERCONTROL ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    MIXERLINECONTROLS lc;
    memset(&lc, 0, sizeof(lc));
    lc.cbStruct = sizeof(lc);
    lc.dwLineID = line.dwLineID;
    lc.dwControlType = MIXERCONTROL_CONTROLTYPE_MUTE;
    lc.cControls = 1;
    lc.cbmxctrl = sizeof(ctrl);
    lc.pamxctrl = &ctrl;
    r = mixerGetLineControls((HMIXEROBJ)mixer, &lc, MIXER_OBJECTF_HMIXER | MIXER_GETLINECONTROLSF_ONEBYTYPE);
    if (r != MMSYSERR_NOERROR) { mixerClose(mixer); error = format("the speaker line has no mute control: error %u", (unsigned)r); return false; }

    MIXERCONTROLDETAILS_BOOLEAN value;
    value.fValue = on ? 1 : 0;
    MIXERCONTROLDETAILS d;
    memset(&d, 0, sizeof(d));
    d.cbStruct = sizeof(d);
    d.dwControlID = ctrl.dwControlID;
    d.cChannels = 1;                 // a mute control is uniform across channels
    d.cMultipleItems = 0;
    d.cbDetails = sizeof(value);
    d.paDetails = &value;
    r = mixerSetControlDetails((HMIXEROBJ)mixer, &d, MIXER_OBJECTF_HMIXER | MIXER_SETCONTROLDETAILSF_VALUE);
    if (r != MMSYSERR_NOERROR) { mixerClose(mixer); error = format("mixerSetControlDetails: error %u", (unsigned)r); return false; }
    value.fValue = on ? 0 : 1;       // so that a failed read does not look like success
    r = mixerGetControlDetails((HMIXEROBJ)mixer, &d, MIXER_OBJECTF_HMIXER | MIXER_GETCONTROLDETAILSF_VALUE);
    mixerClose(mixer);
    if (r != MMSYSERR_NOERROR) { error = format("mixerGetControlDetails: error %u", (unsigned)r); return false; }
    mutedNow = value.fValue != 0;
    return true;
}

} // namespace

bool setAudioMute(bool on, bool& mutedNow, std::string& method, std::string& error)
{
    error.clear();
    method.clear();
    mutedNow = false;
    bool unavailable = false;
    bool ok = coreAudioMute(on, mutedNow, error, unavailable);
    method = "Core Audio";
    if (!ok && unavailable) {
        method = "mixer API";
        ok = mixerMute(on, mutedNow, error);
    }
    if (ok && mutedNow != on) {
        error = format("the device reads back %s", mutedNow ? "muted" : "unmuted");
        return false;
    }
    return ok;
}

} // namespace tlr
