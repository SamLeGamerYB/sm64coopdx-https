#include "smlua_network_api.h"

#include "smlua.h"
#include "pc/mods/mods.h"

#include <pthread.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#define NETWORK_API_MAX_REQUESTS 8
#define NETWORK_API_MAX_URL 2048
#define NETWORK_API_MAX_METHOD 32
#define NETWORK_API_MAX_HEADERS 16384
#define NETWORK_API_MAX_BODY (1024 * 1024)
#define NETWORK_API_TIMEOUT_MS 15000

struct NetworkHttpRequest {
    pthread_t thread;
    bool threadStarted;
    bool complete;
    bool cancelled;
    unsigned int id;
    int callbackRef;
    char ownerPath[SYS_MAX_PATH];
    char* url;
    char* method;
    char* headers;
    char* body;
    size_t bodyLength;
    unsigned int statusCode;
    char* responseHeaders;
    char* responseBody;
    size_t responseBodyLength;
    char error[256];
    struct NetworkHttpRequest* next;
};

#ifdef _WIN32
static pthread_mutex_t sNetworkApiMutex = PTHREAD_MUTEX_INITIALIZER;
static struct NetworkHttpRequest* sNetworkApiRequests = NULL;
static unsigned int sNetworkApiNextId = 1;
static unsigned int sNetworkApiRequestCount = 0;
#else
static unsigned int sNetworkApiRequestCount = 0;
#endif

#ifdef _WIN32
static bool network_api_has_https_prefix(const char* url) {
    static const char prefix[] = "https://";
    if (strlen(url) < sizeof(prefix) - 1) { return false; }
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (tolower((unsigned char)url[i]) != prefix[i]) { return false; }
    }
    return true;
}

static bool network_api_valid_token(const char* value) {
    if (value[0] == '\0') { return false; }
    for (const unsigned char* c = (const unsigned char*)value; *c != '\0'; c++) {
        if (!(isalnum(*c) || strchr("!#$%&'*+-.^_`|~", *c) != NULL)) {
            return false;
        }
    }
    return true;
}

static bool network_api_valid_header_value(const char* value) {
    for (const unsigned char* c = (const unsigned char*)value; *c != '\0'; c++) {
        if ((*c < 0x20 && *c != '\t') || *c == 0x7f) { return false; }
    }
    return true;
}

static bool network_api_valid_url_text(const char* value) {
    for (const unsigned char* c = (const unsigned char*)value; *c != '\0'; c++) {
        if (*c <= 0x20 || *c == 0x7f) { return false; }
    }
    return true;
}

static void network_api_free_request(struct NetworkHttpRequest* request) {
    if (request == NULL) { return; }
    free(request->url);
    free(request->method);
    free(request->headers);
    free(request->body);
    free(request->responseHeaders);
    free(request->responseBody);
    free(request);
}

static struct Mod* network_api_find_active_mod(const char* relativePath) {
    for (u16 i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];
        if (mod != NULL && strcmp(mod->relativePath, relativePath) == 0) {
            return mod;
        }
    }
    return NULL;
}

#endif

#ifdef _WIN32
static wchar_t* network_api_utf8_to_wide(const char* value) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (length <= 0) { return NULL; }
    wchar_t* result = calloc((size_t)length, sizeof(wchar_t));
    if (result == NULL) { return NULL; }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length) <= 0) {
        free(result);
        return NULL;
    }
    return result;
}

static char* network_api_wide_to_utf8(const wchar_t* value) {
    int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (length <= 0) { return NULL; }
    char* result = calloc((size_t)length, 1);
    if (result == NULL) { return NULL; }
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, result, length, NULL, NULL) <= 0) {
        free(result);
        return NULL;
    }
    return result;
}

static void network_api_set_error(struct NetworkHttpRequest* request, const char* message) {
    snprintf(request->error, sizeof(request->error), "%s", message);
}

