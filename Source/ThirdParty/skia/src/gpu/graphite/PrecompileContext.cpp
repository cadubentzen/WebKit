/*
 * Copyright 2024 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/gpu/graphite/PrecompileContext.h"

#include "src/gpu/graphite/GraphicsPipelineDesc.h"
#include "src/gpu/graphite/Log.h"
#include "src/gpu/graphite/RenderPassDesc.h"
#include "src/gpu/graphite/ResourceProvider.h"
#include "src/gpu/graphite/RuntimeEffectDictionary.h"
#include "src/gpu/graphite/SharedContext.h"

#if defined(SK_ENABLE_PRECOMPILE)
#include "src/gpu/graphite/precompile/SerializationUtils.h"
#endif

namespace skgpu::graphite {

#define ASSERT_SINGLE_OWNER SKGPU_ASSERT_SINGLE_OWNER(&fSingleOwner)

PrecompileContext::~PrecompileContext() {
    ASSERT_SINGLE_OWNER
}

PrecompileContext::PrecompileContext(sk_sp<SharedContext> sharedContext)
    : fSharedContext(sharedContext) {

    // ResourceProviders are not thread-safe. Here we create a ResourceProvider
    // specifically for the thread on which precompilation will occur.
    static constexpr size_t kEmptyBudget = 0;
    fResourceProvider =
            fSharedContext->makeResourceProvider(&fSingleOwner, SK_InvalidGenID, kEmptyBudget);
}

void PrecompileContext::purgePipelinesNotUsedInMs(std::chrono::milliseconds msNotUsed) {
    ASSERT_SINGLE_OWNER

    auto purgeTime = skgpu::StdSteadyClock::now() - msNotUsed;

    fSharedContext->globalCache()->purgePipelinesNotUsedSince(purgeTime);
}


bool PrecompileContext::precompile(sk_sp<SkData> serializedPipelineKey) {
#if defined(SK_ENABLE_PRECOMPILE)
    auto rtEffectDict = std::make_unique<RuntimeEffectDictionary>();

    GraphicsPipelineDesc pipelineDesc;
    RenderPassDesc renderPassDesc;

    if (!DataToPipelineDesc(fSharedContext->caps(),
                            fSharedContext->shaderCodeDictionary(),
                            serializedPipelineKey.get(),
                            &pipelineDesc,
                            &renderPassDesc)) {
        return false;
    }

    sk_sp<GraphicsPipeline> pipeline = fResourceProvider->findOrCreateGraphicsPipeline(
            rtEffectDict.get(),
            pipelineDesc,
            renderPassDesc,
            PipelineCreationFlags::kForPrecompilation);
    if (!pipeline) {
        SKGPU_LOG_W("Failed to create GraphicsPipeline in precompile!");
        return false;
    }

    return true;
#else
    return false;
#endif
}

} // namespace skgpu::graphite
