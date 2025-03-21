/*
 * Copyright (c) 2011 Google Inc. All rights reserved.
 * Copyright (c) 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "WorkerOrWorkletGlobalScope.h"
#include <JavaScriptCore/Debugger.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class WorkerDebugger final : public JSC::Debugger {
    WTF_MAKE_NONCOPYABLE(WorkerDebugger);
    WTF_MAKE_TZONE_ALLOCATED(WorkerDebugger);
public:
    WorkerDebugger(WorkerOrWorkletGlobalScope&);
    ~WorkerDebugger() override = default;


private:
    // JSC::Debugger
    void attachDebugger() final;
    void detachDebugger(bool isBeingDestroyed) final;
    void recompileAllJSFunctions() final;
    void runEventLoopWhilePaused() final;
    void reportException(JSC::JSGlobalObject*, JSC::Exception*) const final;

    WeakRef<WorkerOrWorkletGlobalScope> m_globalScope;
};

} // namespace WebCore
