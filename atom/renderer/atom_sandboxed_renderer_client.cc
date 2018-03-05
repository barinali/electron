// Copyright (c) 2016 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/renderer/atom_sandboxed_renderer_client.h"

#include <string>

#include "atom/common/api/api_messages.h"
#include "atom/common/api/atom_bindings.h"
#include "atom/common/native_mate_converters/string16_converter.h"
#include "atom/common/native_mate_converters/v8_value_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "atom/common/node_bindings.h"
#include "atom/common/options_switches.h"
#include "atom/renderer/api/atom_api_renderer_ipc.h"
#include "atom/renderer/atom_render_view_observer.h"
#include "base/command_line.h"
#include "chrome/renderer/printing/print_web_view_helper.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "content/public/renderer/render_view_observer.h"
#include "ipc/ipc_message_macros.h"
#include "native_mate/converter.h"
#include "native_mate/dictionary.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "third_party/WebKit/public/web/WebKit.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebScriptSource.h"
#include "third_party/WebKit/public/web/WebView.h"

#include "atom/common/node_includes.h"
#include "atom_natives.h"  // NOLINT: This file is generated with js2c

namespace atom {

namespace {

const std::string kIpcKey = "ipcNative";
const std::string kModuleCacheKey = "native-module-cache";

v8::Local<v8::Object> GetModuleCache(v8::Isolate* isolate) {
  mate::Dictionary global(isolate, isolate->GetCurrentContext()->Global());
  v8::Local<v8::Value> cache;

  if (!global.GetHidden(kModuleCacheKey, &cache)) {
    cache = v8::Object::New(isolate);
    global.SetHidden(kModuleCacheKey, cache);
  }

  return cache->ToObject();
}

// adapted from node.cc
v8::Local<v8::Value> GetBinding(v8::Isolate* isolate, v8::Local<v8::String> key,
    mate::Arguments* margs) {
  v8::Local<v8::Object> exports;
  std::string module_key = mate::V8ToString(key);
  mate::Dictionary cache(isolate, GetModuleCache(isolate));

  if (cache.Get(module_key.c_str(), &exports)) {
    return exports;
  }

  auto mod = node::get_builtin_module(module_key.c_str());

  if (!mod) {
    char errmsg[1024];
    snprintf(errmsg, sizeof(errmsg), "No such module: %s", module_key.c_str());
    margs->ThrowError(errmsg);
    return exports;
  }

  exports = v8::Object::New(isolate);
  DCHECK_EQ(mod->nm_register_func, nullptr);
  DCHECK_NE(mod->nm_context_register_func, nullptr);
  mod->nm_context_register_func(exports, v8::Null(isolate),
      isolate->GetCurrentContext(), mod->nm_priv);
  cache.Set(module_key.c_str(), exports);
  return exports;
}

static void EnvGetter(Local<Name> property,
                      const PropertyCallbackInfo<Value>& info) {
  Isolate* isolate = info.GetIsolate();
  if (property->IsSymbol()) {
    return info.GetReturnValue().SetUndefined();
  }
#ifdef __POSIX__
  node::Utf8Value key(isolate, property);
  const char* val = getenv(*key);
  if (val) {
    return info.GetReturnValue().Set(String::NewFromUtf8(isolate, val));
  }
#else  // _WIN32
  node::TwoByteValue key(isolate, property);
  WCHAR buffer[32767];  // The maximum size allowed for environment variables.
  SetLastError(ERROR_SUCCESS);
  DWORD result = GetEnvironmentVariableW(reinterpret_cast<WCHAR*>(*key),
                                         buffer,
                                         arraysize(buffer));
  // If result >= sizeof buffer the buffer was too small. That should never
  // happen. If result == 0 and result != ERROR_SUCCESS the variable was not
  // not found.
  if ((result > 0 || GetLastError() == ERROR_SUCCESS) &&
      result < arraysize(buffer)) {
    const uint16_t* two_byte_buffer = reinterpret_cast<const uint16_t*>(buffer);
    Local<String> rc = String::NewFromTwoByte(isolate, two_byte_buffer);
    return info.GetReturnValue().Set(rc);
  }
#endif
}


static void EnvSetter(Local<Name> property,
                      Local<Value> value,
                      const PropertyCallbackInfo<Value>& info) {
#ifdef __POSIX__
  node::Utf8Value key(info.GetIsolate(), property);
  node::Utf8Value val(info.GetIsolate(), value);
  setenv(*key, *val, 1);
#else  // _WIN32
  node::TwoByteValue key(info.GetIsolate(), property);
  node::TwoByteValue val(info.GetIsolate(), value);
  WCHAR* key_ptr = reinterpret_cast<WCHAR*>(*key);
  // Environment variables that start with '=' are read-only.
  if (key_ptr[0] != L'=') {
    SetEnvironmentVariableW(key_ptr, reinterpret_cast<WCHAR*>(*val));
  }
#endif
  // Whether it worked or not, always return value.
  info.GetReturnValue().Set(value);
}


static void EnvQuery(Local<Name> property,
                     const PropertyCallbackInfo<Integer>& info) {
  int32_t rc = -1;  // Not found unless proven otherwise.
  if (property->IsString()) {
#ifdef __POSIX__
    node::Utf8Value key(info.GetIsolate(), property);
    if (getenv(*key))
      rc = 0;
#else  // _WIN32
    node::TwoByteValue key(info.GetIsolate(), property);
    WCHAR* key_ptr = reinterpret_cast<WCHAR*>(*key);
    SetLastError(ERROR_SUCCESS);
    if (GetEnvironmentVariableW(key_ptr, nullptr, 0) > 0 ||
        GetLastError() == ERROR_SUCCESS) {
      rc = 0;
      if (key_ptr[0] == L'=') {
        // Environment variables that start with '=' are hidden and read-only.
        rc = static_cast<int32_t>(v8::ReadOnly) |
             static_cast<int32_t>(v8::DontDelete) |
             static_cast<int32_t>(v8::DontEnum);
      }
    }
#endif
  }
  if (rc != -1)
    info.GetReturnValue().Set(rc);
}


static void EnvDeleter(Local<Name> property,
                       const PropertyCallbackInfo<Boolean>& info) {
  if (property->IsString()) {
#ifdef __POSIX__
    node::Utf8Value key(info.GetIsolate(), property);
    unsetenv(*key);
#else
    node::TwoByteValue key(info.GetIsolate(), property);
    WCHAR* key_ptr = reinterpret_cast<WCHAR*>(*key);
    SetEnvironmentVariableW(key_ptr, nullptr);
#endif
  }

  // process.env never has non-configurable properties, so always
  // return true like the tc39 delete operator.
  info.GetReturnValue().Set(true);
}


static void EnvEnumerator(const PropertyCallbackInfo<Array>& info) {
  Environment* env = Environment::GetCurrent(info);
  Isolate* isolate = env->isolate();
  Local<Context> ctx = env->context();
  Local<Function> fn = env->push_values_to_array_function();
  Local<Value> argv[NODE_PUSH_VAL_TO_ARRAY_MAX];
  size_t idx = 0;

#ifdef __POSIX__
  int size = 0;
  while (environ[size])
    size++;

  Local<Array> envarr = Array::New(isolate);

  for (int i = 0; i < size; ++i) {
    const char* var = environ[i];
    const char* s = strchr(var, '=');
    const int length = s ? s - var : strlen(var);
    argv[idx] = String::NewFromUtf8(isolate,
                                    var,
                                    String::kNormalString,
                                    length);
    if (++idx >= arraysize(argv)) {
      fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
      idx = 0;
    }
  }
  if (idx > 0) {
    fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
  }
#else  // _WIN32
  WCHAR* environment = GetEnvironmentStringsW();
  if (environment == nullptr)
    return;  // This should not happen.
  Local<Array> envarr = Array::New(isolate);
  WCHAR* p = environment;
  while (*p) {
    WCHAR *s;
    if (*p == L'=') {
      // If the key starts with '=' it is a hidden environment variable.
      p += wcslen(p) + 1;
      continue;
    } else {
      s = wcschr(p, L'=');
    }
    if (!s) {
      s = p + wcslen(p);
    }
    const uint16_t* two_byte_buffer = reinterpret_cast<const uint16_t*>(p);
    const size_t two_byte_buffer_len = s - p;
    argv[idx] = String::NewFromTwoByte(isolate,
                                       two_byte_buffer,
                                       String::kNormalString,
                                       two_byte_buffer_len);
    if (++idx >= arraysize(argv)) {
      fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
      idx = 0;
    }
    p = s + wcslen(s) + 1;
  }
  if (idx > 0) {
    fn->Call(ctx, envarr, idx, argv).ToLocalChecked();
  }
  FreeEnvironmentStringsW(environment);
#endif

  info.GetReturnValue().Set(envarr);
}

base::CommandLine::StringVector GetArgv() {
  return base::CommandLine::ForCurrentProcess()->argv();
}

void InitializeBindings(v8::Local<v8::Object> binding,
                        v8::Local<v8::Context> context) {
  auto isolate = context->GetIsolate();
  mate::Dictionary b(isolate, binding);
  b.SetMethod("get", GetBinding);
  b.SetMethod("crash", AtomBindings::Crash);
  b.SetMethod("hang", AtomBindings::Hang);
  b.SetMethod("getArgv", GetArgv);
  b.SetMethod("getProcessMemoryInfo", &AtomBindings::GetProcessMemoryInfo);
  b.SetMethod("getSystemMemoryInfo", &AtomBindings::GetSystemMemoryInfo);
   // create process.env
  Local<ObjectTemplate> process_env_template =
      ObjectTemplate::New(env->isolate());
  process_env_template->SetHandler(NamedPropertyHandlerConfiguration(
          EnvGetter,
          EnvSetter,
          EnvQuery,
          EnvDeleter,
          EnvEnumerator,
          env->as_external()));

  Local<Object> process_env =
      process_env_template->NewInstance(env->context()).ToLocalChecked();
  b.SetMethod("env", process_env);
}

class AtomSandboxedRenderViewObserver : public AtomRenderViewObserver {
 public:
  AtomSandboxedRenderViewObserver(content::RenderView* render_view,
                                  AtomSandboxedRendererClient* renderer_client)
    : AtomRenderViewObserver(render_view, nullptr),
    v8_converter_(new atom::V8ValueConverter),
    renderer_client_(renderer_client) {
      v8_converter_->SetDisableNode(true);
    }

