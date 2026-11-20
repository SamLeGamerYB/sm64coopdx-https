# Native Lua Network API

The native network API is general-purpose and can be used by any SM64CoopDX Lua mod. It is not tied to Twitch or to a specific game mode.

## Supported platforms

The intended target is all platforms on which SM64CoopDX itself is supported: Windows, Linux/SteamOS, and macOS (Apple Silicon and Intel). This is not a port of the game; it uses the existing platform builds.

The Lua API and permission model stay the same on every platform. Native transports are platform-specific: WinHTTP on Windows and libcurl on Linux/macOS. HTTPS and secure WebSockets must be supported on all three targets before the API is considered complete. Plain HTTP and unencrypted WebSockets are never accepted.

## Current status

- **Windows:** WinHTTP HTTPS and WSS implementations exist in this branch, but have not yet been build-tested.
- **Linux/SteamOS and macOS:** cross-platform libcurl transports are still to be implemented; they currently return an explicit unsupported result.
- **Permissions:** opt-in per local mod, disabled by default.

## Permission and capability checks

The API is exposed through the global `network` table.

```lua
local allowed = network.has_permission()
local supported = network.is_supported()
local wssSupported = network.websocket_is_supported()
```

- `network.has_permission()` returns whether the calling mod has been explicitly allowed to access the network by the user.
- `network.is_supported()` returns whether this build has a working HTTPS transport.
- `network.websocket_is_supported()` returns whether this build has a working secure WebSocket transport. It must reflect runtime libcurl capabilities on Linux/macOS and WinHTTP capabilities on Windows.

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
    -- If needed, call this later, before the request completes:
    -- network.cancel_request(requestId)
end
```

The function validates the active mod's permission before creating a worker or performing network activity. It returns a numeric request ID immediately; the completion callback runs later on the game thread. The creating mod can request cancellation of its own pending request with `network.cancel_request(requestId)`, which returns a boolean. The callback is suppressed; the worker stops at the next safe point after its current synchronous WinHTTP call returns, so cancellation may take up to the current operation's timeout.

### HTTPS limits

| Resource | Limit |
|---|---:|
| Concurrent requests | 8 |
| URL length | 2 KiB |
| Request headers | 16 KiB |
| Request body | 1 MiB |
| Response body | 1 MiB |
| Response reading deadline | 30 seconds |

Only `https://` URLs are accepted. WinHTTP redirects, automatic cookies, and ambient authentication are disabled; if this policy cannot be enforced, the request fails. TLS certificate verification remains enabled by WinHTTP defaults. Mods can still provide explicit headers such as `Authorization`. No response is written to disk.

## WebSockets

All supported builds accept only `wss://` URLs once their native WebSocket backend is implemented. The connection is asynchronous and returns a numeric socket ID immediately. Callback functions are optional:

```lua
local socketId, err = network.websocket_connect("wss://example.invalid/socket", {
    on_open = function(id)
        print("Connected", id)
        network.websocket_send(id, "hello", "text")
    end,
    on_message = function(id, payload, isBinary)
        if isBinary then
            print("Received binary data", #payload)
        else
            print("Received text", payload)
        end
    end,
    on_error = function(id, message)
        print("WebSocket error", message)
    end,
    on_close = function(id)
        print("WebSocket closed", id)
    end,
}, {
    ["Authorization"] = "Bearer ...",
})

if socketId then
    -- The ID is returned before the handshake completes.
    -- Send messages from on_open, once the connection is ready.
    -- For binary data, pass "binary" as the third argument.
    -- network.websocket_send(socketId, binary_payload, "binary")
    -- network.websocket_close(socketId)
else
    print(err)
end
```

- `on_open(id)` runs after the WSS handshake succeeds.
- `on_message(id, payload, isBinary)` receives a Lua string and a boolean distinguishing text from binary frames.
- `on_error(id, message)` reports transport errors.
- `on_close(id)` runs when an established connection ends.
- The optional third argument is a headers table for explicit handshake headers (for example, `Authorization`). Header names and values are validated and the total is limited to 16 KiB.
- WinHTTP is configured with a 15-second WebSocket keep-alive interval, a 60-second receive timeout, and a 5-second close-handshake timeout. Actual idle and shutdown timing still needs runtime verification.
- `websocket_send` and `websocket_close` return booleans. A send returning true means it was queued, not necessarily delivered.
- A mod may only send to or close socket IDs it created itself.

### WebSocket limits

| Resource | Limit |
|---|---:|
| Concurrent connections | 4 |
| URL length | 2 KiB |
| Individual message | 1 MiB |
| Queued outgoing messages | 8 |
| Queued incoming events | 32 |
| Queued incoming payload data | 1 MiB |


## Threading and ownership

- Network I/O runs on native worker threads; workers never call Lua or access the Lua registry.
- Completion callbacks are dispatched from `smlua_update()` on the game thread.
- Each request stores a copied mod `relativePath`, not a `struct Mod*`.
- Callback dispatch re-checks that the owner mod is still active and still has permission.
- When a mod's network permission is revoked or one of its Lua files is hot-reloaded, its pending HTTP requests are marked cancelled, WebSockets are asked to close, and callbacks are discarded.
- Lua shutdown marks requests cancelled, asks WebSockets to close, joins workers, and releases callback references before `lua_close()`. A worker already inside a synchronous WinHTTP call must wait for that call to return; exact WebSocket shutdown timing still needs runtime verification.
- The HTTPS worker uses bounded WinHTTP timeouts and a response-size limit. Cancellation does not close an HTTP handle from another thread; the worker checks the cancellation flag between WinHTTP operations. WebSocket receive/send work is handled by dedicated native threads and bounded queues.

## Remaining work

- Implement libcurl HTTPS and WSS transports for Linux/SteamOS and macOS while preserving the existing Windows WinHTTP backend.
- Confirm the minimum libcurl version needed for WebSocket support and ensure CI/runtime dependencies meet it.
- Build and test Windows 32-bit/64-bit, Linux/SteamOS, and both macOS architectures.
