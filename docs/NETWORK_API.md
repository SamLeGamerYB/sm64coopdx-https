# Native Lua Network API

This document defines the first version of the native networking API exposed to SM64CoopDX Lua mods. It is intentionally general-purpose: it is not tied to Twitch or to any one mod.

## Scope

- HTTPS requests with caller-selected HTTP methods and headers.
- Secure WebSockets only (`wss://`), with both text and binary messages.
- Asynchronous operation: network I/O must never block the game thread.
- Lua callbacks are dispatched only from the game/main thread.
- Response and message payloads are kept in memory. This API does not provide a file-download or file-writing facility.
- The initial implementation target is Windows.

## Lua surface

The API is exposed as a `network` table. The HTTPS functions below are implemented in the current feature branch; WebSockets remain planned and are not exposed yet.

### Permission and capability checks

```lua
local allowed = network.has_permission()
local supported = network.is_supported()
```

`has_permission()` reports the active mod's explicit user-granted network permission. `is_supported()` reports whether the current build includes a native transport (currently Windows only).

### HTTPS requests

```lua
local requestId, err = network.https_request({
    url = "https://example.invalid/api",
    method = "POST", -- defaults to GET
    headers = {
        ["Content-Type"] = "application/json",
        ["Authorization"] = "Bearer ...",
    },
    body = "{\\"hello\\":true}", -- optional; Lua strings may also contain binary data
}, function(status, headers, body, errorMessage)
    -- status is the HTTP status code, or nil if the request failed
    -- headers is the raw response header block
    -- body is the in-memory response body
    -- errorMessage is nil on success, otherwise an error string
end)

if requestId == nil then
    print(err)
end
```

Requests are asynchronous. The callback runs on the game thread. The return value is an opaque numeric request ID; cancellation is not exposed yet.

### WebSocket (planned)



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
- The current HTTPS implementation caps request and response bodies at 1 MiB, headers at 16 KiB, active requests at 8, and response reading at 30 seconds. These initial limits are fixed in the native implementation.
- Queues between worker threads and the game thread are bounded. Overflow must fail safely and be observable, never silently corrupt memory.
- No Lua API, callback, registry reference, or Lua state may be accessed from a worker thread.
- Each operation is owned by the mod that created it. The owner is tracked by a stable mod identity, not by retaining a raw `Mod*` across reloads.
- When a mod is disabled, reloaded, or the Lua state shuts down, its operations are cancelled and their callbacks are discarded. Native resources and worker threads must be fully cleaned up before the Lua state is destroyed.
- Callback execution re-checks that the owner mod still exists in the active mod list and retains network permission.

## Threading model

1. Lua calls validate arguments and permissions on the main thread.
2. Native work is queued to a worker. Workers perform only operating-system/network operations and native memory/queue operations.
3. Workers enqueue immutable completion/message events into a bounded queue.
4. `smlua_update()` drains a bounded number of events and invokes Lua callbacks on the main thread.
5. Shutdown first stops accepting work, signals/cancels outstanding operations, waits for workers to finish, frees queued payloads and registry references, and only then calls `lua_close()`.

A blocking network call or an unbounded wait on the game thread is not acceptable.

## Implementation notes

- The current Windows HTTPS transport uses WinHTTP. WebSocket transport is still to be implemented.
- WebSocket sends must preserve message type (text vs binary) and message boundaries.
- Receive fragments must be reassembled into complete messages while enforcing the configured maximum message size.
- HTTPS response bodies are accumulated in memory under the same bounded-resource policy.
- This document describes the intended contract. It does not claim that the API is implemented or usable yet.
