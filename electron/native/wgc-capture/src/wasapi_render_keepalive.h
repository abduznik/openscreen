#pragma once

// getopenscreen/openscreen#724: a headset can be idled by Windows partway through a
// mic-only recording, because nothing in that path ever writes to the render
// (playback) endpoint -- WasapiLoopbackCapture's system-audio path only ever READS
// from it via AUDCLNT_STREAMFLAGS_LOOPBACK. Confirmed on real hardware: the drop
// reproduces with mic-only capture and does NOT reproduce with system audio (i.e.
// loopback) also enabled, so touching the render endpoint at all is what keeps it
// alive, and loopback capture already does that as a side effect when it runs.
//
// This opens an ordinary render stream on the same default output device and
// writes silence to it for as long as a recording is running, independent of
// whether system audio is being captured, since mic-only is exactly the case that
// leaves the endpoint untouched otherwise.
//
// Kill-switch: set OPENSCREEN_WGC_DISABLE_AUDIO_KEEPALIVE=1 to turn this off.

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

class WasapiRenderKeepAlive {
public:
    WasapiRenderKeepAlive() = default;
    ~WasapiRenderKeepAlive();

    WasapiRenderKeepAlive(const WasapiRenderKeepAlive&) = delete;
    WasapiRenderKeepAlive& operator=(const WasapiRenderKeepAlive&) = delete;

    // Opens the default render endpoint in shared mode and starts writing silence.
    // Returns false on any failure -- including another app holding the device
    // exclusively -- which callers must treat as non-fatal to the recording itself.
    bool start();
    void stop();

private:
    void renderLoop();

    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient_;
    WAVEFORMATEX* mixFormat_ = nullptr;
    UINT32 bufferFrameCount_ = 0;
    std::thread thread_;
    std::atomic<bool> stopRequested_ = false;
};
