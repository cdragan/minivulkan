// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "../core/d_printf.h"
#include "../synth/realtime_synth.h"
#include "realtime_synth_os.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>

#include <atomic>

// WASAPI shared-mode, event-driven output on the default render endpoint.
// A dedicated render worker thread owns COM and services the endpoint buffer.
// A separate producer thread runs the GPU synthesis and fills the audio ring,
// so synthesis never runs on the WASAPI service thread.

static constexpr REFERENCE_TIME render_buffer_duration = 300'000; // 30 ms, in 100 ns units
static constexpr DWORD          handshake_timeout_ms   = 5'000;
static constexpr uint32_t       max_prefill_wait_ms    = 2'000; // Bounded startup wait for the ring to fill
static constexpr DWORD          producer_wait_ms       = 10;    // Fallback wake for the producer

// Fixed stream format.  The shared engine converts to the endpoint mix format
// (AUTOCONVERTPCM), so no format negotiation is needed.
static const WAVEFORMATEX stream_format = {
    WAVE_FORMAT_IEEE_FLOAT,      // 32-bit IEEE float samples
    2,                           // Stereo
    Synth::rt_sampling_rate,     // Fixed 44100 Hz
    Synth::rt_sampling_rate * 8, // Byte rate, 2 channels of 4 bytes
    8,                           // One frame is two 4-byte samples
    32,                          // Sample size in bits
    0                            // No format extension
};

static std::atomic<uint64_t> timestamp_ms{ 0 };

static HANDLE render_thread_handle;
static HANDLE producer_thread_handle;
static HANDLE audio_ready_event;   // WASAPI signals this when a packet frees up
static HANDLE stop_event;          // Stops the render worker
static HANDLE producer_wake_event; // Render worker nudges the producer after each pass
static HANDLE handshake_event;     // Render worker reports the startup result

static std::atomic<bool>     producer_running{ false };
static std::atomic<uint32_t> render_worker_result{ 0 }; // 0 = pending, 1 = ready, 2 = failed

static DWORD WINAPI producer_thread_main(LPVOID)
{
    while (producer_running.load(std::memory_order_relaxed)) {
        if (Synth::produce_audio_batch<float, true>())
            continue;
        // The ring lead is full.  Wait for a nudge from the render worker, or
        // fall back to a bounded timeout so production never stalls for long.
        WaitForSingleObject(producer_wake_event, producer_wait_ms);
    }
    return 0;
}

// Serves one WASAPI packet: copies the next ring frames into the endpoint
// buffer and releases the full requested packet, zero-filled on underrun.
static bool render_packet(IAudioRenderClient* render_client, UINT32 frames);

// Fills the endpoint buffer before the stream starts, so playback begins with
// sound instead of a silent gap.  The ring never holds more than
// max_fill_frames, so waiting for more than that cannot help.
static bool prefill_endpoint_buffer(IAudioClient*       audio_client,
                                    IAudioRenderClient* render_client,
                                    UINT32*             buffer_frames)
{
    if (FAILED(audio_client->GetBufferSize(buffer_frames)) || ! *buffer_frames)
        return false;
    const UINT32 size = *buffer_frames;

    // Give the producer a bounded chance to fill the ring first.
    const Synth::AudioRingStatus ring   = Synth::get_audio_ring_status();
    const UINT32                 target = (size < ring.max_fill_frames) ? size : ring.max_fill_frames;
    for (uint32_t waited_ms = 0; waited_ms < max_prefill_wait_ms; waited_ms += 2) {
        if (Synth::get_audio_ring_status().fill_frames >= target)
            break;
        Sleep(2);
    }

    return render_packet(render_client, size);
}

// Renders one packet of the requested frame count from the ring into the
// endpoint buffer.  consume_audio zero-fills the tail when the ring runs
// short, and the full requested packet is released either way.
static bool render_packet(IAudioRenderClient* render_client, UINT32 frames)
{
    BYTE* data = nullptr;
    if (FAILED(render_client->GetBuffer(frames, &data)))
        return false;
    Synth::consume_audio<float, true>(frames, reinterpret_cast<float*>(data), nullptr);
    return SUCCEEDED(render_client->ReleaseBuffer(frames, 0));
}

