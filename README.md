# Eval the Evil

[![Releases](https://badgen.net/github/release/jakajancar/eval-the-evil/stable)](https://github.com/jakajancar/eval-the-evil/releases)
[![CircleCI](https://badgen.net/circleci/github/jakajancar/eval-the-evil)](https://circleci.com/gh/jakajancar/eval-the-evil)
![License](https://badgen.net/github/license/jakajancar/eval-the-evil)

Eval the Evil safely evaluates untrusted JavaScript snippets, passing them JSON-compatible contexts, and returns JSON-compatible results:

      $ echo '{"context":{"i":1},"code":"return i + 1"}' | nc -N localhost 1101
      {"status":"success","return_value":2}

It is intended for executing a large number of different, small (~10 lines), simple, synchronous scripts that describe business logic. It is not intended for running long-lived, persistent "apps" that require larger amounts of memory, NPM packages, make outbound connections, and so on. For that, maybe use  [CloudFlare Workers](https://www.cloudflare.com/products/cloudflare-workers/) or [AWS Lambda](https://aws.amazon.com/lambda/). Similarly, it is not meant for integrating tightly with your app. For that, you can use [V8](https://v8.dev) bindings such as [J2V8](https://github.com/eclipsesource/J2V8) (Java), [isolated-vm](https://github.com/laverdet/isolated-vm) (Node.js), [V8js](https://github.com/phpv8/v8js) (PHP) and similar, or even V8 directly (C++).

Eval the Evil's design goals are:

  - **Secure**: Each request is fully isolated. Also, powered by V8, the JavaScript engine behind Chrome, so you know there are some vested interests in keeping it secure.
  - **Co-locatable**: A single, well-behaved "sidekick" process living alongside your app, in the same container or at least pod (not optimized for network). It has a fixed number of threads and behaves predictably under load.
  - **Usable from any language**: Regardless of the previous point, it communicates over TCP, which is even more widely supported than Unix domain sockets.
  - **High performance**: A request is handled without creating a process, thread or V8 isolate. The only thing needed is a fresh V8 context, and even these are pre-prepared to minimize latency.

Limitations:

  - Heap limited to 64 MB, total thread memory usage expected under 100 MB.
  - Does not currently support ICU (`Intl`, `Date.toLocaleDateString()`, ...).
  - Does not currently support `ArrayBuffer`.

## Usage

### TCP protocol

Request:

    {
        "context": <object>
        "code": <string>
        ["timeout": <int>] // default 10ms
    }

Success response:

    {
      "status": "success",
      "return_value": <any>
    }

Error responses:

    {
      "status": "bad_request", // <- application's problem
      "detail": <string>       // e.g. "Missing 'code' parameter or it is not a string."
    }

or

    {
      "status": "code_error",  // <- code author's problem
      "detail": <string>       // multiline, potentially a stack trace
    }

### Examples

Node.js:

    async function eval_the_evil(request) {
        return new Promise(function(resolve, reject) {
            let buffer = '';
            const socket = new net.Socket();
            socket.connect(1101, '127.0.0.1', () => { socket.end(JSON.stringify(request)); });
            socket.on('data', (chunk) => { buffer+= chunk; });
            socket.on('end', () => { resolve(JSON.parse(buffer)); });
            socket.on('error', reject);
        });
    }

PHP:

    function eval_the_evil(string $code, \stdClass $context) {
        $fp = stream_socket_client("tcp://localhost:1101");
        fwrite($fp, json_encode(['code' => $code, 'context' => $context]));
        stream_socket_shutdown($fp, STREAM_SHUT_WR);
        $response = json_decode(stream_get_contents($fp));
        switch ($response->status) {
            case 'success':
                return $response->return_value;
            default:
                throw new RuntimeException($response->detail);
        }
    }

## Developing

 1. Install Docker Compose (or e.g. Docker Desktop)

 2. (Re-)build and enter the development environment:

        docker-compose run dev-env bash

 3. Inside the container:
      - `bin/build` to build, binary will be placed into `build/`
      - `bin/test` to run tests