static void* network_api_http_worker(void* argument) {
    struct NetworkHttpRequest* request = argument;
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET httpRequest = NULL;
    wchar_t* wideUrl = NULL;
    wchar_t* wideMethod = NULL;
    wchar_t* wideHeaders = NULL;
    wchar_t* host = NULL;
    wchar_t* path = NULL;

    wideUrl = network_api_utf8_to_wide(request->url);
    wideMethod = network_api_utf8_to_wide(request->method);
    if (request->headers != NULL && request->headers[0] != '\0') {
        wideHeaders = network_api_utf8_to_wide(request->headers);
    }
    if (wideUrl == NULL || wideMethod == NULL ||
        (request->headers != NULL && request->headers[0] != '\0' && wideHeaders == NULL)) {
        network_api_set_error(request, "Invalid UTF-8 in request");
        goto cleanup;
    }

    URL_COMPONENTS components;
    memset(&components, 0, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = (DWORD)-1;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wideUrl, 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS) {
        network_api_set_error(request, "Invalid HTTPS URL");
        goto cleanup;
    }

    host = calloc((size_t)components.dwHostNameLength + 1, sizeof(wchar_t));
    path = calloc((size_t)components.dwUrlPathLength + (size_t)components.dwExtraInfoLength + 2, sizeof(wchar_t));
    if (host == NULL || path == NULL) {
        network_api_set_error(request, "Out of memory");
        goto cleanup;
    }
    wcsncpy(host, components.lpszHostName, components.dwHostNameLength);
    if (components.dwUrlPathLength > 0) {
        wcsncat(path, components.lpszUrlPath, components.dwUrlPathLength);
    } else {
        wcscat(path, L"/");
    }
    if (components.dwExtraInfoLength > 0) {
        wcsncat(path, components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    session = WinHttpOpen(L"SM64CoopDX Network API/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS,
                          0);
    if (session == NULL) {
        network_api_set_error(request, "Could not initialize WinHTTP");
        goto cleanup;
    }
    WinHttpSetTimeouts(session, 5000, 5000, NETWORK_API_TIMEOUT_MS, NETWORK_API_TIMEOUT_MS);

    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (connection == NULL) {
        network_api_set_error(request, "HTTPS connection failed");
        goto cleanup;
    }

    httpRequest = WinHttpOpenRequest(connection, wideMethod, path, NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (httpRequest == NULL) {
        network_api_set_error(request, "Could not create HTTPS request");
        goto cleanup;
    }

    // Disable redirects entirely so a secure request can never be redirected to plaintext HTTP.
    DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(httpRequest, WINHTTP_OPTION_DISABLE_FEATURE,
                          &disableRedirects, sizeof(disableRedirects))) {
        network_api_set_error(request, "Could not enforce HTTPS-only redirects");
        goto cleanup;
    }

    if (!WinHttpSendRequest(httpRequest,
                            wideHeaders != NULL ? wideHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                            wideHeaders != NULL ? (DWORD)-1L : 0,
                            request->bodyLength > 0 ? request->body : WINHTTP_NO_REQUEST_DATA,
                            (DWORD)request->bodyLength,
                            (DWORD)request->bodyLength,
                            0)) {
        network_api_set_error(request, "HTTPS request could not be sent");
        goto cleanup;
    }
    if (!WinHttpReceiveResponse(httpRequest, NULL)) {
        network_api_set_error(request, "HTTPS response could not be received");
        goto cleanup;
    }

    DWORD statusSize = sizeof(request->statusCode);
    if (!WinHttpQueryHeaders(httpRequest,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &request->statusCode,
                             &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        network_api_set_error(request, "Could not read HTTPS response status");
        goto cleanup;
    }

    DWORD headerSize = 0;
    WinHttpQueryHeaders(httpRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, NULL, &headerSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && headerSize > 0 && headerSize <= 65536) {
        wchar_t* rawHeaders = calloc((size_t)headerSize / sizeof(wchar_t) + 1, sizeof(wchar_t));
        if (rawHeaders != NULL &&
            WinHttpQueryHeaders(httpRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, rawHeaders, &headerSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            request->responseHeaders = network_api_wide_to_utf8(rawHeaders);
        }
        free(rawHeaders);
    }

    DWORD responseStarted = GetTickCount();
    for (;;) {
        if (GetTickCount() - responseStarted > 30000) {
            network_api_set_error(request, "HTTPS response exceeded the 30 second time limit");
            break;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(httpRequest, &available)) {
            network_api_set_error(request, "Failed while reading HTTPS response");
            break;
        }
        if (available == 0) { break; }
        if ((size_t)available > NETWORK_API_MAX_BODY - request->responseBodyLength) {
            network_api_set_error(request, "HTTPS response exceeds the 1 MiB limit");
            break;
        }

        char* expanded = realloc(request->responseBody, request->responseBodyLength + (size_t)available);
        if (expanded == NULL) {
            network_api_set_error(request, "Out of memory while reading HTTPS response");
            break;
        }
        request->responseBody = expanded;

        DWORD readBytes = 0;
        if (!WinHttpReadData(httpRequest,
                             request->responseBody + request->responseBodyLength,
                             available, &readBytes)) {
            network_api_set_error(request, "Failed while reading HTTPS response");
            break;
        }
        request->responseBodyLength += readBytes;
        if (readBytes == 0) { break; }
    }

cleanup:
    if (httpRequest != NULL) { WinHttpCloseHandle(httpRequest); }
    if (connection != NULL) { WinHttpCloseHandle(connection); }
    if (session != NULL) { WinHttpCloseHandle(session); }
    free(wideUrl);
    free(wideMethod);
    free(wideHeaders);
    free(host);
    free(path);

    pthread_mutex_lock(&sNetworkApiMutex);
    request->complete = true;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return NULL;
}
#endif

static int network_api_has_permission(lua_State* L) {
    struct Mod* mod = gLuaActiveMod;
    lua_pushboolean(L, mod != NULL && mod->networkAccessAllowed);
    return 1;
}

static int network_api_is_supported(lua_State* L) {
#ifdef _WIN32
    lua_pushboolean(L, true);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

static int network_api_https_request(lua_State* L) {
    if (lua_gettop(L) != 2 || lua_type(L, 1) != LUA_TTABLE || lua_type(L, 2) != LUA_TFUNCTION) {
        lua_pushnil(L);
        lua_pushstring(L, "https_request expects an options table and a callback");
        return 2;
    }

    struct Mod* owner = gLuaActiveMod;
    if (owner == NULL || !owner->networkAccessAllowed) {
        lua_pushnil(L);
        lua_pushstring(L, "This mod has not been granted network access");
        return 2;
    }

#ifndef _WIN32
    lua_pushnil(L);
    lua_pushstring(L, "HTTPS transport is currently supported on Windows only");
    return 2;
#else
    if (sNetworkApiRequestCount >= NETWORK_API_MAX_REQUESTS) {
        lua_pushnil(L);
        lua_pushstring(L, "Too many active network requests");
        return 2;
    }

    lua_getfield(L, 1, "url");
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "options.url must be a string");
        return 2;
    }
    size_t urlLength = 0;
    const char* url = lua_tolstring(L, -1, &urlLength);
    if (urlLength == 0 || urlLength > NETWORK_API_MAX_URL ||
        memchr(url, '\0', urlLength) != NULL || !network_api_valid_url_text(url) ||
        !network_api_has_https_prefix(url)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "Only valid https:// URLs up to 2048 bytes are accepted");
        return 2;
    }

    struct NetworkHttpRequest* request = calloc(1, sizeof(struct NetworkHttpRequest));
    if (request == NULL) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }
    request->url = malloc(urlLength + 1);
    if (request->url == NULL) {
        lua_pop(L, 1);
        network_api_free_request(request);
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }
    memcpy(request->url, url, urlLength);
    request->url[urlLength] = '\0';
    lua_pop(L, 1);

    request->method = strdup("GET");
    lua_getfield(L, 1, "method");
    if (!lua_isnil(L, -1)) {
        size_t methodLength = 0;
        const char* method = lua_tolstring(L, -1, &methodLength);
        if (lua_type(L, -1) != LUA_TSTRING || methodLength == 0 ||
            methodLength > NETWORK_API_MAX_METHOD || memchr(method, '\0', methodLength) != NULL ||
            !network_api_valid_token(method)) {
            lua_pop(L, 1);
            network_api_free_request(request);
            lua_pushnil(L);
            lua_pushstring(L, "options.method must be a short, valid string");
            return 2;
        }
        free(request->method);
        request->method = malloc(methodLength + 1);
        if (request->method == NULL) {
            lua_pop(L, 1);
            network_api_free_request(request);
            lua_pushnil(L);
            lua_pushstring(L, "Out of memory");
            return 2;
        }
        memcpy(request->method, method, methodLength);
        request->method[methodLength] = '\0';
    }
    lua_pop(L, 1);
    if (request->method == NULL) {
        network_api_free_request(request);
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }

    lua_getfield(L, 1, "body");
    if (!lua_isnil(L, -1)) {
        size_t bodyLength = 0;
        const char* body = lua_tolstring(L, -1, &bodyLength);
        if (lua_type(L, -1) != LUA_TSTRING || bodyLength > NETWORK_API_MAX_BODY) {
            lua_pop(L, 1);
            network_api_free_request(request);
            lua_pushnil(L);
            lua_pushstring(L, "options.body must be a string no larger than 1 MiB");
            return 2;
        }
        if (bodyLength > 0) {
            request->body = malloc(bodyLength);
            if (request->body == NULL) {
                lua_pop(L, 1);
                network_api_free_request(request);
                lua_pushnil(L);
                lua_pushstring(L, "Out of memory");
                return 2;
            }
            memcpy(request->body, body, bodyLength);
        }
        request->bodyLength = bodyLength;
    }
    lua_pop(L, 1);

    request->headers = calloc(NETWORK_API_MAX_HEADERS + 1, 1);
    if (request->headers == NULL) {
        network_api_free_request(request);
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }
    lua_getfield(L, 1, "headers");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TTABLE) {
            lua_pop(L, 1);
            network_api_free_request(request);
            lua_pushnil(L);
            lua_pushstring(L, "options.headers must be a table");
            return 2;
        }
        int headersIndex = lua_gettop(L);
        size_t used = 0;
        lua_pushnil(L);
        while (lua_next(L, headersIndex) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 2);
                lua_pop(L, 1);
                network_api_free_request(request);
                lua_pushnil(L);
                lua_pushstring(L, "Header names and values must be strings");
                return 2;
            }
            size_t nameLength = 0;
            size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            if (!network_api_valid_token(name) ||
                memchr(name, '\0', nameLength) != NULL ||
                memchr(value, '\0', valueLength) != NULL ||
                !network_api_valid_header_value(value)) {
                lua_pop(L, 2);
                lua_pop(L, 1);
                network_api_free_request(request);
                lua_pushnil(L);
                lua_pushstring(L, "Header names and values cannot contain line breaks");
                return 2;
            }
            int written = snprintf(request->headers + used,
                                   NETWORK_API_MAX_HEADERS + 1 - used,
                                   "%s: %s\r\n", name, value);
            if (written < 0 || (size_t)written >= NETWORK_API_MAX_HEADERS + 1 - used) {
                lua_pop(L, 2);
                lua_pop(L, 1);
                network_api_free_request(request);
                lua_pushnil(L);
                lua_pushstring(L, "Request headers exceed the 16 KiB limit");
                return 2;
            }
            used += (size_t)written;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    if (strlen(owner->relativePath) >= sizeof(request->ownerPath)) {
        network_api_free_request(request);
        lua_pushnil(L);
        lua_pushstring(L, "Invalid mod identity");
        return 2;
    }
    snprintf(request->ownerPath, sizeof(request->ownerPath), "%s", owner->relativePath);
    lua_pushvalue(L, 2);
    request->callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
    request->id = sNetworkApiNextId++;

    request->next = sNetworkApiRequests;
    sNetworkApiRequests = request;
    sNetworkApiRequestCount++;

    if (pthread_create(&request->thread, NULL, network_api_http_worker, request) != 0) {
        sNetworkApiRequests = request->next;
        sNetworkApiRequestCount--;
        luaL_unref(L, LUA_REGISTRYINDEX, request->callbackRef);
        network_api_free_request(request);
        lua_pushnil(L);
        lua_pushstring(L, "Could not start HTTPS worker thread");
        return 2;
    }
    request->threadStarted = true;
    lua_pushinteger(L, request->id);
    return 1;