static void service_passes(IAudioClient*       audio_client,
                           IAudioRenderClient* render_client,
                           IAudioClock*        audio_clock,
                           UINT32              buffer_frames)
{
    HANDLE wait_handles[2] = { stop_event, audio_ready_event };
    bool   stream_lost     = false;

    for (;;) {
        const DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        if (wait_result != WAIT_OBJECT_0 + 1)
            break; // Stop requested, or the wait itself failed

        UINT32 padding = 0;
        if (FAILED(audio_client->GetCurrentPadding(&padding))) {
            stream_lost = true;
            break;
        }
        const UINT32 available = buffer_frames - padding;
        if (available) {
            if (! render_packet(render_client, available)) {
                stream_lost = true;
                break;
            }
            // Let the producer top the ring up promptly.
            SetEvent(producer_wake_event);
        }

        // Snapshot the stream-relative playback position for the GUI.
        UINT64 position  = 0;
        UINT64 frequency = 0;
        if (SUCCEEDED(audio_clock->GetPosition(&position, nullptr)) &&
            SUCCEEDED(audio_clock->GetFrequency(&frequency)) && frequency) {
            timestamp_ms.store(position * 1000 / frequency, std::memory_order_relaxed);
        }
    }

    if (stream_lost)
        d_printf("WASAPI audio stream was invalidated\n");
}

static DWORD WINAPI render_thread_main(LPVOID)
{
    IMMDeviceEnumerator* enumerator      = nullptr;
    IMMDevice*           device          = nullptr;
    IAudioClient*        audio_client    = nullptr;
    IAudioRenderClient*  render_client   = nullptr;
    IAudioClock*         audio_clock     = nullptr;
    UINT32               buffer_frames   = 0;
    bool                 com_initialized = false;
    bool                 started         = false;

    // Register with MMCSS for a scheduling boost.  MMCSS picks the priority;
    // we never raise the thread to TIME_CRITICAL ourselves.
    DWORD  mmcss_index  = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
    if (! mmcss_handle)
        d_printf("Failed to register the audio thread with MMCSS\n");

    bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (! ok)
        d_printf("Failed to initialize COM for audio\n");
    else
        com_initialized = true;

    if (ok && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                      nullptr,
                                      CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&enumerator)))) {
        d_printf("Failed to create the WASAPI device enumerator\n");
        ok = false;
    }

    if (ok && FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        d_printf("Failed to find the default audio output device\n");
        ok = false;
    }

    if (ok &&
        FAILED(
            device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client)))) {
        d_printf("Failed to activate the WASAPI audio client\n");
        ok = false;
    }

    if (ok && FAILED(audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                              render_buffer_duration,
                                              0,
                                              &stream_format,
                                              nullptr))) {
        d_printf("Failed to initialize WASAPI shared-mode streaming\n");
        ok = false;
    }

    if (ok && FAILED(audio_client->SetEventHandle(audio_ready_event))) {
        d_printf("Failed to register the WASAPI buffer event\n");
        ok = false;
    }

    if (ok &&
        FAILED(audio_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client)))) {
        d_printf("Failed to get the WASAPI render client\n");
        ok = false;
    }

    if (ok && FAILED(audio_client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&audio_clock)))) {
        d_printf("Failed to get the WASAPI audio clock\n");
        ok = false;
    }

    if (ok && ! prefill_endpoint_buffer(audio_client, render_client, &buffer_frames)) {
        d_printf("Failed to prefill the WASAPI endpoint buffer\n");
        ok = false;
    }

    if (ok && FAILED(audio_client->Start())) {
        d_printf("Failed to start WASAPI audio streaming\n");
        ok = false;
    }

    if (ok) {
        started = true;
        render_worker_result.store(1, std::memory_order_release);
        SetEvent(handshake_event);
        service_passes(audio_client, render_client, audio_clock, buffer_frames);
    }
    else {
        render_worker_result.store(2, std::memory_order_release);
        SetEvent(handshake_event);
    }

    if (started)
        audio_client->Stop();

    if (audio_clock)
        audio_clock->Release();
    if (render_client)
        render_client->Release();
    if (audio_client)
        audio_client->Release();
    if (device)
        device->Release();
    if (enumerator)
        enumerator->Release();

    if (mmcss_handle)
        AvRevertMmThreadCharacteristics(mmcss_handle);

    if (com_initialized)
        CoUninitialize();

    return 0;
}

