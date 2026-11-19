# Native Lua Network API

The native network API is general-purpose and can be used by any SM64CoopDX Lua mod. It is not tied to Twitch or to a specific game mode.

## Current status

- **HTTPS:** implemented for Windows builds in the current feature branch; not yet build-tested.
- **WebSockets:** planned, not implemented yet.
- **Linux/macOS:** currently return an explicit unsupported result.
- **Permissions:** opt-in per local mod, disabled by default.

## Permission and capability checks

The API is exposed through the global `network` table.

```lua
local allowed = network.has_permission()
local supported = network.is_supported()
```

- `network.has_permission()` returns whether the calling mod has been explicitly allowed to access the network by the user.
- `network.is_supported()` returns whether this build has a native transport. It currently returns true on Windows and false elsewhere.

## HTTPS requests

```lua
local requestId, err = network.https_request({
    url = "https://example.invalid/api",
    method = "POST", -- optional; defaults to GET
    headers = {
        ["Content-Type"] = "application/json",
        ["Authorization"] = "Bearer ...",
    },
    body = '{"hello":true}', -- optional; Lua strings may contain binary data
}, function(status, headers, body, errorMessage)
    -- status: HTTP status code, or nil if the request failed
    -- headers: raw response header block as a string
    -- body: in-memory response body as a Lua string
    -- errorMessage: nil on success, otherwise an error string
end)

if requestId == nil then
    print(err)
else
    -- Later, before completion:
    network.cancel_request(requestId)
end
```

The function validates the active mod's permission before creating a worker or performing network activity. It returns a numeric request ID immediately; the completion callback runs later on the game thread. The creating mod can cancel its own pending request with `network.cancel_request(requestId)`, which returns a boolean.

### HTTPS limits

| Resource | Limit |
|---|---:|
| Concurrent requests | 8 |
| URL length | 2 KiB |
| Request headers | 16 KiB |
| Request body | 1 MiB |
| Response body | 1 MiB |
| Response reading deadline | 30 seconds |

Only `https://` URLs are accepted. WinHTTP redirects are disabled; if the redirect policy cannot be enforced, the request fails. TLS certificate verification remains enabled by WinHTTP defaults. No response is written to disk.

## WebSockets (planned)

The planned API will support only `wss://`, and will preserve text and binary message types:

```lua
local socket = network.websocket_connect("wss://example.invalid/socket", {
    on_open = function() end,
    on_message = function(message, message_type) end,
    on_error = function(message) end,
    on_close = function(code, reason) end,
})

network.websocket_send(socket, "hello", "text")
network.websocket_send(socket, binary_payload, "binary")
network.websocket_close(socket)
```

These functions are design examples only and are not available in the current build.

## Threading and ownership

- Network I/O runs on native worker threads; workers never call Lua or access the Lua registry.
- Completion callbacks are dispatched from `smlua_update()` on the game thread.
- Each request stores a copied mod `relativePath`, not a `struct Mod*`.
- Callback dispatch re-checks that the owner mod is still active and still has permission.
- When a mod's Lua file is hot-reloaded, its pending HTTP requests are cancelled and callbacks from the previous script version are discarded.
- Lua shutdown closes active WinHTTP request handles, joins workers, and releases callback references before `lua_close()`.
- The current Windows worker uses bounded WinHTTP timeouts and a response-size limit. A shutdown may wait for an in-flight request to time out.

## Remaining work

- Implement secure WebSocket connect/send/receive/close with text and binary frames.
- Add explicit cancellation and prompt teardown for in-flight HTTPS requests.
- Implement a non-Windows transport backend or continue returning unsupported.
- Build and test Windows 32-bit/64-bit and all supported non-Windows CI targets.
