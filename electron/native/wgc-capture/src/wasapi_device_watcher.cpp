#include "wasapi_device_watcher.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <chrono>
#include <cstdio>
#include <iostream>

namespace {

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            default:
                result += c;
        }
    }
    return result;
}

std::string deviceStateLabel(DWORD state) {
    switch (state) {
        case DEVICE_STATE_ACTIVE:
            return "active";
        case DEVICE_STATE_DISABLED:
            return "disabled";
        case DEVICE_STATE_NOTPRESENT:
            return "not-present";
        case DEVICE_STATE_UNPLUGGED:
            return "unplugged";
        default:
            return "unknown";
    }
}

int64_t nowUnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring friendlyNameForDevice(IMMDeviceEnumerator* enumerator, LPCWSTR deviceId) {
    if (!enumerator || !deviceId) {
        return {};
    }
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(deviceId, &device)) || !device) {
        return {};
    }
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

} // namespace

WasapiDeviceWatcher::~WasapiDeviceWatcher() {
    stop();
}

bool WasapiDeviceWatcher::start() {
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&deviceEnumerator_));
    if (FAILED(hr) || !deviceEnumerator_) {
        std::cerr << "WARNING: [device-watcher] CoCreateInstance(MMDeviceEnumerator) failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        return false;
    }

    hr = deviceEnumerator_->RegisterEndpointNotificationCallback(this);
    if (FAILED(hr)) {
        std::cerr << "WARNING: [device-watcher] RegisterEndpointNotificationCallback failed (hr=0x"
                  << std::hex << hr << std::dec << ")" << std::endl;
        deviceEnumerator_.Reset();
        return false;
    }
    registered_ = true;

    emitBaseline(eRender, L"render");
    emitBaseline(eCapture, L"capture");
    return true;
}

void WasapiDeviceWatcher::stop() {
    if (registered_ && deviceEnumerator_) {
        deviceEnumerator_->UnregisterEndpointNotificationCallback(this);
    }
    registered_ = false;
    deviceEnumerator_.Reset();
}

void WasapiDeviceWatcher::emitBaseline(EDataFlow flow, const wchar_t* flowLabel) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    HRESULT hr = deviceEnumerator_->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (FAILED(hr) || !device) {
        return;
    }

    LPWSTR rawId = nullptr;
    std::wstring id;
    if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
        id = rawId;
        CoTaskMemFree(rawId);
    }

    DWORD state = 0;
    device->GetState(&state);
    const std::wstring name = friendlyNameForDevice(deviceEnumerator_.Get(), id.c_str());

    std::lock_guard<std::mutex> lock(outputMutex_);
    std::cout << "{\"event\":\"audio-device-watch\",\"schemaVersion\":1,\"type\":\"baseline\","
                 "\"flow\":\""
              << wideToUtf8(flowLabel) << "\",\"deviceId\":\"" << jsonEscape(wideToUtf8(id))
              << "\",\"deviceName\":\"" << jsonEscape(wideToUtf8(name)) << "\",\"state\":\""
              << deviceStateLabel(state) << "\",\"timestampMs\":" << nowUnixMillis() << "}"
              << std::endl;
}

void WasapiDeviceWatcher::emitDeviceEvent(const wchar_t* eventName, LPCWSTR deviceId, const char* extraJson) {
    const std::wstring name = friendlyNameForDevice(deviceEnumerator_.Get(), deviceId);

    std::lock_guard<std::mutex> lock(outputMutex_);
    std::cout << "{\"event\":\"audio-device-watch\",\"schemaVersion\":1,\"type\":\""
              << wideToUtf8(eventName) << "\",\"deviceId\":\""
              << jsonEscape(wideToUtf8(deviceId ? deviceId : L"")) << "\",\"deviceName\":\""
              << jsonEscape(wideToUtf8(name)) << "\"";
    if (extraJson) {
        std::cout << "," << extraJson;
    }
    std::cout << ",\"timestampMs\":" << nowUnixMillis() << "}" << std::endl;
}

ULONG STDMETHODCALLTYPE WasapiDeviceWatcher::AddRef() {
    return ++refCount_;
}

ULONG STDMETHODCALLTYPE WasapiDeviceWatcher::Release() {
    // This object's lifetime is owned by main.cpp, not by COM: it lives on the
    // stack for the duration of the recording and unregisters in stop() before
    // destruction, so a ref count reaching zero here must not delete `this`.
    const ULONG count = --refCount_;
    return count;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceWatcher::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppvObject = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceWatcher::OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) {
    char extra[64];
    snprintf(extra, sizeof(extra), "\"state\":\"%s\"", deviceStateLabel(newState).c_str());
    emitDeviceEvent(L"state-changed", deviceId, extra);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceWatcher::OnDeviceAdded(LPCWSTR deviceId) {
    emitDeviceEvent(L"added", deviceId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceWatcher::OnDeviceRemoved(LPCWSTR deviceId) {
    emitDeviceEvent(L"removed", deviceId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceWatcher::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR defaultDeviceId) {
    // Only eConsole is what this app's capture paths use (GetDefaultAudioEndpoint
    // calls elsewhere all pass eConsole); the other roles fire independently and
    // would just be noise here.
    if (role != eConsole) {
        return S_OK;
    }
    char extra[16];
    snprintf(extra, sizeof(extra), "\"flow\":\"%s\"", flow == eRender ? "render" : "capture");
    emitDeviceEvent(L"default-changed", defaultDeviceId, extra);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WasapiDeviceWatcher::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
    // Not interesting for this diagnosis: fires on things like a renamed endpoint,
    // not on power/connection state.
    return S_OK;
}
