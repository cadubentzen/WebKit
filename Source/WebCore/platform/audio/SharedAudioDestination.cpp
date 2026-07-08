/*
 * Copyright (C) 2024-2025 Apple Inc. All rights reserved.
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

#include "config.h"
#include "SharedAudioDestination.h"

#if ENABLE(WEB_AUDIO)

#include "AudioUtilities.h"
#include <wtf/MediaTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/WTFSemaphore.h>
#include <wtf/WeakPtr.h>
#include <wtf/WorkQueue.h>
#include <wtf/threads/BinarySemaphore.h>

namespace WebCore {

class SharedAudioDestinationAdapter : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<SharedAudioDestinationAdapter>, public AudioIOCallback {
public:
    using CreationOptions = AudioDestinationCreationOptions;
    using AudioDestinationCreationFunction = SharedAudioDestination::AudioDestinationCreationFunction;
    static Ref<SharedAudioDestinationAdapter> ensureAdapter(const CreationOptions&, const AudioDestinationCreationFunction& ensureFunction);
    ~SharedAudioDestinationAdapter();

    void addRenderer(SharedAudioDestination&, CompletionHandler<void(bool)>&&);
    void removeRenderer(SharedAudioDestination&, CompletionHandler<void(bool)>&&);

    unsigned framesPerBuffer() const
    {
        return m_workBus->length();
    }

    MediaTime outputLatency() const
    {
        return m_destination->outputLatency();
    }

#if PLATFORM(IOS_FAMILY)
    const String& sceneIdentifier() const { return m_sceneIdentifier; }
#endif

    void setSinkId(const String& persistentDeviceId, bool isSilent, CompletionHandler<void(bool)>&& completionHandler)
    {
        m_destination->setSinkId(persistentDeviceId, isSilent, WTF::move(completionHandler));
    }

private:
#if PLATFORM(IOS_FAMILY)
    using AdapterKey = std::tuple<unsigned, float, String, bool, String>;
#else
    using AdapterKey = std::tuple<unsigned, float, String, bool>;
#endif
    using AdapterMap = HashMap<AdapterKey, ThreadSafeWeakPtr<SharedAudioDestinationAdapter>>;
    static AdapterMap& sharedMap();

    SharedAudioDestinationAdapter(const CreationOptions&, const AudioDestinationCreationFunction&);

    AdapterKey key() const
    {
        return { m_numberOfOutputChannels, m_sampleRate, m_outputDeviceId, m_isSilent
#if PLATFORM(IOS_FAMILY)
            , m_sceneIdentifier
#endif
        };
    }

    void render(AudioBus& destinationBus, size_t framesToProcess, const AudioIOPosition& outputPosition) final;
    void isPlayingDidChange() final { }

    void configureRenderThread(CompletionHandler<void(bool)>&&);

    unsigned m_numberOfOutputChannels;
    float m_sampleRate;
    String m_outputDeviceId { emptyString() };
    bool m_isSilent { false };

#if PLATFORM(IOS_FAMILY)
    String m_sceneIdentifier { emptyString() };
#endif

    const Ref<AudioDestination> m_destination;
    const Ref<AudioBus> m_workBus;

    bool m_started { false };

    Lock m_renderLock;

    using RenderVector = Vector<RefPtr<SharedAudioDestination>>;
    RenderVector m_renderers WTF_GUARDED_BY_CAPABILITY(mainThread);

    bool m_needsConfiguration WTF_GUARDED_BY_LOCK(m_renderLock) { true };
    RenderVector m_newRenderers WTF_GUARDED_BY_LOCK(m_renderLock);

    // Only accessed on the audio thread:
    RenderVector m_configuredRenderers;
};

auto SharedAudioDestinationAdapter::sharedMap() -> AdapterMap&
{
    static MainThreadNeverDestroyed<AdapterMap> map;
    return map;
}

Ref<SharedAudioDestinationAdapter> SharedAudioDestinationAdapter::ensureAdapter(const CreationOptions& options, const AudioDestinationCreationFunction& ensureFunction)
{
    std::tuple key { options.numberOfOutputChannels, options.sampleRate, options.outputDeviceId.isNull() ? emptyString() : options.outputDeviceId, options.isSilent
#if PLATFORM(IOS_FAMILY)
        , options.sceneIdentifier.isNull() ? emptyString() : options.sceneIdentifier
#endif
    };
    auto results = sharedMap().find(key);
    if (results != sharedMap().end()) {
        if (RefPtr existingAdapter = results->value.get())
            return existingAdapter.releaseNonNull();
    }

    Ref newAdapter = adoptRef(*new SharedAudioDestinationAdapter(options, ensureFunction));
    auto weakAdapter = ThreadSafeWeakPtr<SharedAudioDestinationAdapter> { newAdapter.get() };
    sharedMap().set(key, WTF::move(weakAdapter));
    return newAdapter;
}

SharedAudioDestinationAdapter::SharedAudioDestinationAdapter(const CreationOptions& options, const AudioDestinationCreationFunction& ensureFunction)
    : m_numberOfOutputChannels { options.numberOfOutputChannels }
    , m_sampleRate { options.sampleRate }
    , m_outputDeviceId { options.outputDeviceId.isNull() ? emptyString() : options.outputDeviceId }
    , m_isSilent { options.isSilent }
    , m_destination { ensureFunction({ *this, options.inputDeviceId, options.numberOfInputChannels, options.numberOfOutputChannels, options.sampleRate
#if PLATFORM(IOS_FAMILY)
        , options.sceneIdentifier.isNull() ? emptyString() : options.sceneIdentifier
#endif
        , options.outputDeviceId, options.isSilent }) }
    , m_workBus { AudioBus::create(options.numberOfOutputChannels, AudioUtilities::renderQuantumSize) }
{
}

SharedAudioDestinationAdapter::~SharedAudioDestinationAdapter()
{
    sharedMap().remove(key());
    m_destination->clearCallback();
}

void SharedAudioDestinationAdapter::addRenderer(SharedAudioDestination& renderer, CompletionHandler<void(bool)>&& completionHandler)
{
    assertIsMainThread();
    if (!m_renderers.contains(&renderer))
        m_renderers.append(&renderer);
    configureRenderThread(WTF::move(completionHandler));
}

void SharedAudioDestinationAdapter::removeRenderer(SharedAudioDestination& renderer, CompletionHandler<void(bool)>&& completionHandler)
{
    assertIsMainThread();
    m_renderers.removeFirst(&renderer);
    ASSERT(!m_renderers.contains(&renderer));
    configureRenderThread(WTF::move(completionHandler));
}

void SharedAudioDestinationAdapter::configureRenderThread(CompletionHandler<void(bool)>&& completionHandler)
{
    assertIsMainThread();

    bool shouldStart = !m_started && !m_renderers.isEmpty();
    bool shouldStop = m_started && m_renderers.isEmpty();
    bool shouldSkipRendering = !m_started && m_renderers.isEmpty();
    bool onlyNeedsConfiguration = m_started && !m_renderers.isEmpty();

    {
        Locker locker { m_renderLock };
        m_newRenderers = m_renderers;
        m_needsConfiguration = true;
        if (onlyNeedsConfiguration) {
            // The destination is already running, but needs configuration. Assume
            // the configuration will succeed and call the completionHandler early.
            callOnMainThread([completionHandler = WTF::move(completionHandler)] () mutable {
                completionHandler(true);
            });
            return;
        }
    }

    if (shouldStart) {
        m_started = true;
        m_destination->start(nullptr, WTF::move(completionHandler));
        return;
    }

    if (shouldStop) {
        m_started = false;
        m_destination->stop(WTF::move(completionHandler));
        return;
    }

    // If the destination has not been started, and the list of
    // renderers is empty, do not wait for the render thread to
    // finish configuration, as it will never run.
    if (shouldSkipRendering) {
        callOnMainThread([completionHandler = WTF::move(completionHandler)] () mutable {
            completionHandler(true);
        });
        return;
    }
}


void SharedAudioDestinationAdapter::render(AudioBus& destinationBus, size_t numberOfFrames, const AudioIOPosition& outputPosition)
{
    if (m_renderLock.tryLock()) {
        Locker locker { AdoptLock, m_renderLock };
        if (m_needsConfiguration) {
            // The SharedAudioDestinationAdapter avoids allocing or deallocing on the
            // high priority audio thread by merely swapping the contents of the renderer
            // configuration vectors. After the swap, the previous contents of m_configuredRenderers
            // will be destroyed on the main thread.
            RenderVector oldRenderers = std::exchange(m_configuredRenderers, WTF::move(m_newRenderers));
            m_needsConfiguration = false;
            callOnMainThread([oldRenderers = WTF::move(oldRenderers)] () { });
        }
    }

    bool isFirstRenderer = true;
    for (RefPtr renderer : m_configuredRenderers) {
        if (isFirstRenderer) {
            // The first renderer should render directly to destinationBus.
            renderer->sharedRender(destinationBus, numberOfFrames, outputPosition);
            isFirstRenderer = false;
            continue;
        }
        // Subsequent renderers should render to the m_workBus, which will
        // then be summed to the destinationBus.
        renderer->sharedRender(m_workBus, numberOfFrames, outputPosition);
        destinationBus.sumFrom(m_workBus);
    }
}
Ref<SharedAudioDestination> SharedAudioDestination::create(const CreationOptions& options, AudioDestinationCreationFunction&& ensureFunction)
{
    return adoptRef(*new SharedAudioDestination(options, WTF::move(ensureFunction)));
}

SharedAudioDestination::SharedAudioDestination(const CreationOptions& options, AudioDestinationCreationFunction&& ensureFunction)
    : AudioDestination(options)
    , m_ensureFunction(WTF::move(ensureFunction))
    , m_outputAdapter(SharedAudioDestinationAdapter::ensureAdapter(options, m_ensureFunction))
{
}

SharedAudioDestination::~SharedAudioDestination()
{
    if (isPlaying())
        stop([] (bool) { });
}

void SharedAudioDestination::start(Function<void(Function<void()>&&)>&& dispatchToRenderThread, CompletionHandler<void(bool)>&& completionHandler)
{
    {
        Locker locker { m_dispatchToRenderThreadLock };
        m_dispatchToRenderThread = WTF::move(dispatchToRenderThread);
    }

    setIsPlaying(true);
    protect(m_outputAdapter)->addRenderer(*this, WTF::move(completionHandler));
}

void SharedAudioDestination::stop(CompletionHandler<void(bool)>&& completionHandler)
{
    setIsPlaying(false);
    protect(m_outputAdapter)->removeRenderer(*this, WTF::move(completionHandler));

    {
        Locker locker { m_dispatchToRenderThreadLock };
        m_dispatchToRenderThread = nullptr;
    }
}

unsigned SharedAudioDestination::framesPerBuffer() const
{
    return m_outputAdapter->framesPerBuffer();
}

MediaTime SharedAudioDestination::outputLatency() const
{
    return protect(m_outputAdapter)->outputLatency();
}

void SharedAudioDestination::setIsPlaying(bool isPlaying)
{
    ASSERT(isMainThread());

    if (m_isPlaying == isPlaying)
        return;

    m_isPlaying = isPlaying;

    {
        Locker locker { m_callbackLock };
        if (m_callback)
            m_callback->isPlayingDidChange();
    }
}

void SharedAudioDestination::sharedRender(AudioBus& destinationBus, size_t numberOfFrames, const AudioIOPosition& outputPosition)
{
    if (!m_dispatchToRenderThreadLock.tryLock()) {
        destinationBus.zero();
        return;
    }

    Locker locker { AdoptLock, m_dispatchToRenderThreadLock };
    if (!m_dispatchToRenderThread)
        callRenderCallback(destinationBus, numberOfFrames, outputPosition);
    else {
        BinarySemaphore semaphore;
        m_dispatchToRenderThread([protectedThis = Ref { *this }, destinationBus = Ref { destinationBus }, numberOfFrames, outputPosition, &semaphore]() mutable {
            protectedThis->callRenderCallback(destinationBus, numberOfFrames, outputPosition);
            semaphore.signal();
        });
        semaphore.wait();
    }
}

class NullAudioIOCallback final : public AudioIOCallback {
public:
    static NullAudioIOCallback& singleton()
    {
        static NeverDestroyed<NullAudioIOCallback> callback;
        return callback.get();
    }
private:
    void render(AudioBus&, size_t, const AudioIOPosition&) final { }
    void isPlayingDidChange() final { }
};

#if PLATFORM(IOS_FAMILY)
void SharedAudioDestination::setSceneIdentifier(const String& identifier)
{
    if (protect(m_outputAdapter)->sceneIdentifier() == identifier)
        return;

    bool wasPlaying = isPlaying();

    if (wasPlaying)
        protect(m_outputAdapter)->removeRenderer(*this, [] (bool) { });

    // We need to re-create the outputAdapter when the sceneIdentifier
    // changes, as the adapter may be shared with other destinations
    // whose sceneIdentifier is _not_ changing.
    m_outputAdapter = SharedAudioDestinationAdapter::ensureAdapter({
        NullAudioIOCallback::singleton(),
        inputDeviceId(),
        numberOfInputChannels(),
        numberOfOutputChannels(),
        sampleRate(),
        identifier,
        outputDeviceId(),
        isSilent(),
    }, m_ensureFunction);

    if (wasPlaying)
        protect(m_outputAdapter)->addRenderer(*this, [] (bool) { });
}
#endif

void SharedAudioDestination::setSinkId(const String& persistentDeviceId, bool isSilent, CompletionHandler<void(bool)>&& completionHandler)
{
    assertIsMainThread();

    // Destinations sharing an output adapter share its output device, so a routing change moves
    // this destination to the adapter for the requested sink, leaving other destinations
    // unaffected.
    Ref newAdapter = SharedAudioDestinationAdapter::ensureAdapter({
        NullAudioIOCallback::singleton(),
        inputDeviceId(),
        numberOfInputChannels(),
        numberOfOutputChannels(),
        sampleRate(),
#if PLATFORM(IOS_FAMILY)
        protect(m_outputAdapter)->sceneIdentifier(),
#endif
        persistentDeviceId,
        isSilent,
    }, m_ensureFunction);

    // Ask the underlying destination to acquire the output device before switching over; on
    // failure the current adapter and routing stay in effect (an unused new adapter is dropped).
    newAdapter->setSinkId(persistentDeviceId, isSilent, [protectedThis = Ref { *this }, newAdapter, persistentDeviceId, isSilent, completionHandler = WTF::move(completionHandler)](bool success) mutable {
        assertIsMainThread();
        if (!success) {
            completionHandler(false);
            return;
        }

        protectedThis->setSinkSelection(persistentDeviceId, isSilent);

        if (protectedThis->m_outputAdapter.ptr() == newAdapter.ptr()) {
            completionHandler(true);
            return;
        }

        bool wasPlaying = protectedThis->isPlaying();
        if (wasPlaying)
            protect(protectedThis->m_outputAdapter)->removeRenderer(protectedThis, [] (bool) { });

        protectedThis->m_outputAdapter = WTF::move(newAdapter);

        if (wasPlaying)
            protect(protectedThis->m_outputAdapter)->addRenderer(protectedThis, WTF::move(completionHandler));
        else
            completionHandler(true);
    });
}


} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
