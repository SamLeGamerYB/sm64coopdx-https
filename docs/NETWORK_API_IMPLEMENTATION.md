# Network API implementation notes

This file records repository-specific integration constraints and the current implementation status.

## Confirmed integration points

- Lua state creation and teardown are centralized in `src/pc/lua/smlua.c`.
- `smlua_bind_functions()` is called after the Lua libraries are loaded and before any mod scripts execute.
- `smlua_update()` runs once per game update and is the safe place to dispatch queued completion events to Lua.
- `smlua_call_hook()` in `smlua_hooks.c` temporarily sets `gLuaActiveMod` and restores it after the callback. Network callbacks should follow the same ownership discipline.
- Mod identity is available as `Mod.relativePath`. A worker must never retain a `struct Mod *`, because `mods_clear()` frees mod objects.
- Lua registry references are valid only for the current Lua state. All network workers and queued events must be stopped/discarded before `lua_close()`.

## Platform and build constraints

- The repository's CI workflow builds Windows, Linux, SteamOS, macOS ARM and macOS Intel. Its pull-request trigger targets `dev` and `feature/websocket-api`; changes on this feature branch can be validated without targeting `main`.
- `scripts/build-libcurl.sh` builds a checksum-pinned static libcurl 8.23.0 with `--enable-websockets`, verifies the `WSS` protocol and TLS feature, and exposes the static archive and OpenSSL paths to the following build step. Linux/SteamOS and both macOS CI targets statically link this backend, avoiding new runtime libcurl/OpenSSL dependencies. Local Unix builds may use the system libcurl and must report WSS unavailable when its WebSocket support is absent.
- Windows CI uses MSYS2 MinGW (both 32-bit and 64-bit toolchains). WinHTTP is a Windows system library, but it must be explicitly linked with `-lwinhttp` if used.
- Windows links WinINet, Winsock, and WinHTTP for the current HTTPS implementation.
- New C sources in `src/pc/lua` are discovered automatically through `SRC_DIRS`.
- Keep the Lua-facing API, permission checks, ownership, event queues, and lifecycle code platform-neutral. Isolate transport backends behind a small interface: WinHTTP on Windows and libcurl on Linux/SteamOS/macOS.
- Transport support must be reported honestly per platform. A compile-only stub is not a functioning HTTPS/WebSocket implementation.

## Current native implementation status

- Windows HTTPS continues to use WinHTTP on worker threads. Linux/SteamOS and macOS now have a libcurl HTTPS worker in the same source file.
- Lua surface: `network.has_permission()`, `network.is_supported()`, `network.websocket_is_supported()`, `network.https_request(options, callback)`, owner-only `network.cancel_request(requestId)`, and `network.websocket_connect(url, callbacks, headers)`, `network.websocket_send(id, payload, kind)`, `network.websocket_close(id)`.
- The HTTPS request accepts a caller-selected method, headers, and optional body. The callback receives `statusCode, rawResponseHeaders, responseBody, errorMessage`.
- At most 8 completed HTTPS callbacks are dispatched per game update, matching the maximum number of concurrent requests.
- At most 32 queued WebSocket events are dispatched per game update; each socket also has bounded incoming and outgoing queues.
- Workers never call Lua. Completed requests are checked and dispatched from `smlua_update()` through `smlua_call_hook()`.
- The owner mod is identified by a copied `relativePath`; callback dispatch re-checks that the owner remains active and still has permission.
- When a mod file is hot-reloaded, the user revokes its network permission, the active mod set is replaced, or the local mod cache is refreshed, its pending HTTP requests are marked cancelled, its WebSockets are asked to close, and callbacks are discarded before old mod objects are freed. The worker checks the flag between synchronous WinHTTP operations; it never closes a handle while another thread is using it.
- HTTPS is restricted to `https://`; WinHTTP redirects, automatic cookies, and ambient authentication are disabled. Request/response bodies are capped at 1 MiB, request headers at 16 KiB, concurrent requests at 8, and response reading at 30 seconds.
- Windows HTTPS/WSS use WinHTTP. Linux/SteamOS and macOS use the libcurl backend. CI builds a static WSS-enabled libcurl and OpenSSL; local builds without a supplied `CURL_PREFIX` use the system libcurl and can expose HTTPS while reporting WSS unavailable.
- The Windows WSS backend dynamically loads the Windows 8+ WinHTTP WebSocket entry points. Linux/SteamOS and macOS use libcurl's optional WebSocket API (introduced in 7.86.0) with a single worker loop, bounded queues, fragmented-message assembly, control-frame handling, and main-thread callback dispatch. The upstream API remains experimental; the CI-specific static build enables it explicitly, while local builds check runtime protocol and symbol availability. Neither the new libcurl backend nor the existing Windows backend has been build/runtime-tested in this environment.
- No build or runtime tests have been executed in this environment. Do not describe this implementation as verified until CI/build tests pass.

