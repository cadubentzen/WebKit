/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WEB_AUDIO)

#include <AudioUnit/AudioUnit.h>
#include <atomic>
#include <wtf/text/WTFString.h>

OBJC_CLASS CASpatialAudioExperience;

namespace WebCore {

class AudioUnitRenderer {
public:
    virtual ~AudioUnitRenderer() = default;
    virtual OSStatus render(double sampleTime, uint64_t hostTime, UInt32 numberOfFrames, AudioBufferList* ioData) = 0;
};

class AudioOutputUnitAdaptor {
public:
    WEBCORE_EXPORT AudioOutputUnitAdaptor(AudioUnitRenderer&);
    WEBCORE_EXPORT ~AudioOutputUnitAdaptor();

    WEBCORE_EXPORT void configure(float hardwareSampleRate, unsigned numberOfOutputChannels);
    WEBCORE_EXPORT OSStatus start();
    WEBCORE_EXPORT OSStatus stop();

    WEBCORE_EXPORT size_t outputLatency() const;

    // Routes the output to the audio device with the given persistent id (empty means the default
    // output device), restarting the unit if it is running. Returns false if the device could not
    // be acquired, in which case the previous routing stays in effect.
    WEBCORE_EXPORT bool setOutputDevice(const String& persistentDeviceId);
    const String& outputDevicePersistentId() const LIFETIME_BOUND { return m_outputDevicePersistentId; }

    // A silent sink keeps rendering, so currentTime keeps progressing, but drops the output.
    void setIsSilent(bool isSilent) { m_isSilentSink.store(isSilent, std::memory_order_relaxed); }

#if HAVE(SPATIAL_AUDIO_EXPERIENCE)
    WEBCORE_EXPORT void setSpatialAudioExperience(CASpatialAudioExperience *);
#endif

private:
    static OSStatus inputProc(void* userData, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32 busNumber, UInt32 numberOfFrames, AudioBufferList* ioData);

    bool applyOutputDevice(const String& persistentDeviceId);
#if PLATFORM(MAC)
    bool createOutputUnit();
#endif

    AudioUnit m_outputUnit;
    AudioUnitRenderer& m_audioUnitRenderer;

    String m_outputDevicePersistentId;
    bool m_isRunning { false };
    std::atomic<bool> m_isSilentSink { false };
#if PLATFORM(MAC)
    float m_hardwareSampleRate { 0 };
    unsigned m_numberOfOutputChannels { 0 };
    // CoreAudio device id matching m_outputDevicePersistentId (0 means the default output device).
    uint32_t m_outputDeviceID { 0 };
#endif
};

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
