# SM64CoopDX – HTTPS & WebSocket API

A fork of [Super Mario 64 Coop Deluxe](https://github.com/coop-deluxe/sm64coopdx) that gives Lua mods a permission-based way to communicate with online services.

The goal is to let modders add online features to their mods without having to implement their own native networking code. Examples include Twitch integrations, external APIs, webhooks, online leaderboards, and other services.

> **For mod developers:** Start with the [Quick start](#quick-start-for-mod-developers) below. The full API reference is available in [docs/NETWORK_API.md](https://github.com/SamLeGamerYB/sm64coopdx-https/blob/feature/websocket-api/docs/NETWORK_API.md).

## Quick start for mod developers

The network API is exposed to Lua mods through the global `network` table.

### 1. Ask the user to enable network access

Network access is **opt-in for each local mod** and is disabled by default.

The user must enable **Allow network access** for your mod in the game's host mod list. Your mod cannot grant itself permission through its metadata or Lua code.

Always check permission before trying to use the network:

```lua
if not network.has_permission() then
    print("This mod needs network access. Enable it in the host mod list.")
    return
end

if not network.is_supported() then
    print("HTTPS is not available in this game build.")
    return
end
```

For WebSockets, check the separate capability:

```lua
if not network.websocket_is_supported() then
    print("Secure WebSockets are not available in this build.")
    return
end
```

### 2. Make an HTTPS request

Use `network.https_request(options, callback)` to send an asynchronous HTTPS request.

```lua
local requestId, err = network.https_request({
    url = "https://example.com/api/status",
    method = "GET", -- optional; GET is the default
    headers = {
        ["Accept"] = "application/json",
    },
}, function(status, responseHeaders, responseBody, errorMessage)
    if errorMessage then
        print("Request failed: " .. errorMessage)
        return
    end

    print("HTTP status: " .. tostring(status))
    print("Response: " .. responseBody)
end)

if not requestId then
    print("Could not start request: " .. tostring(err))
end
```

The request is started asynchronously. The callback is called later on the game thread; it is not called immediately by `https_request`.

You can also send a POST request with a body:

```lua
local requestId, err = network.https_request({
    url = "https://example.com/api/events",
    method = "POST",
    headers = {
        ["Content-Type"] = "application/json",
    },
    body = '{"event":"player_joined"}',
}, function(status, responseHeaders, responseBody, errorMessage)
    if errorMessage then
        print("POST failed: " .. errorMessage)
        return
    end

    print("Server replied with status " .. tostring(status))
end)
```

### 3. Connect to a secure WebSocket

Use `network.websocket_connect(url, callbacks, headers)` to open a secure WebSocket connection (`wss://`).

```lua
local socketId, err = network.websocket_connect(
    "wss://example.com/socket",
    {
        on_open = function(id)
            print("Connected!")
            network.websocket_send(id, "Hello from my SM64CoopDX mod", "text")
        end,

        on_message = function(id, payload, isBinary)
            if isBinary then
                print("Received binary message: " .. tostring(#payload) .. " bytes")
            else
                print("Received: " .. payload)
            end
        end,

        on_error = function(id, message)
            print("WebSocket error: " .. message)
        end,

        on_close = function(id)
            print("WebSocket closed")
        end,
    },
    {
        ["Authorization"] = "Bearer YOUR_TOKEN",
    }
)

if not socketId then
    print("Could not connect: " .. tostring(err))
end
```

The socket ID is returned before the connection is ready. Send messages from `on_open`, after the handshake succeeds.

To send binary data, use `network.websocket_send(socketId, payload, "binary")`. To close a connection, call `network.websocket_close(socketId)`.

### 4. Open a link in the user's default browser

Use `network.open_url(url)` to ask the operating system to open an HTTP or HTTPS link in the user's default web browser.

```lua
local opened, err = network.open_url("https://example.com")

if not opened then
    print("Could not open link: " .. tostring(err))
end
```

This function:
- Requires the user to have enabled network access for your mod.
- Accepts only `http://` and `https://` URLs.
- Checks the global Web Settings blacklist before opening the link.
- Records the attempt in the network access history.

It opens the user's browser; it does **not** make an HTTP request or return the web page's contents.

### 5. Cancel a request or close a connection

```lua
-- Cancel an HTTP request started by this mod:
local cancelled = network.cancel_request(requestId)

-- Close a WebSocket connection created by this mod:
local closed = network.websocket_close(socketId)
```

A mod can only cancel its own HTTP requests and send to or close WebSocket connections that it created.

## API overview

| Function | Description |
| --- | --- |
| `network.has_permission()` | Returns whether this mod has been allowed to access the network. |
| `network.is_supported()` | Returns whether HTTPS is available in this build. |
| `network.websocket_is_supported()` | Checks whether secure WebSockets are available. |
| `network.https_request(options, callback)` | Starts an asynchronous HTTPS request. |
| `network.cancel_request(requestId)` | Cancels one of this mod's pending HTTPS requests. |
| `network.websocket_connect(url, callbacks, headers)` | Starts an asynchronous secure WebSocket connection. |
| `network.websocket_send(id, payload, kind)` | Queues a text or binary WebSocket message. |
| `network.websocket_close(id)` | Closes one of this mod's WebSocket connections. |
| `network.open_url(url)` | Opens an HTTP(S) link in the user's default browser, subject to permission and the global blacklist. |

See **[docs/NETWORK_API.md](https://github.com/SamLeGamerYB/sm64coopdx-https/blob/feature/websocket-api/docs/NETWORK_API.md)** for complete function details, callback parameters, limits, threading behavior, and platform notes.

## Security and permissions

- Network access is disabled by default and must be explicitly granted by the user for each local mod.
- A mod cannot enable its own permission.
- Only encrypted `https://` and `wss://` connections are accepted. Plain HTTP and unencrypted WebSockets are not supported.
- TLS certificate and hostname verification remain enabled.
- Redirects, automatic cookies, and ambient authentication are disabled.
- Requests and WebSocket connections are associated with the mod that created them. A mod cannot operate on another mod's request or socket.
- If permission is revoked, the mod is reloaded, or the mod is unloaded, its pending network work is cancelled or closed and its callbacks are discarded.
- Network responses are delivered to Lua in memory; the API does not provide a file-download or file-writing function.

**Modder responsibility:** Treat all data received from online services as untrusted input. Protect API tokens and credentials, avoid exposing secrets in logs or on-screen messages, validate server responses, and handle connection failures. The user's permission to access the network is not an endorsement of the service your mod contacts.

## Limits

The API applies resource limits to keep network activity bounded.

### HTTPS

| Resource | Limit |
| --- | ---: |
| Concurrent requests | 8 |
| URL length | 2 KiB |
| Request headers | 16 KiB |
| Request body | 1 MiB |
| Response body | 1 MiB |
| Response reading deadline | 30 seconds |

### WebSockets

| Resource | Limit |
| --- | ---: |
| Concurrent connections | 4 |
| URL length | 2 KiB |
| Individual message | 1 MiB |
| Queued outgoing messages | 8 |
| Queued incoming events | 32 |
| Queued incoming payload data | 1 MiB |

## Platform support

The native transport is platform-specific, while the Lua API and permission model are shared.

| Platform | HTTPS | Secure WebSockets |
| --- | --- | --- |
| Windows | WinHTTP | WinHTTP |
| Linux / SteamOS | libcurl | libcurl (when WebSocket support is available) |
| macOS (Intel / Apple Silicon) | libcurl | libcurl (when WebSocket support is available) |

The CI workflow currently builds Windows, Linux, SteamOS, and macOS Intel targets successfully. The macOS Apple Silicon job still needs a successful run. Runtime availability of WebSockets can depend on the libcurl build; mods should always use `network.websocket_is_supported()` rather than assuming it is available.

## Documentation

- **[Native Lua Network API](https://github.com/SamLeGamerYB/sm64coopdx-https/blob/feature/websocket-api/docs/NETWORK_API.md)** – API usage, callbacks, limits, permissions, and platform behavior.
- **[Network API implementation notes](https://github.com/SamLeGamerYB/sm64coopdx-https/blob/feature/websocket-api/docs/NETWORK_API_IMPLEMENTATION.md)** – internal architecture and implementation details.

## Building

See the upstream [SM64CoopDX build instructions](https://github.com/coop-deluxe/sm64coopdx) and this fork's GitHub Actions workflows for platform-specific build configuration.

## Project status

This is an ongoing fork. The API and its platform backends are being developed and tested. Builds passing in CI do not, by themselves, guarantee that every HTTPS or WebSocket use case has been runtime-tested on every operating system.

Contributions, bug reports, and compatibility testing are welcome.
