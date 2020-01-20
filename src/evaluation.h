#pragma once

#include <assert.h>
#include <libplatform/libplatform.h>
#include <v8.h>
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
      constexpr char kFlags[] =
        "--no-expose-wasm ";
      v8::V8::SetFlagsFromString(kFlags, sizeof(kFlags));

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

typedef uint64_t (*UsedCpuTimeCallback)(void *data);
class CpuWatchdog {
  private:
    v8::Isolate *isolate;
    enum class Status { disabled, should_watch, triggered, should_exit };
    std::mutex mutex;
    std::condition_variable cv;
    Status status = Status::disabled;
    UsedCpuTimeCallback used_cpu_time_callback;
    void *used_cpu_time_callback_data;
    uint64_t limit;
    std::thread thread;

    void thread_main()
    {
      std::chrono::nanoseconds next_check = std::chrono::hours(1);
      while (true) {
        std::unique_lock<std::mutex> lock(this->mutex);
        this->cv.wait_for(lock, next_check);
        // after the wait, we own the lock.
        switch (this->status) {
          case Status::disabled:
          case Status::triggered:
            next_check = std::chrono::hours(1);
            break;
          case Status::should_exit:
            return;
          case Status::should_watch:
            uint64_t used_cpu_time = (*used_cpu_time_callback)(used_cpu_time_callback_data);
            if (used_cpu_time >= this->limit) {
              this->isolate->TerminateExecution();
              this->status = Status::triggered;
            } else {
              next_check = std::chrono::nanoseconds(this->limit - used_cpu_time);
            }
        }
      }
    }

  public:
    CpuWatchdog(v8::Isolate *isolate):
        isolate(isolate),
        thread(std::thread(&CpuWatchdog::thread_main, this))
    {
    }

    void arm(uint64_t limit, UsedCpuTimeCallback used_cpu_time_callback, void *used_cpu_time_callback_data)
    {
      std::lock_guard<std::mutex> lock(mutex);
      assert(status == Status::disabled);
      status = Status::should_watch;
      this->used_cpu_time_callback = used_cpu_time_callback;
      this->used_cpu_time_callback_data = used_cpu_time_callback_data;
      this->limit = limit;
      cv.notify_all();
    }

    bool disarm()
    {
      std::lock_guard<std::mutex> lock(mutex);
      bool fired;
      switch (status) {
        case Status::should_watch: fired = false; break;
        case Status::triggered:    fired = true; break;
        default: throw_with_trace(std::runtime_error("Unexpected watchdog status"));
      }
      status = Status::disabled;
      this->used_cpu_time_callback = nullptr;
      this->used_cpu_time_callback_data = nullptr;
      return fired;
    }

    ~CpuWatchdog()
    {
      {
        std::lock_guard<std::mutex> lock(mutex);
        status = Status::should_exit;
        cv.notify_all();
      }
      thread.join();
    }
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

    clockid_t clockid;
    VeryBadArrayBufferAllocator allocator;
    std::unique_ptr<v8::Isolate, IsolateDeleter> isolate_owning;
    v8::Isolate *isolate;
    v8::Isolate::Scope isolate_scope;
    v8::HandleScope handle_scope;
    CpuWatchdog cpu_watchdog;
    uint64_t current_gc_start = 0;
    uint64_t gc_total = 0;
    bool heap_limit_enabled = false;
    bool heap_limit_exceeded = false;

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
        cpu_watchdog(isolate),
        response_context(v8::Context::New(isolate))
    {
      if (pthread_getcpuclockid(pthread_self(), &(this->clockid)) != 0)
        throw_with_trace(std::system_error(errno, std::generic_category(), "Cannot get clock id"));

      isolate->SetData(1, this);

      isolate->SetFatalErrorHandler([](const char *location, const char *message) {
        ThreadContext *self = static_cast<ThreadContext *>(v8::Isolate::GetCurrent()->GetData(1));
        assert(self != nullptr);

        std::stringstream stream;
        stream << "V8 fatal error: " << message << " (in " << location << ")";
        throw_with_trace(std::runtime_error(stream.str()));
      });

      isolate->AddGCPrologueCallback([](v8::Isolate *isolate, v8::GCType type, v8::GCCallbackFlags flags) {
        ThreadContext *self = static_cast<ThreadContext *>(v8::Isolate::GetCurrent()->GetData(1));
        assert(self != nullptr);

        assert(self->current_gc_start == 0);
        self->current_gc_start = self->current_cpu_time();
      });

      isolate->AddGCEpilogueCallback([](v8::Isolate *isolate, v8::GCType type, v8::GCCallbackFlags flags) {
        ThreadContext *self = static_cast<ThreadContext *>(v8::Isolate::GetCurrent()->GetData(1));
        assert(self != nullptr);

        assert(self->current_gc_start != 0);
        uint64_t now = self->current_cpu_time();
        self->gc_total += now - self->current_gc_start;
        self->current_gc_start = 0;
      });

      isolate->AddNearHeapLimitCallback([](void *data, size_t current_heap_limit, size_t initial_heap_limit) {
        ThreadContext *self = static_cast<ThreadContext *>(v8::Isolate::GetCurrent()->GetData(1));
        assert(self != nullptr);

        if (self->heap_limit_enabled) {
          self->heap_limit_exceeded = true;
          self->isolate->TerminateExecution();
        }
        return current_heap_limit;
      }, NULL);

    }

