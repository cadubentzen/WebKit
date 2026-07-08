/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 * Copyright (C) 2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AudioOutputUnitAdaptor.h"

#if ENABLE(WEB_AUDIO) && PLATFORM(MAC)

#include "AudioBus.h"
#include "Logging.h"
#include <CoreAudio/AudioHardware.h>

#if ENABLE(MEDIA_STREAM)
#include "AudioMediaStreamTrackRenderer.h"
#include "CoreAudioCaptureDevice.h"
#include "CoreAudioCaptureDeviceManager.h"
#include "RealtimeMediaSourceCenter.h"
#endif

#include <pal/cf/AudioToolboxSoftLink.h>

namespace WebCore {

// Resolves a persistent audio output device id to a CoreAudio device id suitable for
// kAudioOutputUnitProperty_CurrentDevice (0 means the default output device). Returns std::nullopt
// if no audio output device matches the given id.
static std::optional<uint32_t> audioOutputDeviceIDForPersistentId(const String& persistentDeviceId)
{
    if (persistentDeviceId.isEmpty())
        return 0;
#if ENABLE(MEDIA_STREAM)
    if (persistentDeviceId == AudioMediaStreamTrackRenderer::defaultDeviceID())
        return 0;
    if (auto device = CoreAudioCaptureDeviceManager::singleton().coreAudioDeviceWithUID(persistentDeviceId); device && device->type() == CaptureDevice::DeviceType::Speaker)
        return device->deviceID();
    // Speaker devices vended by another factory (e.g. mock capture devices) have no CoreAudio
    // device; keep rendering to the default output device.
    auto speakerDevices = RealtimeMediaSourceCenter::singleton().audioCaptureFactory().speakerDevices();
    if (speakerDevices.containsIf([&](auto& device) { return device.persistentId() == persistentDeviceId; }))
        return 0;
#endif
    return std::nullopt;
}

void AudioOutputUnitAdaptor::configure(float hardwareSampleRate, unsigned numberOfOutputChannels)
{
    m_hardwareSampleRate = hardwareSampleRate;
    m_numberOfOutputChannels = numberOfOutputChannels;
    createOutputUnit();
}

bool AudioOutputUnitAdaptor::createOutputUnit()
{
    ASSERT(!m_outputUnit);

    // Open and initialize the output unit. The default output unit follows the default output
    // device; a specific device needs an AUHAL unit with the device set explicitly.
    AudioComponent comp;
    AudioComponentDescription desc;

    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = m_outputDeviceID ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;
    comp = PAL::AudioComponentFindNext(0, &desc);

    ASSERT(comp);
    if (!comp)
        return false;

    OSStatus result = PAL::AudioComponentInstanceNew(comp, &m_outputUnit);
    ASSERT(!result);
    if (result)
        return false;

    auto destroyOutputUnit = [&] {
        PAL::AudioComponentInstanceDispose(m_outputUnit);
        m_outputUnit = 0;
    };

    if (m_outputDeviceID) {
        result = PAL::AudioUnitSetProperty(m_outputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &m_outputDeviceID, sizeof(m_outputDeviceID));
        if (result) {
            RELEASE_LOG_ERROR(Media, "AudioOutputUnitAdaptor::createOutputUnit: failed to set output device %u, error code: %d", static_cast<unsigned>(m_outputDeviceID), static_cast<int>(result));
            destroyOutputUnit();
            return false;
        }
    }

    result = PAL::AudioUnitInitialize(m_outputUnit);
    ASSERT(!result);
    if (result) {
        destroyOutputUnit();
        return false;
    }

    // Set render callback
    AURenderCallbackStruct input;
    input.inputProc = inputProc;
    input.inputProcRefCon = this;
    result = PAL::AudioUnitSetProperty(m_outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, 0, &input, sizeof(input));
    ASSERT(!result);
    if (result) {
        destroyOutputUnit();
        return false;
    }

    // Set stream format
    constexpr int bytesPerFloat = sizeof(Float32);
    constexpr int bitsPerByte = 8;

    AudioStreamBasicDescription streamFormat;
    streamFormat.mSampleRate = m_hardwareSampleRate;
    streamFormat.mFormatID = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) | static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
    streamFormat.mBytesPerPacket = bytesPerFloat;
    streamFormat.mFramesPerPacket = 1;
    streamFormat.mBytesPerFrame = bytesPerFloat;
    streamFormat.mChannelsPerFrame = m_numberOfOutputChannels;
    streamFormat.mBitsPerChannel = bitsPerByte * bytesPerFloat;

    result = PAL::AudioUnitSetProperty(m_outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, (void*)&streamFormat, sizeof(AudioStreamBasicDescription));
    ASSERT(!result);
    if (result) {
        destroyOutputUnit();
        return false;
    }

    return true;
}

bool AudioOutputUnitAdaptor::applyOutputDevice(const String& persistentDeviceId)
{
    auto deviceID = audioOutputDeviceIDForPersistentId(persistentDeviceId);
    if (!deviceID)
        return false;

    if (*deviceID == m_outputDeviceID) {
        m_outputDevicePersistentId = persistentDeviceId;
        return true;
    }

    // Recreate the output unit targeting the new device: the default output unit cannot be
    // retargeted in place.
    auto previousDeviceID = std::exchange(m_outputDeviceID, *deviceID);
    if (m_outputUnit) {
        PAL::AudioComponentInstanceDispose(m_outputUnit);
        m_outputUnit = 0;
    }

    if (!createOutputUnit()) {
        // Restore the previous routing on failure.
        m_outputDeviceID = previousDeviceID;
        createOutputUnit();
        return false;
    }

    m_outputDevicePersistentId = persistentDeviceId;
    return true;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && PLATFORM(MAC)
