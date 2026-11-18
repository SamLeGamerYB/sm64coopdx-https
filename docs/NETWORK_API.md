# Native Lua Network API

This document defines the first version of the native networking API exposed to SM64CoopDX Lua mods. It is intentionally general-purpose: it is not tied to Twitch or to any one mod.

## Scope

- HTTPS requests with caller-selected HTTP methods and headers.
- Secure WebSockets only (`wss://`), with both text and binary messages.
- Asynchronous operation: network I/O must never block the game thread.
- Lua callbacks are dispatched only from the game/main thread.
- Response and message payloads are kept in memory. This API does not provide a file-download or file-writing facility.
- The initial implementation target is Windows.

## Lua surface (proposed)

The API is exposed as a `network` table. The exact names and callback payloads below are the contract to implement; they are not available until the native implementation lands.

### WebSocket

```lua
local socket = network.websocket_connect("wss://example.invalid/socket", {
    on_open = function() end,
    on_message = function(message, message_type) end, -- message_type: "text" or "binary"
    on_error = function(message) end,
    on_close = function(code, reason) end,
})

network.websocket_send(socket, "hello", "text")
network.websocket_send(socket, binary_payload, "binary")
network.websocket_close(socket)
```

The connection function returns an opaque handle immediately. A connection failure is reported asynchronously through `on_error` and/or `on_close`. Binary payloads are Lua strings containing arbitrary bytes; they must never be treated as null-terminated text.

### HTTPS

```lua
local request = network.https_request({
    url = "https://example.invalid/api",
    method = "POST",
    headers = {
        ["Content-Type"] = "application/json",
        ["Authorization"] = "Bearer ...",
    },
    body = "{\"hello\":true}",
}, {
    on_complete = function(response) end,
    on_error = function(message) end,
})
```

The response object will expose the HTTP status, response headers, and an in-memory body. The API must not write response data to disk.

## Security and resource rules

- Mod permissions are opt-in and disabled by default. A mod without the relevant permission cannot start network operations.
- WebSocket URLs must use `wss://`; plain `ws://` is rejected.
- HTTPS URLs must use `https://`; redirects must not downgrade to HTTP.
- Request methods and headers are caller-controlled, subject to validation and bounded sizes.
- Message and response sizes are bounded. The default limit should be conservative and configurable through a documented API/configuration point; changing the limit must not permit unbounded allocation.
- Queues between worker threads and the game thread are bounded. Overflow must fail safely and be observable, never silently corrupt memory.
- No Lua API, callback, registry reference, or Lua state may be accessed from a worker thread.
- Each operation is owned by the mod that created it. The owner is tracked by a stable mod identity, not by retaining a raw `Mod*` across reloads.
- When a mod is disabled, reloaded, or the Lua state shuts down, its operations are cancelled and their callbacks are discarded. Native resources and worker threads must be fully cleaned up before the Lua state is destroyed.
- Callback execution must re-check that the owner mod and Lua state are still valid.

## Threading model

1. Lua calls validate arguments and permissions on the main thread.
2. Native work is queued to a worker. Workers perform only operating-system/network operations and native memory/queue operations.
3. Workers enqueue immutable completion/message events into a bounded queue.
4. `smlua_update()` drains a bounded number of events and invokes Lua callbacks on the main thread.
5. Shutdown first stops accepting work, signals/cancels outstanding operations, waits for workers to finish, frees queued payloads and registry references, and only then calls `lua_close()`.

A blocking network call or an unbounded wait on the game thread is not acceptable.

## Implementation notes

- On Windows, WinHTTP is the intended HTTPS/WebSocket transport because it supports TLS and the WebSocket upgrade APIs.
- WebSocket sends must preserve message type (text vs binary) and message boundaries.
- Receive fragments must be reassembled into complete messages while enforcing the configured maximum message size.
- HTTPS response bodies are accumulated in memory under the same bounded-resource policy.
- This document describes the intended contract. It does not claim that the API is implemented or usable yet.
