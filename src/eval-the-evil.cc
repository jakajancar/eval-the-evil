// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO: remove current context

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <thread>

#include <boost/program_options.hpp>
#include <boost/asio.hpp>

#include <libplatform/libplatform.h>
#include <v8.h>

namespace po = boost::program_options;
using boost::asio::ip::tcp;

v8::Local<v8::String> evaluate_request(v8::Isolate *isolate, v8::Local<v8::Context> safe_context, std::string *request_blob);
v8::Local<v8::String> error_response(v8::Isolate *isolate, v8::Local<v8::Context> safe_context, const char *status, v8::Local<v8::String> detail);
v8::Local<v8::String> trycatch_to_detail(v8::Isolate *isolate, v8::Local<v8::Context> tostring_context, v8::TryCatch *try_catch);

class VeryBadArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
    void* Allocate(size_t length) override { return NULL; }
    void* AllocateUninitialized(size_t length) override { return NULL; }
    void Free(void* data, size_t) override { }
};

int main(int argc, char *argv[])
{
  // Process arguments
  int port;
  int num_threads;
  po::options_description desc("Allowed options");
  desc.add_options()
      ("help", "produce help message")
      ("port", po::value<int>(&port)->default_value(1101), "port to listen on")
      ("threads", po::value<int>(&num_threads)->default_value(std::thread::hardware_concurrency()), "number of threads (defaults to hardware concurrency)")
  ;
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  if (vm.count("help")) {
      std::cout << desc << "\n";
      return 1;
  }

  // Initialize V8.
  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  // Start threads
  std::thread threads[num_threads];
  for (int i = 0; i < num_threads; i++) {
    threads[i] = std::thread([port]{
      // Listen for TCP connections.
      boost::asio::io_service io_service;
      tcp::acceptor acceptor(io_service);
      acceptor.open(tcp::v4());
      acceptor.set_option(boost::asio::socket_base::reuse_address(true));
      acceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));
      acceptor.bind(tcp::endpoint(tcp::v4(), port));
      acceptor.listen();

      // Create a new Isolate and make it the current one.
      v8::ResourceConstraints resource_constraints;
      resource_constraints.set_max_semi_space_size_in_kb(1024);
      resource_constraints.set_max_old_space_size(64);

      v8::Isolate::CreateParams create_params;
      create_params.constraints = resource_constraints;
      create_params.array_buffer_allocator = new VeryBadArrayBufferAllocator();

      v8::Isolate* isolate = v8::Isolate::New(create_params);
      {
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);

        isolate->AddNearHeapLimitCallback([](void *isolate, size_t current_heap_limit, size_t initial_heap_limit) {
          static_cast<v8::Isolate*>(isolate)->TerminateExecution();
          return current_heap_limit;
        }, isolate);

        // We use a separate context for serializing our JSON responses, because
        // we don't want toJSON() or some other weirdness breaking the protocol.
        v8::Local<v8::Context> safe_context = v8::Context::New(isolate);

        while (true)
        {
          v8::HandleScope handle_scope(isolate);
          // TODO: Prepare the context here instead, before accepting connection, to reduce latency.

          // Accept a connection
          tcp::socket sock(io_service);
          acceptor.accept(sock);

          // Read the request
          boost::system::error_code error;
          std::string *request_buffer = new std::string();
          std::size_t read_bytes = boost::asio::read(sock, boost::asio::dynamic_buffer(*request_buffer), boost::asio::transfer_all(), error);
          if (error != boost::asio::error::eof)
            throw boost::system::system_error(error);
          assert(request_buffer->length() == read_bytes);

          // Evaluate
          v8::Local<v8::String> response_string = evaluate_request(isolate, safe_context, request_buffer);

          // Send response
          v8::String::Utf8Value response_buffer(isolate, response_string); // TODO: do not buffer
          std::size_t written_bytes = boost::asio::write(sock, boost::asio::buffer(*response_buffer, response_buffer.length()));
          assert(written_bytes == response_buffer.length());
          sock.close();

          delete request_buffer;
        } // run loop
      } // isolate scope
      acceptor.close();
      isolate->Dispose();
      delete create_params.array_buffer_allocator;
    }); // thread
  }
  printf("eval-the-evil listening on port %d.\n", port);

  // Wait for threads to exit
  for (auto &thread : threads) {
    thread.join();
  }

  // Tear down V8.
  v8::V8::Dispose();
  v8::V8::ShutdownPlatform();
  return 0;
}

inline v8::Local<v8::String> v8_str(const char* string) {
  return v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), string, v8::NewStringType::kNormal).ToLocalChecked();
}

inline v8::Local<v8::String> v8_istr(const char* string) {
  return v8::String::NewFromOneByte(v8::Isolate::GetCurrent(), (const uint8_t*)string, v8::NewStringType::kInternalized).ToLocalChecked();
}

template <size_t SIZE> v8::Local<v8::String> v8_concat(const v8::Local<v8::String> (&parts)[SIZE])
{
  v8::Isolate *isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::String> buf = v8::String::Empty(isolate);
  for (size_t i = 0; i < SIZE; i++)
    buf = v8::String::Concat(isolate, buf, parts[i]);
  return buf;
}