    ~ThreadContext()
    {
    }

    uint64_t current_cpu_time()
    {
      struct timespec time;
      if (clock_gettime(clockid, &time) != 0)
        throw_with_trace(std::system_error(errno, std::generic_category(), "Cannot get time"));
      return time.tv_sec * 1e9 + time.tv_nsec;
    }

};

class RequestContext {
  private:
    ThreadContext *thread;
    v8::HandleScope handle_scope;
    v8::Local<v8::Context> user_context;
    uint64_t start = 0;
    uint64_t gc_total_at_start = 0;
    std::unique_ptr<v8::String::Utf8Value> response_utf8;

  public:
    RequestContext(ThreadContext *thread) :
        thread(thread),
        handle_scope(thread->isolate),
        user_context(v8::Context::New(thread->isolate))
    {
      user_context->SetAlignedPointerInEmbedderData(1, this);
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
    // Called from watchdog thread as well.
    uint64_t used_cpu_time()
    {
      uint64_t now = thread->current_gc_start ? thread->current_gc_start : this->thread->current_cpu_time();
      return now - start - (thread->gc_total - gc_total_at_start);
    }

    v8::Local<v8::String> handle_request_string(const char *request_blob)
    {
      assert(start == 0); // Another request already contaminated this RequestContext, create a new one.

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
        if (timeout_value->IsUndefined()) {
          timeout_millis = 10;
        } else if (timeout_value->IsUint32()) {
          timeout_millis = v8::Local<v8::Uint32>::Cast(timeout_value)->Value();
          if (timeout_millis == 0)
            return error_response("bad_request", v8_istr("'timeout' parameter must be a positive integer."));
        } else {
          return error_response("bad_request", v8_istr("'timeout' parameter must be a positive integer."));
        }
      }

      // Prepare the source code.
      v8::ScriptOrigin origin(v8_istr("<user-code>"));
      v8::ScriptCompiler::Source source(request_code, origin);

      // Prepare the implicit context.
      v8::Local<v8::Object> implicit_context = v8::Object::New(thread->isolate);
      // todo move into ctor
      implicit_context->Set(user_context, v8_istr("global"), user_context->Global()).ToChecked();
      bool tmp = implicit_context->SetNativeDataProperty(user_context, v8_istr("cputime"), [](v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value> &info) {
        v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
        RequestContext *request_context = static_cast<RequestContext *>(context->GetAlignedPointerFromEmbedderData(1));
        uint32_t used_time_ms = (request_context->used_cpu_time() + 1e6 - 1) / 1e6;
        info.GetReturnValue().Set(used_time_ms);
      }).ToChecked();
      assert(tmp == true);

      // Compile the source code with the implicit and user-provided contexts.
      v8::Local<v8::Object> context_extensions[] = {implicit_context, request_context};
      v8::Local<v8::Function> function;
      if (!v8::ScriptCompiler::CompileFunctionInContext(user_context, &source, 0, {}, 2, context_extensions).ToLocal(&function))
        return error_response("code_error", trycatch_to_detail(user_context, &try_catch));

      // Run user code
      // 1. Set memory limit
      thread->heap_limit_enabled = true;
      thread->heap_limit_exceeded = false;

      // 2. Set CPU limit
      this->start = thread->current_cpu_time();
      this->gc_total_at_start = thread->gc_total;
      thread->cpu_watchdog.arm(timeout_millis * 1e6, [](void *data){
        RequestContext *self = static_cast<RequestContext *>(data);
        return self->used_cpu_time();
      }, this);

      // 3. Run
      v8::Local<v8::Value> retval;
      v8::Local<v8::String> retval_stringified;
      bool success = function->Call(user_context, user_context->Global(), 0, {}).ToLocal(&retval) &&
                     v8::JSON::Stringify(user_context, retval).ToLocal(&retval_stringified);
      uint64_t used_time = this->used_cpu_time();

      // 4. Unset CPU limit (watchdog may still fire until this is finished)
      bool over_cpu = thread->cpu_watchdog.disarm();
      if (over_cpu)
        assert(used_cpu_time() >= timeout_millis * 1e6);

      // 5. Unset memory limit
      thread->heap_limit_enabled = false;

      // Prepare response
      if (thread->isolate->IsExecutionTerminating()) {
        thread->isolate->CancelTerminateExecution();
        if (thread->heap_limit_exceeded) {
          return error_response("code_error", v8_istr("Memory limit exceeded."));
        } else if (over_cpu) {
          std::stringstream detail;
          detail << "CPU time limit exceeded (limit " << timeout_millis << " ms, "
                 << "used " << used_time / 1e6 << " ms, "
                 << "plus " << (this->thread->gc_total - this->gc_total_at_start) / 1e6 << " ms for gc).";
          return error_response("code_error", v8_str(detail.str().c_str()));
        } else {
          throw_with_trace(std::runtime_error("Execution terminating but neither over memory or cpu time limits?"));
        }
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

      return success_response(retval_stringified, used_time);
    }

    /* Response generation */

    v8::Local<v8::String> success_response(v8::Local<v8::String> retval, uint64_t time)
    {
      v8::Local<v8::Context> response_context = thread->response_context;
      v8::Context::Scope context_scope(response_context);

      uint32_t time_ms = (time + 1e6 - 1) / 1e6;

      std::stringstream time_string;
      time_string << time_ms;

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
};

}