## Required implementation invariants

1. No network worker may call Lua or access the Lua registry.
2. Completion callbacks are dispatched only on the game thread.
3. Every operation is associated with a stable mod identity and is discarded if that mod is no longer active.
4. Shutdown prevents new work, marks HTTP requests cancelled, asks WebSockets to close, joins workers, clears pending events and releases callback references before Lua is closed.
5. All queues, request bodies, response bodies, headers, and WebSocket messages need explicit size limits.
6. Only `https://` and `wss://` are accepted. Redirects must not downgrade to plaintext.
7. The API must not expose a file-writing/download primitive; payloads are delivered in memory.
8. WebSocket send/receive must distinguish text and binary payloads.
9. A worker may own copied request data and a stable owner ID only; it must not retain pointers into Lua, mod objects, or mod-owned file buffers.

## Permission groundwork implemented

- `struct Mod` now carries a `networkAccessAllowed` flag. It defaults to false because mod instances are zero-initialized.
- The host mod list displays an explicit per-mod **Allow network access** checkbox. The user must opt in; mod metadata cannot grant access.
- The opt-in is saved as `allow-mod-network:` entries in the normal config file and restored by matching the mod's stable `relativePath`.
- Local mod refresh preserves granted permissions for unchanged mod paths. A newly installed mod or a mod whose path changes remains denied.
- The current HTTPS API checks the active mod's flag before creating a worker, and re-checks it before invoking the callback. Remote mods remain denied unless a deliberate permission UI is provided for them.

## Verification checklist

- Windows 32-bit and 64-bit compile/link.
- Linux, SteamOS and both macOS CI targets build and statically link the pinned WSS-enabled libcurl/OpenSSL backend.
- HTTPS success, HTTP error status, invalid URL, TLS failure, timeout, oversized response, and cancellation.
- WebSocket connect, text/binary send and receive, remote close, local close, reconnect/error paths, oversized frame, and cancellation.
- Mod disable/reload while requests are active.
- Lua shutdown while requests are active.
- Verify callbacks run on the game thread and no callback runs after owner unload.
- Verify no worker can access Lua after teardown.
- Verify denied permissions cannot start a DNS lookup or network request.

## Source audit: mod metadata and UI

- Mod metadata is parsed from leading Lua comment lines in `mod_extract_fields()` in `src/pc/mods/mod.c`. The parser stops at the first non-comment line.
- Existing metadata includes `name`, `incompatible`, `category`, `description`, `pausable`, and `ignore-script-warnings`. There is no network capability field today.
- The built-in host mod list in `djui_panel_host_mods.c` is the current user-facing permission surface. The Lua-generated per-mod menu remains separate.
- Mod metadata is not treated as consent; only the user's persisted checkbox selection grants the local mod permission.
- The Lua runtime lifecycle is suitable for a shared network service, but owner tracking must use a copied `relativePath` string and callback references must remain owned by the current Lua state.