//TODO: Remove isolate parameter
v8::Local<v8::String> evaluate_request(v8::Isolate *isolate, v8::Local<v8::Context> safe_context, std::string *request_blob)
{
  // Stuff passed from unsafe to safe context
  //
  // Be careful about what you pass between them. For example, an Object
  // received from untrusted context is not safe to serialize (toJSON).
  v8::Local<v8::String> retval_string;

  {
    // Create a new context.
    v8::Local<v8::Context> user_context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(user_context);
    v8::TryCatch try_catch(isolate);

    // Parse the request.
    v8::Local<v8::Object> request_context;
    v8::Local<v8::String> request_code;
    {
      v8::Local<v8::String> request_string;
      if (!v8::String::NewFromUtf8(isolate, request_blob->c_str(), v8::NewStringType::kNormal).ToLocal(&request_string))
        return error_response(isolate, safe_context, "bad_request", v8_istr("Request is not valid UTF-8."));

      v8::Local<v8::Value> request_value;
      if (!v8::JSON::Parse(user_context, request_string).ToLocal(&request_value))
        return error_response(isolate, safe_context, "bad_request", v8_istr("Request is not valid JSON."));

      if (!request_value->IsObject() || request_value->IsArray())
        return error_response(isolate, safe_context, "bad_request", v8_istr("Request is not an object."));
      v8::Local<v8::Object> request_object = v8::Local<v8::Object>::Cast(request_value);

      v8::Local<v8::Value> request_context_value;
      if (!request_object->Get(user_context, v8_istr("context")).ToLocal(&request_context_value) ||
          !request_context_value->IsObject())
        return error_response(isolate, safe_context, "bad_request", v8_istr("Missing 'context' parameter or it is not an object."));
      request_context = v8::Local<v8::Object>::Cast(request_context_value);

      v8::Local<v8::Value> request_code_value;
      if (!request_object->Get(user_context, v8_istr("code")).ToLocal(&request_code_value) ||
          !request_code_value->IsString())
        return error_response(isolate, safe_context, "bad_request", v8_istr("Missing 'code' parameter or it is not a string."));
      request_code = v8::Local<v8::String>::Cast(request_code_value);
    }

    // Prepare the source code.
    v8::ScriptOrigin origin(v8_istr("<user-code>"));
    v8::ScriptCompiler::Source source(request_code, origin);

    // Prepare the implicit context.
    v8::Local<v8::Object> implicit_context = v8::Object::New(isolate);
    implicit_context->Set(user_context, v8_istr("global"), user_context->Global()).ToChecked();

    // Compile the source code with the implicit and user-provided contexts.
    v8::Local<v8::Object> context_extensions[] = {implicit_context, request_context};
    v8::Local<v8::Function> function;
    if (!v8::ScriptCompiler::CompileFunctionInContext(user_context, &source, 0, {}, 2, context_extensions).ToLocal(&function))
      return error_response(isolate, safe_context, "code_error", trycatch_to_detail(isolate, user_context, &try_catch));

    // Execute the compiled function to get the result, then serialize it.
    v8::Local<v8::Value> retval;
    if (
        !function->Call(user_context, user_context->Global(), 0, {}).ToLocal(&retval) ||
        !v8::JSON::Stringify(user_context, retval).ToLocal(&retval_string)
    ) {
      if (isolate->IsExecutionTerminating()) {
        // Execution terminated by us
        isolate->CancelTerminateExecution();
        return error_response(isolate, safe_context, "code_error", v8_istr("Memory limit exceeded."));
      } else {
        return error_response(isolate, safe_context, "code_error", trycatch_to_detail(isolate, user_context, &try_catch));
      }
    }

    // Stringify may return `undefined` in several cases, which is clearly not
    // valid JSON. Fix that here.
    if (retval_string->Length() == 9) { // convert to UTF-8 only if short
      v8::String::Utf8Value utf8(isolate, retval_string);
      if (utf8.length() == 9 && strcmp(*utf8, "undefined") == 0)
        retval_string = v8_istr("null");
    }
  }

  {
    v8::Context::Scope context_scope(safe_context);

    // Ugly, but more efficient than parsing and stringifying again.
    return v8_concat({ v8_istr("{\"status\":\"success\",\"return_value\":"), retval_string, v8_istr("}") });
  }
}

// TODO: remove isolate parameter
v8::Local<v8::String> error_response(v8::Isolate *isolate, v8::Local<v8::Context> safe_context, const char *status, v8::Local<v8::String> detail)
{
  v8::Context::Scope context_scope(safe_context);

  v8::Local<v8::Object> response = v8::Object::New(isolate);
  response->Set(safe_context, v8_istr("status"), v8_istr(status)).ToChecked();
  response->Set(safe_context, v8_istr("detail"), detail).ToChecked();
  return v8::JSON::Stringify(safe_context, response).ToLocalChecked();
}

v8::Local<v8::String> trycatch_to_detail(v8::Isolate *isolate, v8::Local<v8::Context> tostring_context, v8::TryCatch *try_catch)
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