#endif
}

void smlua_network_api_bind(lua_State* L) {
    lua_getglobal(L, "network");
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, network_api_has_permission);
    lua_setfield(L, -2, "has_permission");
    lua_pushcfunction(L, network_api_is_supported);
    lua_setfield(L, -2, "is_supported");
    lua_pushcfunction(L, network_api_https_request);
    lua_setfield(L, -2, "https_request");
    lua_setglobal(L, "network");
}

void smlua_network_api_discard_mod_requests(const char* relativePath) {
#ifdef _WIN32
    if (relativePath == NULL) { return; }
    pthread_mutex_lock(&sNetworkApiMutex);
    for (struct NetworkHttpRequest* request = sNetworkApiRequests; request != NULL; request = request->next) {
        if (strcmp(request->ownerPath, relativePath) == 0) {
            request->cancelled = true;
        }
    }
    pthread_mutex_unlock(&sNetworkApiMutex);
#else
    UNUSED relativePath;
#endif
}

void smlua_network_api_update(lua_State* L) {
#ifdef _WIN32
    for (;;) {
        struct NetworkHttpRequest* completed = NULL;
        pthread_mutex_lock(&sNetworkApiMutex);
        struct NetworkHttpRequest** link = &sNetworkApiRequests;
        while (*link != NULL) {
            if ((*link)->complete) {
                completed = *link;
                *link = completed->next;
                break;
            }
            link = &(*link)->next;
        }
        pthread_mutex_unlock(&sNetworkApiMutex);
        if (completed == NULL) { break; }

        if (completed->threadStarted) { pthread_join(completed->thread, NULL); }
        if (sNetworkApiRequestCount > 0) { sNetworkApiRequestCount--; }
        struct Mod* owner = network_api_find_active_mod(completed->ownerPath);
        if (!completed->cancelled && owner != NULL && owner->networkAccessAllowed && L != NULL) {
            int previousTop = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, completed->callbackRef);
            if (completed->statusCode > 0) {
                lua_pushinteger(L, completed->statusCode);
            } else {
                lua_pushnil(L);
            }
            lua_pushstring(L, completed->responseHeaders != NULL ? completed->responseHeaders : "");
            lua_pushlstring(L,
                            completed->responseBody != NULL ? completed->responseBody : "",
                            completed->responseBodyLength);
            if (completed->error[0] != '\0') {
                lua_pushstring(L, completed->error);
            } else {
                lua_pushnil(L);
            }
            if (smlua_call_hook(L, 4, 0, 0, owner, NULL) != LUA_OK) {
                LOG_LUA("Failed to call network HTTPS completion callback");
            }
            lua_settop(L, previousTop);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, completed->callbackRef);
        network_api_free_request(completed);
    }
#else
    UNUSED L;
#endif
}

void smlua_network_api_shutdown(lua_State* L) {
#ifdef _WIN32
    pthread_mutex_lock(&sNetworkApiMutex);
    struct NetworkHttpRequest* requests = sNetworkApiRequests;
    sNetworkApiRequests = NULL;
    pthread_mutex_unlock(&sNetworkApiMutex);

    while (requests != NULL) {
        struct NetworkHttpRequest* next = requests->next;
        if (requests->threadStarted) { pthread_join(requests->thread, NULL); }
        if (L != NULL && requests->callbackRef >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, requests->callbackRef);
        }
        network_api_free_request(requests);
        requests = next;
    }
#else
    UNUSED L;
#endif
    sNetworkApiRequestCount = 0;
}