// Stops both workers and closes all worker handles.  Safe at any point, even
// after a partial initialization failure.
static void stop_workers()
{
    // Stop the render worker first, then the producer.  The render worker is
    // the only ring consumer, so it stops consuming before the producer stops
    // filling.  The waits are unbounded, so both threads have exited before
    // any handle is closed.
    if (render_thread_handle) {
        SetEvent(stop_event);
        WaitForSingleObject(render_thread_handle, INFINITE);
        CloseHandle(render_thread_handle);
        render_thread_handle = nullptr;
    }

    if (producer_running.exchange(false, std::memory_order_relaxed))
        SetEvent(producer_wake_event);

    if (producer_thread_handle) {
        WaitForSingleObject(producer_thread_handle, INFINITE);
        CloseHandle(producer_thread_handle);
        producer_thread_handle = nullptr;
    }

    // Close the events last, after both threads have exited.
    if (handshake_event) {
        CloseHandle(handshake_event);
        handshake_event = nullptr;
    }
    if (audio_ready_event) {
        CloseHandle(audio_ready_event);
        audio_ready_event = nullptr;
    }
    if (producer_wake_event) {
        CloseHandle(producer_wake_event);
        producer_wake_event = nullptr;
    }
    if (stop_event) {
        CloseHandle(stop_event);
        stop_event = nullptr;
    }
}

bool Synth::init_synth_os()
{
    // Live MIDI input is nice to have; audio still initializes without it.
    if (! init_midi_os())
        d_printf("Live MIDI input unavailable\n");

    audio_ready_event   = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    stop_event          = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    producer_wake_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    handshake_event     = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    bool ok = audio_ready_event && stop_event && producer_wake_event && handshake_event;
    if (! ok)
        d_printf("Failed to create the audio worker events\n");

    // Start the producer first, so the ring fills while the render worker
    // prepares and prefills the endpoint buffer.
    if (ok) {
        producer_running.store(true, std::memory_order_relaxed);
        producer_thread_handle = CreateThread(nullptr, 0, producer_thread_main, nullptr, 0, nullptr);
        if (! producer_thread_handle) {
            d_printf("Failed to start the audio producer thread\n");
            producer_running.store(false, std::memory_order_relaxed);
            ok = false;
        }
    }

    if (ok) {
        render_thread_handle = CreateThread(nullptr, 0, render_thread_main, nullptr, 0, nullptr);
        if (! render_thread_handle) {
            d_printf("Failed to start the WASAPI render thread\n");
            ok = false;
        }
    }

    if (ok) {
        // Bounded handshake: return only after the render worker prefilled the
        // endpoint buffer and started the stream, or after it failed.
        const DWORD wait_result = WaitForSingleObject(handshake_event, handshake_timeout_ms);
        ok = wait_result == WAIT_OBJECT_0 && render_worker_result.load(std::memory_order_acquire) == 1;
        if (! ok)
            d_printf("Audio output initialization failed\n");
    }

    if (! ok) {
        stop_midi_os();
        stop_workers();
    }

    return ok;
}

void Synth::stop_synth_os()
{
    stop_midi_os();
    stop_workers();
}

uint64_t Synth::get_current_timestamp_ms()
{
    return timestamp_ms.load(std::memory_order_relaxed);
}