 protected:
  void EmitIPCEvent(blink::WebLocalFrame* frame,
                    const base::string16& channel,
                    const base::ListValue& args) override {
    if (!frame)
      return;

    auto isolate = blink::MainThreadIsolate();
    v8::HandleScope handle_scope(isolate);
    auto context = frame->MainWorldScriptContext();
    v8::Context::Scope context_scope(context);
    v8::Local<v8::Value> argv[] = {
      mate::ConvertToV8(isolate, channel),
      v8_converter_->ToV8Value(&args, context)
    };
    renderer_client_->InvokeIpcCallback(
        context,
        "onMessage",
        std::vector<v8::Local<v8::Value>>(argv, argv + 2));
  }

 private:
  std::unique_ptr<atom::V8ValueConverter> v8_converter_;
  AtomSandboxedRendererClient* renderer_client_;
  DISALLOW_COPY_AND_ASSIGN(AtomSandboxedRenderViewObserver);
};

}  // namespace


AtomSandboxedRendererClient::AtomSandboxedRendererClient() {
  // Explicitly register electron's builtin modules.
  NodeBindings::RegisterBuiltinModules();
}

AtomSandboxedRendererClient::~AtomSandboxedRendererClient() {
}

void AtomSandboxedRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  RendererClientBase::RenderFrameCreated(render_frame);
}

