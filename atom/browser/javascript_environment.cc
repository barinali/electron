// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/javascript_environment.h"

#include <string>

#include "base/command_line.h"
#include "base/message_loop/message_loop.h"
#include "base/task_scheduler/initialization_util.h"
#include "base/threading/thread_task_runner_handle.h"
#include "content/public/common/content_switches.h"
#include "gin/array_buffer.h"
#include "gin/v8_initializer.h"

#include "atom/common/node_includes.h"
#include "tracing/trace_event.h"

namespace atom {

JavascriptEnvironment::JavascriptEnvironment()
    : initialized_(Initialize()),
      isolate_holder_(base::ThreadTaskRunnerHandle::Get()),
      isolate_(isolate_holder_.isolate()),
      isolate_scope_(isolate_),
      locker_(isolate_),
      handle_scope_(isolate_),
      context_(isolate_, v8::Context::New(isolate_)),
      context_scope_(v8::Local<v8::Context>::New(isolate_, context_)) {}

JavascriptEnvironment::~JavascriptEnvironment() = default;

void JavascriptEnvironment::OnMessageLoopCreated() {
  isolate_holder_.AddRunMicrotasksObserver();
}

void JavascriptEnvironment::OnMessageLoopDestroying() {
  isolate_holder_.RemoveRunMicrotasksObserver();
}

bool JavascriptEnvironment::Initialize() {
  auto* cmd = base::CommandLine::ForCurrentProcess();

  // --js-flags.
  std::string js_flags = cmd->GetSwitchValueASCII(switches::kJavaScriptFlags);
  if (!js_flags.empty())
    v8::V8::SetFlagsFromString(js_flags.c_str(), js_flags.size());

  // The V8Platform of gin relies on Chromium's task schedule, which has not
  // been started at this point, so we have to rely on Node's V8Platform.
  platform_ = node::CreatePlatform(
      base::RecommendedMaxNumberOfThreadsInPool(3, 8, 0.1, 0), nullptr);
  v8::V8::InitializePlatform(platform_);
  node::tracing::TraceEventHelper::SetTracingController(
      new v8::TracingController());
  gin::IsolateHolder::Initialize(
      gin::IsolateHolder::kNonStrictMode, gin::IsolateHolder::kStableV8Extras,
      gin::ArrayBufferAllocator::SharedInstance(), false);
  return true;
}

NodeEnvironment::NodeEnvironment(node::Environment* env) : env_(env) {}

NodeEnvironment::~NodeEnvironment() {
  node::FreeEnvironment(env_);
}

}  // namespace atom
