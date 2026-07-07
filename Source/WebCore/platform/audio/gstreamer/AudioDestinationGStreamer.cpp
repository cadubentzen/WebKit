/*
 *  Copyright (C) 2011, 2012 Igalia S.L
 *  Copyright (C) 2014 Sebastian Dröge <sebastian@centricular.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "AudioDestinationGStreamer.h"

#include "AudioSourceProvider.h"
#include "AudioUtilities.h"
#include "GStreamerCommon.h"
#include "GStreamerQuirks.h"
#include "WebKitWebAudioSourceGStreamer.h"
#include <gst/audio/gstaudiobasesink.h>
#include <gst/gst.h>
#include <wtf/PrintStream.h>
#include <wtf/Scope.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebCore {

GST_DEBUG_CATEGORY(webkit_audio_destination_debug);
#define GST_CAT_DEFAULT webkit_audio_destination_debug

static void initializeAudioDestinationDebugCategory()
{
    ensureGStreamerInitialized();
    registerWebKitGStreamerElements();

    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_audio_destination_debug, "webkitaudiodestination", 0, "WebKit WebAudio Destination");
    });
}

static unsigned long maximumNumberOfOutputChannels()
{
    initializeAudioDestinationDebugCategory();

    static int count = 0;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        auto maxFromEnvironment = StringView::fromLatin1(g_getenv("WEBKIT_GST_MAX_NUMBER_OF_AUDIO_OUTPUT_CHANNELS"));
        if (!maxFromEnvironment.isEmpty()) {
            if (auto value = WTF::parseInteger<int>(maxFromEnvironment)) {
                count = *value;
                return;
            }
        }

        GRefPtr monitor = adoptGRef(gst_device_monitor_new());
        GRefPtr caps = adoptGRef(gst_caps_new_empty_simple("audio/x-raw"));
        gst_device_monitor_add_filter(monitor.get(), "Audio/Sink", caps.get());
        bool started = gst_device_monitor_start(monitor.get());
        auto* devices = gst_device_monitor_get_devices(monitor.get());
        while (devices) {
            GRefPtr device = adoptGRef(GST_DEVICE_CAST(devices->data));
            GRefPtr caps = adoptGRef(gst_device_get_caps(device.get()));
            unsigned size = gst_caps_get_size(caps.get());
            for (unsigned i = 0; i < size; i++) {
                auto* structure = gst_caps_get_structure(caps.get(), i);
                if (gstStructureGetName(structure) != "audio/x-raw"_s)
                    continue;
                if (auto value = gstStructureGet<int>(structure, "channels"_s))
                    count = std::max(count, *value);
            }
            devices = g_list_delete_link(devices, devices);
        }
        GST_DEBUG("maximumNumberOfOutputChannels: %d", count);
        if (started)
            gst_device_monitor_stop(monitor.get());
    });

    return count;
}

Ref<AudioDestination> AudioDestination::create(const CreationOptions& options)
{
    initializeAudioDestinationDebugCategory();
    // FIXME: make use of inputDeviceId as appropriate.

    // FIXME: Add support for local/live audio input.
    if (options.numberOfInputChannels)
        WTFLogAlways("AudioDestination::create(%u, %u, %f) - unhandled input channels", options.numberOfInputChannels, options.numberOfOutputChannels, options.sampleRate);

    return adoptRef(*new AudioDestinationGStreamer(options));
}

float AudioDestination::hardwareSampleRate()
{
    return 44100;
}

unsigned long AudioDestination::maxChannelCount()
{
    return maximumNumberOfOutputChannels();
}

AudioDestinationGStreamer::AudioDestinationGStreamer(const CreationOptions& options)
    : AudioDestination(options)
    , m_renderBus(AudioBus::create(options.numberOfOutputChannels, AudioUtilities::renderQuantumSize, false))
{
}

AudioDestinationGStreamer::~AudioDestinationGStreamer()
{
    auto scopeExit = makeScopeExit([&] {
        notifyStopResult(true);
    });

    if (!m_pipeline)
        return;

    GST_DEBUG_OBJECT(m_pipeline.get(), "Disposing");
    unregisterPipeline(m_pipeline);
    disconnectSimpleBusMessageCallback(m_pipeline.get());
    gst_element_set_state(m_pipeline.get(), GST_STATE_NULL);
}

void AudioDestinationGStreamer::initializePipeline()
{
    if (m_pipeline)
        return;

    static Atomic<uint32_t> pipelineId;
    m_pipeline = gst_pipeline_new(makeString("audio-destination-"_s, pipelineId.exchangeAdd(1)).ascii().data());
    registerActivePipeline(m_pipeline);
    connectSimpleBusMessageCallback(m_pipeline.get(), [weakThis = ThreadSafeWeakPtr { *this }](GstMessage* message) {
        RefPtr destination = weakThis.get();
        if (!destination)
            return;
        destination->handleMessage(message, false);
    });

    m_src = GST_ELEMENT_CAST(g_object_new(WEBKIT_TYPE_WEB_AUDIO_SRC, nullptr));
    webkitWebAudioSourceSetDestination(WEBKIT_WEB_AUDIO_SRC(m_src.get()), this);

    m_audioSink = createAudioSinkElement(SinkDeviceFallback::Allowed);
    m_audioSinkAvailable = m_audioSink;
    if (!m_audioSink) {
        GST_ERROR("Failed to create GStreamer audio sink element");
        return;
    }

    // Probe platform early on for a working audio output device in autoaudiosink.
    auto nameView = StringView::fromLatin1(GST_OBJECT_NAME(m_audioSink.get()));
    if (nameView.startsWith("autoaudiosink"_s)) {
        // Autoaudiosink does the real sink detection in the GST_STATE_NULL->READY transition
        // so it's best to roll it to READY as soon as possible to ensure the underlying platform
        // audiosink was loaded correctly.
        GstStateChangeReturn stateChangeReturn = gst_element_set_state(m_audioSink.get(), GST_STATE_READY);
        if (stateChangeReturn == GST_STATE_CHANGE_FAILURE) {
            GST_ERROR("Failed to change autoaudiosink element state");
            gst_element_set_state(m_audioSink.get(), GST_STATE_NULL);
            m_audioSinkAvailable = false;
            return;
        }
    }

    GstElement* audioConvert = makeGStreamerElement("audioconvert"_s);
    GstElement* audioResample = makeGStreamerElement("audioresample"_s);
    auto clockSync = gst_element_factory_make("clocksync", nullptr);
    m_queue = gst_element_factory_make("queue", nullptr);

    gst_bin_add_many(GST_BIN_CAST(m_pipeline.get()), m_src.get(), audioConvert, audioResample, clockSync, m_queue.get(), m_audioSink.get(), nullptr);

    // Link src pads from webkitAudioSrc to clocksync ! audioConvert ! audioResample ! queue ! audiosink.
    gst_element_link_pads_full(m_src.get(), "src", clockSync, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(clockSync, "src", audioConvert, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(audioConvert, "src", audioResample, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(audioResample, "src", m_queue.get(), "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(m_queue.get(), "src", m_audioSink.get(), "sink", GST_PAD_LINK_CHECK_NOTHING);
}

GRefPtr<GstElement> AudioDestinationGStreamer::createAudioSinkElement(SinkDeviceFallback deviceFallback)
{
    // A silent sink (AudioSinkOptions { type: "none" }) keeps the graph running with no hardware output.
    if (isSilent()) {
        GRefPtr<GstElement> sink = makeGStreamerElement("fakesink"_s);
        if (sink)
            g_object_set(sink.get(), "sync", TRUE, "async", FALSE, nullptr);
        return sink;
    }

#if ENABLE(MEDIA_STREAM)
    // Creating a fresh element is the reliable way to (re)route: device-specific sinks
    // (e.g. alsasink) only read their device configuration on the GST_STATE_NULL->READY transition.
    if (!outputDeviceId().isEmpty()) {
        auto [resolvedId, device] = gstGetAudioOutputDevice(outputDeviceId());
        if (device) {
            if (GRefPtr<GstElement> sink = gst_device_create_element(device.get(), nullptr))
                return sink;
        }
        GST_WARNING_OBJECT(m_pipeline.get(), "Could not create a sink for device '%s'.", outputDeviceId().utf8().data());
        if (deviceFallback == SinkDeviceFallback::Disallowed)
            return nullptr;
        GST_WARNING_OBJECT(m_pipeline.get(), "Using the default output device instead.");
    }
#else
    UNUSED_PARAM(deviceFallback);
#endif

    auto& quirksManager = GStreamerQuirksManager::singleton();
    GRefPtr<GstElement> sink = quirksManager.createWebAudioSink();

    // Configure a sensible buffer-time on whichever real sink autoaudiosink instantiates.
    if (sink) {
        auto nameView = StringView::fromLatin1(GST_OBJECT_NAME(sink.get()));
        if (nameView.startsWith("autoaudiosink"_s)) {
            g_signal_connect(sink.get(), "child-added", G_CALLBACK(+[](GstChildProxy*, GObject* object, gchar*, gpointer) {
                if (GST_IS_AUDIO_BASE_SINK(object))
                    g_object_set(GST_AUDIO_BASE_SINK(object), "buffer-time", static_cast<gint64>(100000), nullptr);
            }), nullptr);
        }
    }

    return sink;
}

bool AudioDestinationGStreamer::replaceAudioSink()
{
    ASSERT(m_pipeline && m_audioSink && m_queue);

    auto newSink = createAudioSinkElement(SinkDeviceFallback::Disallowed);
    if (!newSink) {
        GST_ERROR_OBJECT(m_pipeline.get(), "Failed to create replacement audio sink element, keeping the current sink");
        return false;
    }

    // Attempt to acquire the output device before swapping anything: device sinks open their device
    // on the GST_STATE_NULL->READY transition.
    if (gst_element_set_state(newSink.get(), GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
        GST_ERROR_OBJECT(m_pipeline.get(), "Replacement audio sink failed to reach READY, keeping the current sink");
        gst_element_set_state(newSink.get(), GST_STATE_NULL);
        return false;
    }

    auto oldSink = std::exchange(m_audioSink, WTF::move(newSink));
    gst_element_set_locked_state(oldSink.get(), TRUE);
    gst_element_set_state(oldSink.get(), GST_STATE_NULL);
    gst_element_unlink(m_queue.get(), oldSink.get());
    gst_bin_remove(GST_BIN_CAST(m_pipeline.get()), oldSink.get());

    gst_bin_add(GST_BIN_CAST(m_pipeline.get()), m_audioSink.get());
    gst_element_link_pads_full(m_queue.get(), "src", m_audioSink.get(), "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_sync_state_with_parent(m_audioSink.get());
    m_audioSinkAvailable = true;
    return true;
}

void AudioDestinationGStreamer::setSinkId(const String& persistentDeviceId, bool isSilent, CompletionHandler<void(bool)>&& completionHandler)
{
    if (outputDeviceId() == persistentDeviceId && this->isSilent() == isSilent) {
        completionHandler(true);
        return;
    }

    auto previousDeviceId = outputDeviceId();
    bool previousIsSilent = this->isSilent();
    setSinkSelection(persistentDeviceId, isSilent);

    // Not (fully) started yet: the selection will be applied when the pipeline is created. m_queue is
    // only set once initializePipeline() links the graph, so its absence also covers a failed sink probe.
    if (!m_pipeline || !m_audioSink || !m_queue) {
        completionHandler(true);
        return;
    }

    // Swap the sink element for any routing change: in-place reconfiguration of an already-started
    // sink is not reliable (see createAudioSinkElement()). The caller suspends rendering around this
    // for a running context, so the pipeline is not PLAYING here.
    if (!replaceAudioSink()) {
        setSinkSelection(previousDeviceId, previousIsSilent);
        completionHandler(false);
        return;
    }

    completionHandler(true);
}

unsigned AudioDestinationGStreamer::framesPerBuffer() const
{
    return AudioUtilities::renderQuantumSize;
}

bool AudioDestinationGStreamer::handleMessage(GstMessage* message, bool handleLatencyMessage)
{
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        notifyIsPlaying(false);
        break;
    case GST_MESSAGE_LATENCY:
        if (!handleLatencyMessage)
            break;
        gst_element_call_async(m_pipeline.get(), reinterpret_cast<GstElementCallAsyncFunc>(+[](GstElement* pipeline, gpointer) {
            gst_bin_recalculate_latency(GST_BIN_CAST(pipeline));
        }), nullptr, nullptr);
        break;
    default:
        break;
    }
    return true;
}

void AudioDestinationGStreamer::start(Function<void(Function<void()>&&)>&& dispatchToRenderThread, CompletionHandler<void(bool)>&& completionHandler)
{
    if (!m_pipeline)
        initializePipeline();
    webkitWebAudioSourceSetDispatchToRenderThreadFunction(WEBKIT_WEB_AUDIO_SRC(m_src.get()), WTF::move(dispatchToRenderThread));
    startRendering(WTF::move(completionHandler));
}

void AudioDestinationGStreamer::startRendering(CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(m_audioSinkAvailable);
    ASSERT(m_pipeline);
    m_startupCompletionHandler = WTF::move(completionHandler);
    GST_DEBUG_OBJECT(m_pipeline.get(), "Starting audio rendering, sink %s", m_audioSinkAvailable ? "available" : "not available");

    if (m_isPlaying) {
        notifyStartupResult(true);
        return;
    }

    if (!m_audioSinkAvailable) {
        notifyStartupResult(false);
        return;
    }

    notifyStartupResult(webkitGstSetElementStateSynchronously(m_pipeline.get(), GST_STATE_PLAYING, [weakThis = ThreadSafeWeakPtr { *this }](GstMessage* message) -> bool {
        RefPtr destination = weakThis.get();
        if (!destination)
            return false;
        return destination->handleMessage(message, true);
    }));
}

void AudioDestinationGStreamer::stop(CompletionHandler<void(bool)>&& completionHandler)
{
    stopRendering(WTF::move(completionHandler));
    if (m_src)
        webkitWebAudioSourceSetDispatchToRenderThreadFunction(WEBKIT_WEB_AUDIO_SRC(m_src.get()), nullptr);
}

void AudioDestinationGStreamer::stopRendering(CompletionHandler<void(bool)>&& completionHandler)
{
    m_stopCompletionHandler = WTF::move(completionHandler);
    if (!m_pipeline) {
        notifyStopResult(true);
        return;
    }

    ASSERT(m_audioSinkAvailable);
    GST_DEBUG_OBJECT(m_pipeline.get(), "Stopping audio rendering, sink %s", m_audioSinkAvailable ? "available" : "not available");

    if (!m_isPlaying) {
        GST_DEBUG_OBJECT(m_pipeline.get(), "Already stopped");
        notifyStopResult(true);
        return;
    }

    if (!m_audioSinkAvailable) {
        notifyStopResult(false);
        return;
    }

    notifyStopResult(webkitGstSetElementStateSynchronously(m_pipeline.get(), GST_STATE_READY, [weakThis = ThreadSafeWeakPtr { *this }](GstMessage* message) -> bool {
        RefPtr destination = weakThis.get();
        if (!destination)
            return false;
        return destination->handleMessage(message, true);
    }));
}

void AudioDestinationGStreamer::notifyStartupResult(bool success)
{
    if (success)
        notifyIsPlaying(true);

    callOnMainThreadAndWait([this, completionHandler = WTF::move(m_startupCompletionHandler), success]() mutable {
#ifdef GST_DISABLE_GST_DEBUG
        UNUSED_VARIABLE(this);
#endif
        GST_DEBUG_OBJECT(m_pipeline.get(), "Has start completion handler: %s", boolForPrinting(!!completionHandler));
        if (completionHandler)
            completionHandler(success);
    });
}

void AudioDestinationGStreamer::notifyStopResult(bool success)
{
    if (success)
        notifyIsPlaying(false);

    callOnMainThreadAndWait([this, completionHandler = WTF::move(m_stopCompletionHandler), success]() mutable {
#ifdef GST_DISABLE_GST_DEBUG
        UNUSED_VARIABLE(this);
#endif
        if (m_pipeline)
            GST_DEBUG_OBJECT(m_pipeline.get(), "Has stop completion handler: %s", boolForPrinting(!!completionHandler));
        if (completionHandler)
            completionHandler(success);
    });
}

void AudioDestinationGStreamer::notifyIsPlaying(bool isPlaying)
{
    if (m_isPlaying == isPlaying)
        return;

    GST_DEBUG("Is playing: %s", boolForPrinting(isPlaying));
    m_isPlaying = isPlaying;
    if (m_callback)
        m_callback->isPlayingDidChange();
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
