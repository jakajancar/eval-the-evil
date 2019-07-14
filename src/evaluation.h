#pragma once

#include <assert.h>
#include <libplatform/libplatform.h>
#include <v8.h>
#include "timer.h"
#include "time.h"
#include <atomic>
#include <stdexcept>
#include "error-handling.h"

namespace eval {

class GlobalContext {
  private:
    std::unique_ptr<v8::Platform> platform;

  public:
    GlobalContext()
    {
      platform = v8::platform::NewDefaultPlatform();
      v8::V8::InitializePlatform(platform.get());
      v8::V8::Initialize();
    }

    ~GlobalContext()
    {
      v8::V8::Dispose();
      v8::V8::ShutdownPlatform();
    }
};

class VeryBadArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
    void* Allocate(size_t length) override { return NULL; }
    void* AllocateUninitialized(size_t length) override { return NULL; }
    void Free(void* data, size_t) override { }
};

class ThreadContext {
  friend class RequestContext;
  private:
    // The isolate gets deleted by its {Dispose} method, not by the default
    // deleter. Therefore we have to define a custom deleter for the unique_ptr to
    // call {Dispose}. We have to use the unique_ptr so that the isolate get
    // disposed in the right order, relative to other member variables.
    struct IsolateDeleter {
      void operator()(v8::Isolate* isolate) const { isolate->Dispose(); }
    };

    VeryBadArrayBufferAllocator allocator;
    std::unique_ptr<v8::Isolate, IsolateDeleter> isolate_owning;
    v8::Isolate *isolate;
    v8::Isolate::Scope isolate_scope;
    v8::HandleScope handle_scope;
    // We use a separate context for serializing our JSON responses, because
    // we don't want toJSON() or some other weirdness breaking the protocol.
    v8::Local<v8::Context> response_context;

    static std::unique_ptr<v8::Isolate, IsolateDeleter> create_isolate(v8::ArrayBuffer::Allocator *allocator)
    {
      v8::ResourceConstraints resource_constraints;
      resource_constraints.set_max_semi_space_size_in_kb(1024);
      resource_constraints.set_max_old_space_size(64);

      v8::Isolate::CreateParams create_params;
      create_params.constraints = resource_constraints;
      create_params.array_buffer_allocator = allocator;

      return std::unique_ptr<v8::Isolate, IsolateDeleter>(v8::Isolate::New(create_params));
    }

  public:
    ThreadContext() :
        isolate_owning(create_isolate(&allocator)),
        isolate(isolate_owning.get()),
        isolate_scope(isolate),
        handle_scope(isolate),
        response_context(v8::Context::New(isolate))
    {
      isolate->SetFatalErrorHandler([](const char *location, const char *message) {
        std::stringstream stream;
        stream << "V8 fatal error: " << message << " (in " << location << ")";
        throw_with_trace(std::runtime_error(stream.str()));
      });
    }

    ~ThreadContext()
    {
    }
};

template<typename Callback>
class NearHeapLimitCallbackScope
{
  friend class RequestContext;
  private:
    v8::Isolate *isolate;
    const Callback &callback;
    NearHeapLimitCallbackScope(v8::Isolate *isolate, const Callback &callback): isolate(isolate), callback(callback)
    {
      isolate->AddNearHeapLimitCallback(wrapper, this);
    }

    ~NearHeapLimitCallbackScope()
    {
      isolate->RemoveNearHeapLimitCallback(wrapper, 0);
    }

    static size_t wrapper(void *data, size_t current_heap_limit, size_t initial_heap_limit)
    {
      static_cast<NearHeapLimitCallbackScope*>(data)->callback();
      return current_heap_limit;
    }
};

class RequestContext {
  private:
    ThreadContext *thread;
    v8::HandleScope handle_scope;
    v8::Local<v8::Context> user_context;
    std::unique_ptr<v8::String::Utf8Value> response_utf8;

  public:
    RequestContext(ThreadContext *thread) :
        thread(thread),
        handle_scope(thread->isolate),
        user_context(v8::Context::New(thread->isolate))
    {
    }

    ~RequestContext()
    {
    }

    void handle_request(const char *request_blob, char *&response_blob, size_t &response_blob_length)
    {
      v8::Local<v8::String> response_string = handle_request_string(request_blob);

      // Convert to UTF-8 and return
      response_utf8 = std::make_unique<v8::String::Utf8Value>(thread->isolate, response_string); // TODO: do not buffer
      response_blob = **response_utf8;
      response_blob_length = response_utf8->length();
    }

