# Network API implementation notes

This file records repository-specific integration constraints discovered while preparing the native implementation. It is not a claim that the API is implemented.

## Confirmed integration points

- Lua state creation and teardown are centralized in `src/pc/lua/smlua.c`.
- `smlua_bind_functions()` is called after the Lua libraries are loaded and before any mod scripts execute.
- `smlua_update()` runs once per game update and is the safe place to dispatch queued completion events to Lua.
- `smlua_call_hook()` in `smlua_hooks.c` temporarily sets `gLuaActiveMod` and restores it after the callback. Network callbacks should follow the same ownership discipline.
- Mod identity is available as `Mod.relativePath`. A worker must never retain a `struct Mod *`, because `mods_clear()` frees mod objects.
- Lua registry references are valid only for the current Lua state. All network workers and queued events must be stopped/discarded before `lua_close()`.

## Platform and build constraints

- The existing CI builds Windows, Linux, and macOS.
- Windows currently links WinINet and Winsock; WinHTTP is not currently linked.
- Linux/macOS currently link libcurl.
- The first supported native transport is Windows. Non-Windows builds must still compile cleanly, with explicit unsupported-platform results until a transport is implemented there.
- The source tree is discovered automatically from `SRC_DIRS`, so a new C source in `src/pc/lua` is included without a separate source-list edit.

## Required implementation invariants

1. No network worker may call Lua or access the Lua registry.
2. Completion callbacks are dispatched only on the game thread.
3. Every operation is associated with a stable mod identity and is discarded if that mod is no longer active.
4. Shutdown must prevent new work, cancel/close active transport handles, join workers, clear queued events and release callback references before Lua is closed.
5. All queues, request bodies, response bodies, headers, and WebSocket messages need explicit size limits.
6. Only `https://` and `wss://` are accepted. Redirects must not downgrade to plaintext.
7. The API must not expose a file-writing/download primitive; payloads are delivered in memory.
8. WebSocket send/receive must distinguish text and binary payloads.

## Permissions gap

The current mod model has no general network-permission field or per-mod permission UI. A secure default-deny permission flow must be designed and integrated before network access can be described as permission-protected. Merely checking that a mod called a Lua function is not user consent.

## Verification checklist

- Windows 32-bit and 64-bit compile/link.
- Linux and macOS compile with unsupported transport stubs.
- HTTPS success, HTTP error status, invalid URL, TLS failure, timeout, oversized response, and cancellation.
- WebSocket connect, text/binary send and receive, remote close, local close, reconnect/error paths, oversized frame, and cancellation.
- Mod disable/reload while requests are active.
- Lua shutdown while requests are active.
- Verify callbacks run on the game thread and no callback runs after owner unload.
- Verify no worker can access Lua after teardown.
