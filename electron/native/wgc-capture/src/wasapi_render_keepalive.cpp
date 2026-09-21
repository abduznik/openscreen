#include "wasapi_render_keepalive.h"

#include <chrono>
#include <iostream>

namespace {

constexpr REFERENCE_TIME BufferDurationHns = 10'000'000;

} // namespace

WasapiRenderKeepAlive::~WasapiRenderKeepAlive() {
    stop();
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
}

bool WasapiRenderKeepAlive::start() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> deviceEnumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        // No default render device to keep alive -- nothing to do.
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient_);
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) {
        return false;
    }

    // Shared mode: mixes into whatever else may be playing rather than requesting
    // exclusive control, so it can't block another app from using the device, and
    // it fails cleanly (non-fatal to the caller) if another app already holds it
    // exclusively.
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, BufferDurationHns, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->GetBufferSize(&bufferFrameCount_);
    if (FAILED(hr)) {
        return false;
    }

    hr = audioClient_->GetService(IID_PPV_ARGS(&renderClient_));
    if (FAILED(hr)) {
        return false;
    }

    // Prime the full buffer with silence before Start() so there's no gap for the
    // audio engine to glitch on.
    BYTE* data = nullptr;
    hr = renderClient_->GetBuffer(bufferFrameCount_, &data);
    if (FAILED(hr)) {
        return false;
    }
    renderClient_->ReleaseBuffer(bufferFrameCount_, AUDCLNT_BUFFERFLAGS_SILENT);

    stopRequested_ = false;
    hr = audioClient_->Start();
    if (FAILED(hr)) {
        return false;
    }

    thread_ = std::thread([this] {
        // GetBuffer/ReleaseBuffer/GetCurrentPadding are called from this thread,
        // which is otherwise never COM-initialized. Both this and the wmain thread
        // are MTA (see winrt::init_apartment in main.cpp), so no marshaling is
        // needed -- this only satisfies the "calling thread must be initialized"
        // requirement.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        renderLoop();
        if (SUCCEEDED(comInit)) {
            CoUninitialize();
        }
    });
    return true;
}

void WasapiRenderKeepAlive::stop() {
    stopRequested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (audioClient_) {
        audioClient_->Stop();
    }
    renderClient_.Reset();
    audioClient_.Reset();
}

void WasapiRenderKeepAlive::renderLoop() {
    while (!stopRequested_) {
        UINT32 paddingFrames = 0;
        if (FAILED(audioClient_->GetCurrentPadding(&paddingFrames))) {
            break;
        }

        const UINT32 framesAvailable = bufferFrameCount_ - paddingFrames;
        if (framesAvailable > 0) {
            BYTE* data = nullptr;
            if (SUCCEEDED(renderClient_->GetBuffer(framesAvailable, &data))) {
                renderClient_->ReleaseBuffer(framesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