  private:
    v8::Local<v8::String> handle_request_string(const char *request_blob)
    {
      assert(response_utf8 == NULL); // Another request already contaminated this RequestContext, create a new one.

      v8::Context::Scope context_scope(user_context);
      v8::TryCatch try_catch(thread->isolate);

      // Parse the request.
      v8::Local<v8::Object> request_context;
      v8::Local<v8::String> request_code;
      uint32_t timeout_millis;
      {
        v8::Local<v8::String> request_string;
        if (!v8::String::NewFromUtf8(thread->isolate, request_blob, v8::NewStringType::kNormal).ToLocal(&request_string))
          return error_response("bad_request", v8_istr("Request is not valid UTF-8."));

        v8::Local<v8::Value> request_value;
        if (!v8::JSON::Parse(user_context, request_string).ToLocal(&request_value))
          return error_response("bad_request", v8_istr("Request is not valid JSON."));

        if (!request_value->IsObject() || request_value->IsArray())
          return error_response("bad_request", v8_istr("Request is not an object."));
        v8::Local<v8::Object> request_object = v8::Local<v8::Object>::Cast(request_value);

        v8::Local<v8::Value> request_context_value;
        if (!request_object->Get(user_context, v8_istr("context")).ToLocal(&request_context_value) ||
            !request_context_value->IsObject())
          return error_response("bad_request", v8_istr("Missing 'context' parameter or it is not an object."));
        request_context = v8::Local<v8::Object>::Cast(request_context_value);

        v8::Local<v8::Value> request_code_value;
        if (!request_object->Get(user_context, v8_istr("code")).ToLocal(&request_code_value) ||
            !request_code_value->IsString())
          return error_response("bad_request", v8_istr("Missing 'code' parameter or it is not a string."));
        request_code = v8::Local<v8::String>::Cast(request_code_value);

        v8::Local<v8::Value> timeout_value = request_object->Get(user_context, v8_istr("timeout")).ToLocalChecked();
        if (timeout_value->IsUndefined())
          timeout_millis = 10;
        else if (timeout_value->IsUint32())
          timeout_millis = v8::Local<v8::Uint32>::Cast(timeout_value)->Value();
        else
          return error_response("bad_request", v8_istr("'timeout' parameter must be a non-negative integer."));
      }

      // Prepare the source code.
      v8::ScriptOrigin origin(v8_istr("<user-code>"));
      v8::ScriptCompiler::Source source(request_code, origin);

      // Prepare the implicit context.
      v8::Local<v8::Object> implicit_context = v8::Object::New(thread->isolate);
      implicit_context->Set(user_context, v8_istr("global"), user_context->Global()).ToChecked();

      // Compile the source code with the implicit and user-provided contexts.
      v8::Local<v8::Object> context_extensions[] = {implicit_context, request_context};
      v8::Local<v8::Function> function;
      if (!v8::ScriptCompiler::CompileFunctionInContext(user_context, &source, 0, {}, 2, context_extensions).ToLocal(&function))
        return error_response("code_error", trycatch_to_detail(user_context, &try_catch));

      // Resource-constrained execution scope for user-defined code
      std::atomic<bool> over_memory(false);
      std::atomic<bool> over_cpu(false);
      bool success = false;
      v8::Local<v8::String> retval_stringified;
      uint32_t time = 0;
      {
        auto oom_callback = [&]() {
          over_memory.store(true);
          thread->isolate->TerminateExecution();
        };
        NearHeapLimitCallbackScope<decltype(oom_callback)> oom_scope(thread->isolate, oom_callback);

        auto cpu_callback = [&]{
          over_cpu.store(true);
          thread->isolate->TerminateExecution();
        };
        Timer<decltype(cpu_callback)> timer(std::chrono::milliseconds(timeout_millis), CLOCK_THREAD_CPUTIME_ID, cpu_callback);

        const uint64_t start = clock_gettime_nanos(CLOCK_THREAD_CPUTIME_ID);
        v8::Local<v8::Value> retval;
        if (
            function->Call(user_context, user_context->Global(), 0, {}).ToLocal(&retval) &&
            v8::JSON::Stringify(user_context, retval).ToLocal(&retval_stringified)
        ) {
          success = true;
        }
        const uint64_t end = clock_gettime_nanos(CLOCK_THREAD_CPUTIME_ID);
        time = (end - start) / 1e6;
      }
      if (thread->isolate->IsExecutionTerminating()) {
        thread->isolate->CancelTerminateExecution();
        if (over_memory.load())
          return error_response("code_error", v8_istr("Memory limit exceeded."));
        else if (over_cpu.load())
          return error_response("code_error", v8_istr("CPU time limit exceeded."));
        else
          throw_with_trace(std::runtime_error("Execution terminating but neither over memory or cpu time limits?"));
      } else if (!success) {
        return error_response("code_error", trycatch_to_detail(user_context, &try_catch));
      } else if (retval_stringified.IsEmpty()) {
        throw_with_trace(std::runtime_error("Execution succeeded but retval is empty?"));
      }

      // Stringify may return `undefined` in several cases, which is clearly not
      // valid JSON. Fix that here.
      if (retval_stringified->Length() == 9) { // convert to UTF-8 only if short
        v8::String::Utf8Value utf8(thread->isolate, retval_stringified);
        if (utf8.length() == 9 && strcmp(*utf8, "undefined") == 0)
          retval_stringified = v8_istr("null");
      }

      return success_response(retval_stringified, time);
    }

