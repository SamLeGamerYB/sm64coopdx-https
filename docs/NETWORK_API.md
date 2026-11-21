# Native Lua Network API

The native network API is general-purpose and can be used by any SM64CoopDX Lua mod. It is not tied to Twitch or to a specific game mode.

## Supported platforms

The intended target is all platforms on which SM64CoopDX itself is supported: Windows, Linux/SteamOS, and macOS (Apple Silicon and Intel). This is not a port of the game; it uses the existing platform builds.

The Lua API and permission model stay the same on every platform. Native transports are platform-specific: WinHTTP on Windows and libcurl on Linux/macOS. HTTPS and secure WebSockets must be supported on all three targets before the API is considered complete. Plain HTTP and unencrypted WebSockets are never accepted.

## Current status

- **Windows:** HTTPS and WSS use WinHTTP.
- **Linux/SteamOS and macOS:** HTTPS and WSS use libcurl. CI is configured to build and statically link libcurl 8.23.0 with WebSocket support enabled; local builds depend on the system libcurl's available protocols and symbols.
- **Verification:** the new Unix transports have not yet passed CI or runtime tests; do not treat them as verified.
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
- `network.websocket_is_supported()` checks WinHTTP WebSocket entry points on Windows. On Linux/macOS it checks that the runtime libcurl advertises `wss` and exposes the WebSocket send/receive API.

## Opening a link in the default browser

A mod can ask the operating system to open an HTTP(S) link in the user's default browser:

```lua
local opened, err = network.open_url("https://example.com")
if not opened then
    print("Could not open link:", err)
end
```

- The calling mod must have network permission.
- Only `http://` and `https://` URLs are accepted; local files and executable/custom schemes are rejected.
- The global Web Settings domain blacklist is applied to browser links too.
- The function opens the link immediately when called and returns `true` on success, or `nil, errorMessage` if it cannot be opened.
- Access is recorded in the local web access history as `OPENED`, `BLOCKED`, or `ERROR`.


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

The function validates the active mod's permission before creating a worker or performing network activity. It returns a numeric request ID immediately; the completion callback runs later on the game thread. The creating mod can request cancellation of its own pending request with `network.cancel_request(requestId)`, which returns a boolean. The callback is suppressed. Windows checks cancellation between synchronous WinHTTP operations; libcurl uses its transfer-progress callback to abort an active transfer.

### HTTPS limits

| Resource | Limit |
|---|---:|
| Concurrent requests | 8 |
| URL length | 2 KiB |
| Request headers | 16 KiB |
| Request body | 1 MiB |
| Response body | 1 MiB |
| Response reading deadline | 30 seconds |

Only `https://` URLs are accepted. Redirects, automatic cookies, and ambient authentication are disabled in both backends; TLS certificate and hostname verification remain enabled. Mods can still provide explicit headers such as `Authorization`. No response is written to disk.

## WebSockets

All supported builds accept only `wss://` URLs. Windows uses WinHTTP; Linux/macOS use libcurl's optional WebSocket interface (7.86.0+). That upstream interface remains experimental, so builds without it report WebSocket support as unavailable. The connection is asynchronous and returns a numeric socket ID immediately. Callback functions are optional:

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
- The libcurl backend automatically answers WebSocket PING control frames with matching PONG frames.
- The optional third argument is a headers table for explicit handshake headers (for example, `Authorization`). Header names and values are validated and the total is limited to 16 KiB.
- WinHTTP is configured with a 15-second WebSocket keep-alive interval, a 60-second receive timeout, and a 5-second close-handshake timeout. The libcurl backend uses TCP keep-alive, answers PING frames, bounds send waits, and uses a bounded close wait. Both backends still require runtime timing verification.
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
- Lua shutdown marks requests cancelled, asks WebSockets to close, joins workers, and releases callback references before `lua_close()`. A worker already inside a synchronous WinHTTP call must wait for that call to return; the libcurl HTTPS worker uses its progress callback to observe cancellation. Exact WebSocket shutdown timing still needs runtime verification.
- The HTTPS workers use bounded timeouts and response-size limits. Cancellation does not close an HTTP handle from another thread. WinHTTP WebSockets use dedicated send/receive threads; the libcurl WebSocket backend uses one worker loop and bounded queues.

## Remaining work

- Build and test Windows 32-bit/64-bit, Linux/SteamOS, and both macOS architectures.
- Decide whether to keep the upstream experimental libcurl WebSocket API or replace it with a stable dedicated backend before declaring WSS production-ready.
- Verify the minimum libcurl version and runtime availability of its WebSocket protocol on each supported Linux/macOS distribution.
- Run HTTPS and WSS integration tests, including close, cancellation, message fragmentation, and oversized payload cases.
