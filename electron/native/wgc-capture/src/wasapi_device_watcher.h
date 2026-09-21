#pragma once

// DIAGNOSTIC ONLY, opt-in via OPENSCREEN_WGC_LOG_AUDIO_DEVICE_EVENTS=1 (see main.cpp).
//
// getopenscreen/openscreen#724: before committing to a fix, we need a real,
// timestamped signal on WHY a headset's render/capture endpoints change state
// mid-recording -- USB selective suspend, WASAPI endpoint idle, and the headset's
// own firmware auto-off all look the same to the user ("headphones turned off"),
// but only some of them are fixable from inside this process. This watcher answers
// that by registering an IMMNotificationClient for the duration of the recording
// and emitting a structured JSON event on every state transition of the default
// render and capture endpoints, so it can be correlated against the moment a user
// hears their headset drop.
//
// This does not attempt to fix anything -- it only observes and reports. See the
// issue for the three-way split this is meant to distinguish:
//   1. USB selective suspend (device level): the endpoint would go NOTPRESENT/
//      UNPLUGGED, i.e. the whole device disappears, not just the audio state.
//   2. WASAPI endpoint idle (audio-engine level): the endpoint would typically stay
//      ACTIVE while going quiet -- unlikely to show anything here at all, since nothing
//      in the device's own docs treats "unwritten render buffer" as a state change.
//   3. Headset firmware auto-off: same observable shape as (1) from Windows' point of
//      view (the endpoint disappears), but no amount of keeping the render endpoint
//      "busy" in software prevents a keyed hardware timer from firing.
//
// A DEVICE_STATE_NOTPRESENT/UNPLUGGED transition on the render or capture endpoint,
// correlated with the moment the user hears the drop, points at (1) or (3). No event
// at all around the drop, with the endpoint remaining ACTIVE throughout, would point
// at (2) instead.

#include <Windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>

class WasapiDeviceWatcher : public IMMNotificationClient {
public:
    WasapiDeviceWatcher() = default;
    ~WasapiDeviceWatcher();

    WasapiDeviceWatcher(const WasapiDeviceWatcher&) = delete;
    WasapiDeviceWatcher& operator=(const WasapiDeviceWatcher&) = delete;

    // Registers for notifications and emits one baseline event per endpoint
    // (render + capture) with their state at the moment the recording starts, so a
    // report has a starting point even if nothing changes afterward.
    bool start();
    void stop();

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override;

private:
    void emitBaseline(EDataFlow flow, const wchar_t* flowLabel);
    void emitDeviceEvent(const wchar_t* eventName, LPCWSTR deviceId, const char* extraJson = nullptr);

    std::atomic<ULONG> refCount_ = 1;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> deviceEnumerator_;
    bool registered_ = false;
    // Guards std::cout: notifications can arrive on a COM callback thread
    // concurrently with the main thread's own JSON event writes.
    std::mutex outputMutex_;
};