    /* Response generation */

    v8::Local<v8::String> success_response(v8::Local<v8::String> retval, uint32_t time)
    {
      v8::Local<v8::Context> response_context = thread->response_context;
      v8::Context::Scope context_scope(response_context);

      std::stringstream time_string;
      time_string << time;

      return v8_concat({ v8_istr("{\"status\":\"success\",\"return_value\":"), retval, v8_istr(",\"time\":"), v8_str(time_string.str().c_str()), v8_istr("}") });
    }

    v8::Local<v8::String> error_response(const char *status, v8::Local<v8::String> detail)
    {
      v8::Local<v8::Context> response_context = thread->response_context;
      v8::Context::Scope context_scope(response_context);

      v8::Local<v8::Object> response = v8::Object::New(thread->isolate);
      response->Set(response_context, v8_istr("status"), v8_istr(status)).ToChecked();
      response->Set(response_context, v8_istr("detail"), detail).ToChecked();
      return v8::JSON::Stringify(response_context, response).ToLocalChecked();
    }

    v8::Local<v8::String> trycatch_to_detail(v8::Local<v8::Context> tostring_context, v8::TryCatch *try_catch)
    {
      v8::Local<v8::String> message_string = v8_istr("<no message>");
      {
        v8::Local<v8::Message> message = try_catch->Message();
        if (!message.IsEmpty()) {
          char line_string[22];
          sprintf(line_string, "%d", message->GetLineNumber(tostring_context).FromMaybe(-1));
          message_string = v8_concat({ message->Get(), v8_istr(" ["), message->GetScriptOrigin().ResourceName()->ToString(tostring_context).ToLocalChecked(), v8_istr(":"), v8_str(line_string), v8_istr("]") });
        }
      }

      // The thrown value may have a .stack
      v8::Local<v8::String> stack_trace_string = v8_istr("<no stack trace>");
      {
        v8::Local<v8::Value> stack_trace_value;
        if (try_catch->StackTrace(tostring_context).ToLocal(&stack_trace_value) && stack_trace_value->IsString()) {
          stack_trace_string = v8::Local<v8::String>::Cast(stack_trace_value);
        }
      }

      return v8_concat({ message_string, v8_istr("\n\nStack trace:\n"), stack_trace_string });
    }

    /* v8::String utilities */

    inline v8::Local<v8::String> v8_str(const char* string) {
      return v8::String::NewFromUtf8(thread->isolate, string, v8::NewStringType::kNormal).ToLocalChecked();
    }

    inline v8::Local<v8::String> v8_istr(const char* string) {
      return v8::String::NewFromOneByte(thread->isolate, (const uint8_t*)string, v8::NewStringType::kInternalized).ToLocalChecked();
    }

    template <size_t SIZE> v8::Local<v8::String> v8_concat(const v8::Local<v8::String> (&parts)[SIZE])
    {
      v8::Local<v8::String> buf = v8::String::Empty(thread->isolate);
      for (size_t i = 0; i < SIZE; i++)
        buf = v8::String::Concat(thread->isolate, buf, parts[i]);
      return buf;
    }

    uint64_t clock_gettime_nanos(clockid_t clockid)
    {
      struct timespec time;
      if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
        throw_with_trace(std::system_error(errno, std::generic_category(), "Cannot get time"));
      return time.tv_sec * 1e9 + time.tv_nsec;
    }

};

}