void AtomSandboxedRendererClient::RenderViewCreated(
    content::RenderView* render_view) {
  new AtomSandboxedRenderViewObserver(render_view, this);
  RendererClientBase::RenderViewCreated(render_view);
}

void AtomSandboxedRendererClient::DidCreateScriptContext(
    v8::Handle<v8::Context> context, content::RenderFrame* render_frame) {

  // Only allow preload for the main frame
  if (!render_frame->IsMainFrame())
    return;

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  std::string preload_script = command_line->GetSwitchValueASCII(
      switches::kPreloadScript);
  if (preload_script.empty())
    return;

  auto isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  // Wrap the bundle into a function that receives the binding object and the
  // preload script path as arguments.
  std::string preload_bundle_native(node::preload_bundle_data,
      node::preload_bundle_data + sizeof(node::preload_bundle_data));
  std::stringstream ss;
  ss << "(function(binding, preloadPath, require) {\n";
  ss << preload_bundle_native << "\n";
  ss << "})";
  std::string preload_wrapper = ss.str();
  // Compile the wrapper and run it to get the function object
  auto script = v8::Script::Compile(
      mate::ConvertToV8(isolate, preload_wrapper)->ToString());
  auto func = v8::Handle<v8::Function>::Cast(
      script->Run(context).ToLocalChecked());
  // Create and initialize the binding object
  auto binding = v8::Object::New(isolate);
  InitializeBindings(binding, context);
  AddRenderBindings(isolate, binding);
  v8::Local<v8::Value> args[] = {
    binding,
    mate::ConvertToV8(isolate, preload_script)
  };
  // Execute the function with proper arguments
  ignore_result(func->Call(context, v8::Null(isolate), 2, args));
}

void AtomSandboxedRendererClient::WillReleaseScriptContext(
    v8::Handle<v8::Context> context, content::RenderFrame* render_frame) {

  // Only allow preload for the main frame
  if (!render_frame->IsMainFrame())
    return;

  auto isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context);
  InvokeIpcCallback(context, "onExit", std::vector<v8::Local<v8::Value>>());
}

void AtomSandboxedRendererClient::InvokeIpcCallback(
    v8::Handle<v8::Context> context,
    const std::string& callback_name,
    std::vector<v8::Handle<v8::Value>> args) {
  auto isolate = context->GetIsolate();
  auto binding_key = mate::ConvertToV8(isolate, kIpcKey)->ToString();
  auto private_binding_key = v8::Private::ForApi(isolate, binding_key);
  auto global_object = context->Global();
  v8::Local<v8::Value> value;
  if (!global_object->GetPrivate(context, private_binding_key).ToLocal(&value))
    return;
  if (value.IsEmpty() || !value->IsObject())
    return;
  auto binding = value->ToObject();
  auto callback_key = mate::ConvertToV8(isolate, callback_name)->ToString();
  auto callback_value = binding->Get(callback_key);
  DCHECK(callback_value->IsFunction());  // set by sandboxed_renderer/init.js
  auto callback = v8::Handle<v8::Function>::Cast(callback_value);
  ignore_result(callback->Call(context, binding, args.size(), args.data()));
}

}  // namespace atom
