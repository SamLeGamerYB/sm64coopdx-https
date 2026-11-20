#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#elif _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef WINVER
#define WINVER 0x0602
#elif WINVER < 0x0602
#undef WINVER
#define WINVER 0x0602
#endif
#endif

#include "smlua_network_api.h"

#include "smlua.h"
#include "pc/mods/mods.h"

#include <pthread.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <ctype.h>
#ifndef _WIN32
#include <errno.h>
#endif
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef _WIN32
#include <curl/curl.h>
#endif

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
#define NETWORK_WS_MAX_MESSAGE (1024 * 1024)
#define NETWORK_WS_MAX_CONNECTIONS 4

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

enum NetworkWebSocketEventType {
    NETWORK_WS_EVENT_OPEN,
    NETWORK_WS_EVENT_MESSAGE,
    NETWORK_WS_EVENT_CLOSE,
    NETWORK_WS_EVENT_ERROR,
};

struct NetworkWebSocketEvent {
    enum NetworkWebSocketEventType type;
    char* payload;
    size_t payloadLength;
    bool binary;
    struct NetworkWebSocketEvent* next;
};

struct NetworkWebSocketMessage {
    char* payload;
    size_t payloadLength;
    bool binary;
    struct NetworkWebSocketMessage* next;
};

struct NetworkWebSocket {
    pthread_t thread;
    pthread_t receiveThread;
    pthread_t sendThread;
    bool threadStarted;
    bool receiveThreadStarted;
    bool sendThreadStarted;
    bool complete;
    bool connected;
    bool closeRequested;
    bool remoteCloseReceived;
    bool transportFailed;
    bool callbacksSuppressed;
    unsigned int id;
    int callbackRefs[4];
    char ownerPath[SYS_MAX_PATH];
    char* url;
    char* headers;
#ifdef _WIN32
    HINTERNET socket;
#endif
    pthread_cond_t condition;
    struct NetworkWebSocketMessage* sendHead;
    struct NetworkWebSocketMessage* sendTail;
    unsigned int sendCount;
    size_t sendBytes;
    struct NetworkWebSocketEvent* eventHead;
    struct NetworkWebSocketEvent* eventTail;
    unsigned int eventCount;
    size_t eventBytes;
    struct NetworkWebSocket* next;
};

static pthread_mutex_t sNetworkApiMutex = PTHREAD_MUTEX_INITIALIZER;
static struct NetworkHttpRequest* sNetworkApiRequests = NULL;
static unsigned int sNetworkApiNextId = 1;
static unsigned int sNetworkApiRequestCount = 0;
#ifdef _WIN32
static struct NetworkWebSocket* sNetworkWebSockets = NULL;
static unsigned int sNetworkApiWebSocketCount = 0;
static unsigned int sNetworkApiWebSocketNextId = 1;
#else
static struct NetworkWebSocket* sNetworkWebSockets = NULL;
static unsigned int sNetworkApiWebSocketCount = 0;
static unsigned int sNetworkApiWebSocketNextId = 1;
static pthread_once_t sNetworkApiCurlInitOnce = PTHREAD_ONCE_INIT;
static bool sNetworkApiCurlAvailable = false;
static void network_api_curl_global_init(void) {
    sNetworkApiCurlAvailable = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}
#endif

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

#ifndef _WIN32
static bool network_api_url_has_credentials(const char* value) {
    const char* separator = strstr(value, "://");
    if (separator == NULL) { return true; }
    const char* authority = separator + 3;
    const char* end = strpbrk(authority, "/?#");
    if (end == NULL) { end = authority + strlen(authority); }
    return memchr(authority, '@', (size_t)(end - authority)) != NULL;
}
#endif

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

static bool network_api_request_is_cancelled(struct NetworkHttpRequest* request) {
    pthread_mutex_lock(&sNetworkApiMutex);
    bool cancelled = request->cancelled;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return cancelled;
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
    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
        goto cleanup;
    }

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
    components.dwUserNameLength = (DWORD)-1;
    components.dwPasswordLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wideUrl, 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwUserNameLength > 0 || components.dwPasswordLength > 0) {
        network_api_set_error(request, "Invalid HTTPS URL (credentials in URLs are not accepted)");
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

    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
        goto cleanup;
    }

    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (connection == NULL) {
        network_api_set_error(request, "HTTPS connection failed");
        goto cleanup;
    }

    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
        goto cleanup;
    }

    httpRequest = WinHttpOpenRequest(connection, wideMethod, path, NULL,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
    if (httpRequest == NULL) {
        network_api_set_error(request, "Could not create HTTPS request");
        goto cleanup;
    }

    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
        goto cleanup;
    }

    // Prevent downgrade redirects and avoid implicit cookies or ambient authentication.
    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS |
                             WINHTTP_DISABLE_COOKIES |
                             WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(httpRequest, WINHTTP_OPTION_DISABLE_FEATURE,
                          &disabledFeatures, sizeof(disabledFeatures))) {
        network_api_set_error(request, "Could not enforce secure request policy");
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
    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
        goto cleanup;
    }
    if (!WinHttpReceiveResponse(httpRequest, NULL)) {
        network_api_set_error(request, "HTTPS response could not be received");
        goto cleanup;
    }
    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
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
        if (network_api_request_is_cancelled(request)) {
            network_api_set_error(request, "HTTPS request was cancelled");
            break;
        }
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

#else
static void network_api_set_error(struct NetworkHttpRequest* request, const char* message) {
    snprintf(request->error, sizeof(request->error), "%s", message);
}

static bool network_api_request_is_cancelled(struct NetworkHttpRequest* request) {
    pthread_mutex_lock(&sNetworkApiMutex);
    bool cancelled = request->cancelled;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return cancelled;
}

static size_t network_api_curl_write_body(char* data, size_t size, size_t count, void* context) {
    struct NetworkHttpRequest* request = context;
    if (size != 0 && count > SIZE_MAX / size) { return 0; }
    size_t bytes = size * count;
    if (bytes > NETWORK_API_MAX_BODY - request->responseBodyLength) { return 0; }
    char* expanded = realloc(request->responseBody, request->responseBodyLength + bytes);
    if (expanded == NULL && bytes > 0) { return 0; }
    request->responseBody = expanded;
    if (bytes > 0) {
        memcpy(request->responseBody + request->responseBodyLength, data, bytes);
        request->responseBodyLength += bytes;
    }
    return bytes;
}

static size_t network_api_curl_write_headers(char* data, size_t size, size_t count, void* context) {
    struct NetworkHttpRequest* request = context;
    if (size != 0 && count > SIZE_MAX / size) { return 0; }
    size_t bytes = size * count;
    const size_t maxHeaders = 65536;
    size_t current = request->responseHeaders != NULL ? strlen(request->responseHeaders) : 0;
    if (bytes > maxHeaders - current) { return 0; }
    char* expanded = realloc(request->responseHeaders, current + bytes + 1);
    if (expanded == NULL) { return 0; }
    request->responseHeaders = expanded;
    memcpy(request->responseHeaders + current, data, bytes);
    request->responseHeaders[current + bytes] = '\0';
    return bytes;
}

static int network_api_curl_progress(void* context, curl_off_t downloadTotal,
                                     curl_off_t downloadNow, curl_off_t uploadTotal,
                                     curl_off_t uploadNow) {
    UNUSED downloadTotal;
    UNUSED downloadNow;
    UNUSED uploadTotal;
    UNUSED uploadNow;
    return network_api_request_is_cancelled(context) ? 1 : 0;
}

static struct curl_slist* network_api_curl_headers(const char* rawHeaders) {
    struct curl_slist* headers = NULL;
    if (rawHeaders == NULL) { return NULL; }
    const char* line = rawHeaders;
    while (*line != '\0') {
        const char* end = strstr(line, "\r\n");
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        if (length == 0) { break; }
        char* value = malloc(length + 1);
        if (value == NULL) { curl_slist_free_all(headers); return NULL; }
        memcpy(value, line, length);
        value[length] = '\0';
        struct curl_slist* next = curl_slist_append(headers, value);
        free(value);
        if (next == NULL) { curl_slist_free_all(headers); return NULL; }
        headers = next;
        if (end == NULL) { break; }
        line = end + 2;
    }
    return headers;
}

static void* network_api_http_worker(void* argument) {
    struct NetworkHttpRequest* request = argument;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    CURLcode result = CURLE_FAILED_INIT;
    char errorBuffer[CURL_ERROR_SIZE] = {0};

    if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
        goto cleanup;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        network_api_set_error(request, "Could not initialize libcurl");
        goto cleanup;
    }
    headers = network_api_curl_headers(request->headers);
    if (request->headers != NULL && request->headers[0] != '\0' && headers == NULL) {
        network_api_set_error(request, "Could not allocate request headers");
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request->method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, network_api_curl_write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, request);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, network_api_curl_write_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, request);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, network_api_curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    if (headers != NULL) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (request->bodyLength > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->bodyLength);
    }

    result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        if (responseCode > 0 && (unsigned long)responseCode <= UINT_MAX) {
            request->statusCode = (unsigned int)responseCode;
        }
    } else if (network_api_request_is_cancelled(request)) {
        network_api_set_error(request, "HTTPS request was cancelled");
    } else if (result == CURLE_WRITE_ERROR && request->responseBodyLength >= NETWORK_API_MAX_BODY) {
        network_api_set_error(request, "HTTPS response exceeds the 1 MiB limit");
    } else if (errorBuffer[0] != '\0') {
        network_api_set_error(request, errorBuffer);
    } else {
        network_api_set_error(request, curl_easy_strerror(result));
    }

cleanup:
    if (headers != NULL) { curl_slist_free_all(headers); }
    if (curl != NULL) { curl_easy_cleanup(curl); }
    pthread_mutex_lock(&sNetworkApiMutex);
    request->complete = true;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return NULL;
}
#endif



#ifdef _WIN32
typedef HINTERNET (WINAPI *NetworkApiWebSocketCompleteUpgradeFn)(HINTERNET, DWORD_PTR);
typedef DWORD (WINAPI *NetworkApiWebSocketSendFn)(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, PVOID, DWORD);
typedef DWORD (WINAPI *NetworkApiWebSocketReceiveFn)(HINTERNET, PVOID, DWORD, DWORD*, WINHTTP_WEB_SOCKET_BUFFER_TYPE*);
typedef DWORD (WINAPI *NetworkApiWebSocketCloseFn)(HINTERNET, USHORT, PVOID, DWORD);
typedef DWORD (WINAPI *NetworkApiWebSocketShutdownFn)(HINTERNET, USHORT, PVOID, DWORD);
typedef DWORD (WINAPI *NetworkApiWebSocketQueryCloseStatusFn)(HINTERNET, USHORT*, PVOID, DWORD, DWORD*);

static HMODULE sNetworkApiWinHttpModule = NULL;
static NetworkApiWebSocketCompleteUpgradeFn sNetworkApiWebSocketCompleteUpgrade = NULL;
static NetworkApiWebSocketSendFn sNetworkApiWebSocketSend = NULL;
static NetworkApiWebSocketReceiveFn sNetworkApiWebSocketReceive = NULL;
static NetworkApiWebSocketCloseFn sNetworkApiWebSocketClose = NULL;
static NetworkApiWebSocketShutdownFn sNetworkApiWebSocketShutdown = NULL;
static NetworkApiWebSocketQueryCloseStatusFn sNetworkApiWebSocketQueryCloseStatus = NULL;
static bool sNetworkApiWebSocketFunctionsLoaded = false;
static bool sNetworkApiWebSocketFunctionsAvailable = false;

static bool network_api_load_websocket_functions(void) {
    if (sNetworkApiWebSocketFunctionsLoaded) { return sNetworkApiWebSocketFunctionsAvailable; }
    sNetworkApiWebSocketFunctionsLoaded = true;
    sNetworkApiWinHttpModule = GetModuleHandleW(L"winhttp.dll");
    if (sNetworkApiWinHttpModule == NULL) {
        sNetworkApiWinHttpModule = LoadLibraryW(L"winhttp.dll");
    }
    if (sNetworkApiWinHttpModule == NULL) { return false; }

    sNetworkApiWebSocketCompleteUpgrade = (NetworkApiWebSocketCompleteUpgradeFn)GetProcAddress(sNetworkApiWinHttpModule, "WinHttpWebSocketCompleteUpgrade");
    sNetworkApiWebSocketSend = (NetworkApiWebSocketSendFn)GetProcAddress(sNetworkApiWinHttpModule, "WinHttpWebSocketSend");
    sNetworkApiWebSocketReceive = (NetworkApiWebSocketReceiveFn)GetProcAddress(sNetworkApiWinHttpModule, "WinHttpWebSocketReceive");
    sNetworkApiWebSocketClose = (NetworkApiWebSocketCloseFn)GetProcAddress(sNetworkApiWinHttpModule, "WinHttpWebSocketClose");
    sNetworkApiWebSocketShutdown = (NetworkApiWebSocketShutdownFn)GetProcAddress(sNetworkApiWinHttpModule, "WinHttpWebSocketShutdown");
    sNetworkApiWebSocketQueryCloseStatus = (NetworkApiWebSocketQueryCloseStatusFn)GetProcAddress(sNetworkApiWinHttpModule, "WinHttpWebSocketQueryCloseStatus");
    sNetworkApiWebSocketFunctionsAvailable =
        sNetworkApiWebSocketCompleteUpgrade != NULL &&
        sNetworkApiWebSocketSend != NULL &&
        sNetworkApiWebSocketReceive != NULL &&
        sNetworkApiWebSocketClose != NULL &&
        sNetworkApiWebSocketShutdown != NULL &&
        sNetworkApiWebSocketQueryCloseStatus != NULL;
    return sNetworkApiWebSocketFunctionsAvailable;
}

static bool network_api_has_wss_prefix(const char* url) {
    static const char prefix[] = "wss://";
    if (strlen(url) < sizeof(prefix) - 1) { return false; }
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (tolower((unsigned char)url[i]) != prefix[i]) { return false; }
    }
    return true;
}

static bool network_api_ws_reserved_header(const char* name) {
    static const char* reserved[] = {
        "host", "connection", "upgrade", "content-length", "transfer-encoding",
        "sec-websocket-key", "sec-websocket-version", "sec-websocket-extensions"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        const char* a = name;
        const char* b = reserved[i];
        while (*a != '\0' && *b != '\0' &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') { return true; }
    }
    return false;
}

static void network_api_ws_queue_event(struct NetworkWebSocket* ws,
                                       enum NetworkWebSocketEventType type,
                                       const char* payload, size_t payloadLength,
                                       bool binary) {
    struct NetworkWebSocketEvent* event = calloc(1, sizeof(*event));
    if (event == NULL) { return; }
    if (payloadLength > 0) {
        event->payload = malloc(payloadLength);
        if (event->payload == NULL) { free(event); return; }
        memcpy(event->payload, payload, payloadLength);
    }
    event->payloadLength = payloadLength;
    event->type = type;
    event->binary = binary;
    pthread_mutex_lock(&sNetworkApiMutex);
    if (ws->eventCount >= 32 || payloadLength > NETWORK_WS_MAX_MESSAGE - ws->eventBytes) {
        ws->closeRequested = true;
        ws->transportFailed = true;
        pthread_cond_broadcast(&ws->condition);
        pthread_mutex_unlock(&sNetworkApiMutex);
        free(event->payload);
        free(event);
        return;
    }
    if (ws->eventTail == NULL) { ws->eventHead = event; }
    else { ws->eventTail->next = event; }
    ws->eventTail = event;
    ws->eventCount++;
    ws->eventBytes += payloadLength;
    pthread_mutex_unlock(&sNetworkApiMutex);
}

static void network_api_ws_set_error(struct NetworkWebSocket* ws, const char* message) {
    network_api_ws_queue_event(ws, NETWORK_WS_EVENT_ERROR, message, strlen(message), false);
}

static bool network_api_ws_is_closing(struct NetworkWebSocket* ws) {
    pthread_mutex_lock(&sNetworkApiMutex);
    bool closing = ws->closeRequested || ws->callbacksSuppressed;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return closing;
}

static void network_api_ws_free_messages(struct NetworkWebSocket* ws) {
    struct NetworkWebSocketMessage* message = ws->sendHead;
    while (message != NULL) {
        struct NetworkWebSocketMessage* next = message->next;
        free(message->payload);
        free(message);
        message = next;
    }
    ws->sendHead = ws->sendTail = NULL;
}

static void* network_api_ws_send_worker(void* argument) {
    struct NetworkWebSocket* ws = argument;
    for (;;) {
        pthread_mutex_lock(&sNetworkApiMutex);
        while (ws->sendHead == NULL && !ws->closeRequested) {
            pthread_cond_wait(&ws->condition, &sNetworkApiMutex);
        }
        struct NetworkWebSocketMessage* message = ws->sendHead;
        if (message != NULL) {
            ws->sendHead = message->next;
            if (ws->sendHead == NULL) { ws->sendTail = NULL; }
            if (ws->sendCount > 0) { ws->sendCount--; }
            if (ws->sendBytes >= message->payloadLength) { ws->sendBytes -= message->payloadLength; }
        }
        bool closeRequested = ws->closeRequested;
        bool remoteCloseReceived = ws->remoteCloseReceived;
        bool discardPending = ws->callbacksSuppressed || ws->transportFailed || ws->remoteCloseReceived;
        pthread_mutex_unlock(&sNetworkApiMutex);

        if (message != NULL && discardPending) {
            free(message->payload);
            free(message);
            continue;
        }
        if (message != NULL) {
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = message->binary
                ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            DWORD result = sNetworkApiWebSocketSend(ws->socket, type,
                                                message->payload,
                                                (DWORD)message->payloadLength);
            free(message->payload);
            free(message);
            if (result != NO_ERROR) {
                network_api_ws_set_error(ws, "WebSocket send failed");
                pthread_mutex_lock(&sNetworkApiMutex);
                ws->closeRequested = true;
                ws->transportFailed = true;
                pthread_cond_broadcast(&ws->condition);
                pthread_mutex_unlock(&sNetworkApiMutex);
            }
            continue;
        }
        if (closeRequested) {
            // A peer-initiated close is answered by the connection owner after
            // both workers have stopped. Local close only shuts down the send side.
            if (!remoteCloseReceived) {
                sNetworkApiWebSocketShutdown(ws->socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                         NULL, 0);
            }
            break;
        }
    }
    return NULL;
}

static void* network_api_ws_receive_worker(void* argument) {
    struct NetworkWebSocket* ws = argument;
    char chunk[16384];
    char* message = NULL;
    size_t messageLength = 0;
    bool messageBinary = false;
    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;
        DWORD result = sNetworkApiWebSocketReceive(ws->socket, chunk, sizeof(chunk),
                                               &bytesRead, &bufferType);
        if (result != NO_ERROR) {
            pthread_mutex_lock(&sNetworkApiMutex);
            bool expectedClose = ws->closeRequested;
            ws->closeRequested = true;
            if (!expectedClose) { ws->transportFailed = true; }
            pthread_cond_broadcast(&ws->condition);
            pthread_mutex_unlock(&sNetworkApiMutex);
            if (!expectedClose) { network_api_ws_set_error(ws, "WebSocket receive failed"); }
            break;
        }
        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            pthread_mutex_lock(&sNetworkApiMutex);
            ws->remoteCloseReceived = true;
            ws->closeRequested = true;
            pthread_cond_broadcast(&ws->condition);
            pthread_mutex_unlock(&sNetworkApiMutex);
            break;
        }

        if (messageLength == 0) {
            messageBinary = bufferType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
                            bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        }
        if ((size_t)bytesRead > NETWORK_WS_MAX_MESSAGE - messageLength) {
            free(message);
            message = NULL;
            messageLength = 0;
            network_api_ws_set_error(ws, "WebSocket message exceeds the 1 MiB limit");
            pthread_mutex_lock(&sNetworkApiMutex);
            ws->closeRequested = true;
            ws->transportFailed = true;
            pthread_cond_broadcast(&ws->condition);
            pthread_mutex_unlock(&sNetworkApiMutex);
            break;
        }
        if (bytesRead > 0) {
            char* expanded = realloc(message, messageLength + bytesRead);
            if (expanded == NULL) {
                free(message);
                message = NULL;
                messageLength = 0;
                network_api_ws_set_error(ws, "Out of memory receiving WebSocket message");
                pthread_mutex_lock(&sNetworkApiMutex);
                ws->closeRequested = true;
                ws->transportFailed = true;
                pthread_cond_broadcast(&ws->condition);
                pthread_mutex_unlock(&sNetworkApiMutex);
                break;
            }
            message = expanded;
            memcpy(message + messageLength, chunk, bytesRead);
            messageLength += bytesRead;
        }

        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            network_api_ws_queue_event(ws, NETWORK_WS_EVENT_MESSAGE,
                                       message, messageLength, messageBinary);
            free(message);
            message = NULL;
            messageLength = 0;
        }
    }
    free(message);
    return NULL;
}

static void* network_api_ws_worker(void* argument) {
    struct NetworkWebSocket* ws = argument;
    HINTERNET session = NULL;
    HINTERNET connection = NULL;
    HINTERNET request = NULL;
    HINTERNET socket = NULL;
    wchar_t* wideUrl = NULL;
    wchar_t* wideHeaders = NULL;
    wchar_t* host = NULL;
    wchar_t* path = NULL;
    char* httpsUrl = malloc(strlen(ws->url) + 3);

    if (httpsUrl != NULL) {
        snprintf(httpsUrl, strlen(ws->url) + 3, "https://%s", ws->url + 6);
        wideUrl = network_api_utf8_to_wide(httpsUrl);
    }
    free(httpsUrl);
    if (network_api_ws_is_closing(ws)) { goto cleanup; }
    if (wideUrl == NULL) {
        network_api_ws_set_error(ws, "Invalid UTF-8 in WebSocket URL");
        goto cleanup;
    }
    if (ws->headers != NULL && ws->headers[0] != '\0') {
        wideHeaders = network_api_utf8_to_wide(ws->headers);
        if (wideHeaders == NULL) {
            network_api_ws_set_error(ws, "Invalid UTF-8 in WebSocket headers");
            goto cleanup;
        }
    }

    URL_COMPONENTS components;
    memset(&components, 0, sizeof(components));
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = (DWORD)-1;
    components.dwHostNameLength = (DWORD)-1;
    components.dwUserNameLength = (DWORD)-1;
    components.dwPasswordLength = (DWORD)-1;
    components.dwUrlPathLength = (DWORD)-1;
    components.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wideUrl, 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwUserNameLength > 0 || components.dwPasswordLength > 0) {
        network_api_ws_set_error(ws, "Invalid WSS URL");
        goto cleanup;
    }

    host = calloc((size_t)components.dwHostNameLength + 1, sizeof(wchar_t));
    path = calloc((size_t)components.dwUrlPathLength + (size_t)components.dwExtraInfoLength + 2, sizeof(wchar_t));
    if (host == NULL || path == NULL) {
        network_api_ws_set_error(ws, "Out of memory");
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
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        network_api_ws_set_error(ws, "Could not initialize WinHTTP");
        goto cleanup;
    }
    if (!WinHttpSetTimeouts(session, 10000, 5000, NETWORK_API_TIMEOUT_MS, 60000)) {
        network_api_ws_set_error(ws, "Could not configure WebSocket timeouts");
        goto cleanup;
    }
    connection = WinHttpConnect(session, host, components.nPort, 0);
    if (network_api_ws_is_closing(ws)) { goto cleanup; }
    if (connection == NULL) {
        network_api_ws_set_error(ws, "WebSocket connection failed");
        goto cleanup;
    }
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (request == NULL) {
        network_api_ws_set_error(ws, "Could not create WSS handshake");
        goto cleanup;
    }
    if (wideHeaders != NULL &&
        !WinHttpAddRequestHeaders(request, wideHeaders, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) {
        network_api_ws_set_error(ws, "Could not add WebSocket request headers");
        goto cleanup;
    }
    DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS |
                             WINHTTP_DISABLE_COOKIES |
                             WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                          &disabledFeatures, sizeof(disabledFeatures))) {
        network_api_ws_set_error(ws, "Could not enforce secure WebSocket policy");
        goto cleanup;
    }
    if (network_api_ws_is_closing(ws)) { goto cleanup; }
    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        network_api_ws_set_error(ws, "WSS handshake failed");
        goto cleanup;
    }
    socket = sNetworkApiWebSocketCompleteUpgrade(request, 0);
    if (socket == NULL) {
        network_api_ws_set_error(ws, "Server did not accept the WebSocket upgrade");
        goto cleanup;
    }
    WinHttpCloseHandle(request);
    request = NULL;

    DWORD closeTimeout = 5000;
    DWORD keepAliveInterval = 15000;
    WinHttpSetOption(socket, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
                     &closeTimeout, sizeof(closeTimeout));
    WinHttpSetOption(socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
                     &keepAliveInterval, sizeof(keepAliveInterval));

    pthread_mutex_lock(&sNetworkApiMutex);
    ws->socket = socket;
    ws->connected = true;
    bool closeImmediately = ws->closeRequested;
    pthread_mutex_unlock(&sNetworkApiMutex);
    if (!closeImmediately) {
        network_api_ws_queue_event(ws, NETWORK_WS_EVENT_OPEN, NULL, 0, false);
    }

    if (pthread_create(&ws->sendThread, NULL, network_api_ws_send_worker, ws) == 0) {
        ws->sendThreadStarted = true;
    } else {
        network_api_ws_set_error(ws, "Could not start WebSocket sender");
        pthread_mutex_lock(&sNetworkApiMutex);
        ws->closeRequested = true;
        pthread_cond_broadcast(&ws->condition);
        pthread_mutex_unlock(&sNetworkApiMutex);
    }
    if (ws->sendThreadStarted && pthread_create(&ws->receiveThread, NULL, network_api_ws_receive_worker, ws) == 0) {
        ws->receiveThreadStarted = true;
    } else if (ws->sendThreadStarted) {
        network_api_ws_set_error(ws, "Could not start WebSocket receiver");
        pthread_mutex_lock(&sNetworkApiMutex);
        ws->closeRequested = true;
        pthread_cond_broadcast(&ws->condition);
        pthread_mutex_unlock(&sNetworkApiMutex);
    }
    if (closeImmediately) {
        pthread_mutex_lock(&sNetworkApiMutex);
        ws->closeRequested = true;
        pthread_cond_broadcast(&ws->condition);
        pthread_mutex_unlock(&sNetworkApiMutex);
    }
    if (ws->sendThreadStarted) { pthread_join(ws->sendThread, NULL); }
    if (ws->receiveThreadStarted) { pthread_join(ws->receiveThread, NULL); }
    // Both send and receive workers have stopped, so it is now safe to finish
    // the close handshake and release the WebSocket handle.
    USHORT peerCloseStatus = WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS;
    BYTE peerCloseReason[123];
    DWORD peerCloseReasonLength = 0;
    if (sNetworkApiWebSocketQueryCloseStatus(socket, &peerCloseStatus,
                                         peerCloseReason, sizeof(peerCloseReason),
                                         &peerCloseReasonLength) == NO_ERROR) {
        sNetworkApiWebSocketClose(socket, peerCloseStatus,
                              peerCloseReason, peerCloseReasonLength);
    } else {
        sNetworkApiWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
    }
    network_api_ws_free_messages(ws);
    network_api_ws_queue_event(ws, NETWORK_WS_EVENT_CLOSE, NULL, 0, false);

cleanup:
    if (socket != NULL) { WinHttpCloseHandle(socket); }
    if (request != NULL) { WinHttpCloseHandle(request); }
    if (connection != NULL) { WinHttpCloseHandle(connection); }
    if (session != NULL) { WinHttpCloseHandle(session); }
    free(wideUrl);
    free(wideHeaders);
    free(host);
    free(path);
    pthread_mutex_lock(&sNetworkApiMutex);
    ws->socket = NULL;
    ws->complete = true;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return NULL;
}

static void network_api_ws_free(struct NetworkWebSocket* ws) {
    if (ws == NULL) { return; }
    network_api_ws_free_messages(ws);
    struct NetworkWebSocketEvent* event = ws->eventHead;
    while (event != NULL) {
        struct NetworkWebSocketEvent* next = event->next;
        free(event->payload);
        free(event);
        event = next;
    }
    free(ws->url);
    free(ws->headers);

    pthread_cond_destroy(&ws->condition);
    free(ws);
}
#else

#if LIBCURL_VERSION_NUM >= 0x075600
static bool network_api_load_websocket_functions(void) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!sNetworkApiCurlAvailable || info == NULL || info->protocols == NULL) { return false; }
    for (const char* const* protocol = info->protocols; *protocol != NULL; protocol++) {
        if (strcmp(*protocol, "wss") == 0) { return true; }
    }
    return false;
}
#else
static bool network_api_load_websocket_functions(void) {
    return false;
}
#endif

static bool network_api_has_wss_prefix(const char* url) {
    static const char prefix[] = "wss://";
    if (strlen(url) < sizeof(prefix) - 1) { return false; }
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (tolower((unsigned char)url[i]) != prefix[i]) { return false; }
    }
    return true;
}

static bool network_api_ws_reserved_header(const char* name) {
    static const char* reserved[] = {
        "host", "connection", "upgrade", "content-length", "transfer-encoding",
        "sec-websocket-key", "sec-websocket-version", "sec-websocket-extensions"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        const char* a = name;
        const char* b = reserved[i];
        while (*a != '\0' && *b != '\0' &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') { return true; }
    }
    return false;
}

static void network_api_ws_queue_event(struct NetworkWebSocket* ws,
                                       enum NetworkWebSocketEventType type,
                                       const char* payload, size_t payloadLength,
                                       bool binary) {
    struct NetworkWebSocketEvent* event = calloc(1, sizeof(*event));
    if (event == NULL) { return; }
    if (payloadLength > 0) {
        event->payload = malloc(payloadLength);
        if (event->payload == NULL) { free(event); return; }
        memcpy(event->payload, payload, payloadLength);
    }
    event->payloadLength = payloadLength;
    event->type = type;
    event->binary = binary;
    pthread_mutex_lock(&sNetworkApiMutex);
    if (ws->eventCount >= 32 || payloadLength > NETWORK_WS_MAX_MESSAGE - ws->eventBytes) {
        ws->closeRequested = true;
        ws->transportFailed = true;
        pthread_cond_broadcast(&ws->condition);
        pthread_mutex_unlock(&sNetworkApiMutex);
        free(event->payload);
        free(event);
        return;
    }
    if (ws->eventTail == NULL) { ws->eventHead = event; }
    else { ws->eventTail->next = event; }
    ws->eventTail = event;
    ws->eventCount++;
    ws->eventBytes += payloadLength;
    pthread_mutex_unlock(&sNetworkApiMutex);
}

static void network_api_ws_set_error(struct NetworkWebSocket* ws, const char* message) {
    network_api_ws_queue_event(ws, NETWORK_WS_EVENT_ERROR, message, strlen(message), false);
}

static bool network_api_ws_is_closing(struct NetworkWebSocket* ws) {
    pthread_mutex_lock(&sNetworkApiMutex);
    bool closing = ws->closeRequested || ws->callbacksSuppressed;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return closing;
}

static void network_api_ws_free_messages(struct NetworkWebSocket* ws) {
    struct NetworkWebSocketMessage* message = ws->sendHead;
    while (message != NULL) {
        struct NetworkWebSocketMessage* next = message->next;
        free(message->payload);
        free(message);
        message = next;
    }
    ws->sendHead = ws->sendTail = NULL;
}

#if LIBCURL_VERSION_NUM >= 0x075600
static bool network_api_curl_wait(curl_socket_t socket, short events, int timeoutMs) {
    struct pollfd descriptor;
    descriptor.fd = socket;
    descriptor.events = events;
    descriptor.revents = 0;
    int result;
    do {
        result = poll(&descriptor, 1, timeoutMs);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
}

static bool network_api_curl_ws_send_message(CURL* curl, curl_socket_t socket,
                                             struct NetworkWebSocketMessage* message) {
    const size_t maxFrame = 64 * 1024;
    size_t offset = 0;
    bool emptyPending = message->payloadLength == 0;
    do {
        size_t frameLength = message->payloadLength - offset;
        if (frameLength > maxFrame) { frameLength = maxFrame; }
        unsigned int flags = message->binary ? CURLWS_BINARY : CURLWS_TEXT;
        if (offset + frameLength < message->payloadLength) { flags |= CURLWS_CONT; }
        size_t frameOffset = 0;
        do {
            size_t sent = 0;
            const char* data = message->payload != NULL ? message->payload + offset + frameOffset : "";
            CURLcode result = curl_ws_send(curl, data + frameOffset, frameLength - frameOffset,
                                           &sent, 0, flags);
            if (result == CURLE_AGAIN) {
                if (!network_api_curl_wait(socket, POLLOUT, 1000)) { continue; }
                continue;
            }
            if (result != CURLE_OK) { return false; }
            frameOffset += sent;
            if (sent == 0 && frameOffset < frameLength) {
                network_api_curl_wait(socket, POLLOUT, 1000);
            }
        } while (frameOffset < frameLength);
        offset += frameLength;
        emptyPending = false;
    } while (offset < message->payloadLength || emptyPending);
    return true;
}

static bool network_api_curl_ws_send_close(CURL* curl, curl_socket_t socket) {
    const unsigned char closeStatus[2] = { 0x03, 0xE8 };
    size_t sent = 0;
    CURLcode result;
    do {
        result = curl_ws_send(curl, closeStatus, sizeof(closeStatus), &sent, 0, CURLWS_CLOSE);
        if (result == CURLE_AGAIN) { network_api_curl_wait(socket, POLLOUT, 100); }
    } while (result == CURLE_AGAIN);
    return result == CURLE_OK;
}

static void network_api_ws_fail(struct NetworkWebSocket* ws, const char* message) {
    network_api_ws_set_error(ws, message);
    pthread_mutex_lock(&sNetworkApiMutex);
    ws->transportFailed = true;
    ws->closeRequested = true;
    pthread_cond_broadcast(&ws->condition);
    pthread_mutex_unlock(&sNetworkApiMutex);
}

static void* network_api_ws_worker(void* argument) {
    struct NetworkWebSocket* ws = argument;
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    curl_socket_t socket = CURL_SOCKET_BAD;
    char errorBuffer[CURL_ERROR_SIZE] = {0};
    char receiveBuffer[16384];
    char* messageBuffer = NULL;
    size_t messageLength = 0;
    bool messageBinary = false;
    bool connected = false;
    bool remoteClosed = false;

    if (network_api_ws_is_closing(ws)) { goto cleanup; }
    curl = curl_easy_init();
    if (curl == NULL) {
        network_api_ws_set_error(ws, "Could not initialize libcurl WebSocket");
        goto cleanup;
    }
    headers = network_api_curl_headers(ws->headers);
    if (ws->headers != NULL && ws->headers[0] != '\0' && headers == NULL) {
        network_api_ws_set_error(ws, "Could not allocate WebSocket headers");
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ws->url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_WSS);
#endif

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        network_api_ws_set_error(ws, errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result));
        goto cleanup;
    }
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD) {
        network_api_ws_set_error(ws, "Could not obtain WebSocket network socket");
        goto cleanup;
    }

    pthread_mutex_lock(&sNetworkApiMutex);
    ws->connected = true;
    bool closeImmediately = ws->closeRequested || ws->callbacksSuppressed;
    pthread_mutex_unlock(&sNetworkApiMutex);
    connected = true;
    if (!closeImmediately) { network_api_ws_queue_event(ws, NETWORK_WS_EVENT_OPEN, NULL, 0, false); }

    while (!network_api_ws_is_closing(ws)) {
        struct NetworkWebSocketMessage* outgoing = NULL;
        pthread_mutex_lock(&sNetworkApiMutex);
        outgoing = ws->sendHead;
        if (outgoing != NULL) {
            ws->sendHead = outgoing->next;
            if (ws->sendHead == NULL) { ws->sendTail = NULL; }
            if (ws->sendCount > 0) { ws->sendCount--; }
            if (ws->sendBytes >= outgoing->payloadLength) { ws->sendBytes -= outgoing->payloadLength; }
        }
        pthread_mutex_unlock(&sNetworkApiMutex);

        if (outgoing != NULL) {
            bool sent = network_api_curl_ws_send_message(curl, socket, outgoing);
            free(outgoing->payload);
            free(outgoing);
            if (!sent) {
                network_api_ws_fail(ws, "WebSocket send failed");
                break;
            }
        }

        if (!network_api_curl_wait(socket, POLLIN, 100)) { continue; }
        for (;;) {
            size_t received = 0;
            const struct curl_ws_frame* metadata = NULL;
            result = curl_ws_recv(curl, receiveBuffer, sizeof(receiveBuffer), &received, &metadata);
            if (result == CURLE_AGAIN) { break; }
            if (result == CURLE_GOT_NOTHING) {
                remoteClosed = true;
                break;
            }
            if (result != CURLE_OK || metadata == NULL) {
                network_api_ws_fail(ws, "WebSocket receive failed");
                remoteClosed = true;
                break;
            }
            if (metadata->flags & CURLWS_CLOSE) {
                remoteClosed = true;
                break;
            }
            if (messageLength == 0) {
                messageBinary = (metadata->flags & CURLWS_BINARY) != 0;
            }
            if (received > NETWORK_WS_MAX_MESSAGE - messageLength) {
                network_api_ws_fail(ws, "WebSocket message exceeds the 1 MiB limit");
                remoteClosed = true;
                break;
            }
            if (received > 0) {
                char* expanded = realloc(messageBuffer, messageLength + received);
                if (expanded == NULL) {
                    network_api_ws_fail(ws, "Out of memory receiving WebSocket message");
                    remoteClosed = true;
                    break;
                }
                messageBuffer = expanded;
                memcpy(messageBuffer + messageLength, receiveBuffer, received);
                messageLength += received;
            }
            if (metadata->bytesleft == 0 && !(metadata->flags & CURLWS_CONT)) {
                network_api_ws_queue_event(ws, NETWORK_WS_EVENT_MESSAGE,
                                           messageBuffer, messageLength, messageBinary);
                free(messageBuffer);
                messageBuffer = NULL;
                messageLength = 0;
            }
            if (received == 0 && metadata->bytesleft == 0) { break; }
        }
        if (remoteClosed) { break; }
    }

    if (connected && !remoteClosed) {
        network_api_curl_ws_send_close(curl, socket);
        int waited = 0;
        while (waited < 5000) {
            if (!network_api_curl_wait(socket, POLLIN, 100)) { waited += 100; continue; }
            size_t received = 0;
            const struct curl_ws_frame* metadata = NULL;
            result = curl_ws_recv(curl, receiveBuffer, sizeof(receiveBuffer), &received, &metadata);
            if (result == CURLE_OK && metadata != NULL && (metadata->flags & CURLWS_CLOSE)) {
                remoteClosed = true;
                break;
            }
            if (result != CURLE_AGAIN && result != CURLE_OK) { break; }
            waited += 100;
        }
    } else if (connected && remoteClosed) {
        network_api_curl_ws_send_close(curl, socket);
    }

    network_api_ws_free_messages(ws);
    network_api_ws_queue_event(ws, NETWORK_WS_EVENT_CLOSE, NULL, 0, false);

cleanup:
    free(messageBuffer);
    network_api_ws_free_messages(ws);
    if (headers != NULL) { curl_slist_free_all(headers); }
    if (curl != NULL) { curl_easy_cleanup(curl); }
    pthread_mutex_lock(&sNetworkApiMutex);
    ws->complete = true;
    pthread_mutex_unlock(&sNetworkApiMutex);
    return NULL;
}
#else
static void* network_api_ws_worker(void* argument) {
    UNUSED argument;
    return NULL;
}
#endif

static void network_api_ws_free(struct NetworkWebSocket* ws) {
    if (ws == NULL) { return; }
    network_api_ws_free_messages(ws);
    struct NetworkWebSocketEvent* event = ws->eventHead;
    while (event != NULL) {
        struct NetworkWebSocketEvent* next = event->next;
        free(event->payload);
        free(event);
        event = next;
    }
    free(ws->url);
    free(ws->headers);
    pthread_cond_destroy(&ws->condition);
    free(ws);
}
#endif


static struct NetworkWebSocket* network_api_ws_find_locked(unsigned int id, const char* ownerPath) {
    for (struct NetworkWebSocket* ws = sNetworkWebSockets; ws != NULL; ws = ws->next) {
        if (ws->id == id && strcmp(ws->ownerPath, ownerPath) == 0) { return ws; }
    }
    return NULL;
}

static int network_api_websocket_connect(lua_State* L) {
    if ((lua_gettop(L) != 2 && lua_gettop(L) != 3) ||
        lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TTABLE ||
        (lua_gettop(L) == 3 && !lua_isnil(L, 3) && lua_type(L, 3) != LUA_TTABLE)) {
        lua_pushnil(L);
        lua_pushstring(L, "websocket_connect expects a WSS URL, callbacks table, and optional headers table");
        return 2;
    }
    struct Mod* owner = gLuaActiveMod;
    if (owner == NULL || !owner->networkAccessAllowed) {
        lua_pushnil(L);
        lua_pushstring(L, "This mod has not been granted network access");
        return 2;
    }
    size_t urlLength = 0;
    const char* url = lua_tolstring(L, 1, &urlLength);
    if (urlLength == 0 || urlLength > NETWORK_API_MAX_URL ||
        memchr(url, '\0', urlLength) != NULL || !network_api_valid_url_text(url) ||
        !network_api_has_wss_prefix(url)
#ifndef _WIN32
        || network_api_url_has_credentials(url)
#endif
        ) {
        lua_pushnil(L);
        lua_pushstring(L, "Only valid wss:// URLs up to 2048 bytes are accepted");
        return 2;
    }
    if (!network_api_load_websocket_functions()) {
        lua_pushnil(L);
#ifdef _WIN32
        lua_pushstring(L, "WSS requires Windows 8 or newer with WinHTTP WebSocket support");
#else
        lua_pushstring(L, "WSS requires a libcurl build with WebSocket support (7.86.0 or newer)");
#endif
        return 2;
    }
    if (sNetworkApiWebSocketCount >= NETWORK_WS_MAX_CONNECTIONS) {
        lua_pushnil(L);
        lua_pushstring(L, "Too many active WebSocket connections");
        return 2;
    }
    if (strlen(owner->relativePath) >= SYS_MAX_PATH) {
        lua_pushnil(L);
        lua_pushstring(L, "Invalid mod identity");
        return 2;
    }

    struct NetworkWebSocket* ws = calloc(1, sizeof(*ws));
    if (ws == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }
    for (int i = 0; i < 4; i++) { ws->callbackRefs[i] = LUA_NOREF; }
    if (pthread_cond_init(&ws->condition, NULL) != 0) {
        free(ws);
        lua_pushnil(L);
        lua_pushstring(L, "Could not initialize WebSocket synchronization");
        return 2;
    }
    ws->url = malloc(urlLength + 1);
    if (ws->url == NULL) {
        pthread_cond_destroy(&ws->condition);
        free(ws);
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }
    memcpy(ws->url, url, urlLength);
    ws->url[urlLength] = '\0';
    snprintf(ws->ownerPath, sizeof(ws->ownerPath), "%s", owner->relativePath);

    ws->headers = calloc(NETWORK_API_MAX_HEADERS + 1, 1);
    if (ws->headers == NULL) {
        network_api_ws_free(ws);
        lua_pushnil(L);
        lua_pushstring(L, "Out of memory");
        return 2;
    }
    if (lua_gettop(L) == 3 && lua_istable(L, 3)) {
        size_t used = 0;
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 2);
                network_api_ws_free(ws);
                lua_pushnil(L);
                lua_pushstring(L, "WebSocket header names and values must be strings");
                return 2;
            }
            size_t nameLength = 0;
            size_t valueLength = 0;
            const char* name = lua_tolstring(L, -2, &nameLength);
            const char* value = lua_tolstring(L, -1, &valueLength);
            if (!network_api_valid_token(name) ||
                network_api_ws_reserved_header(name) ||
                memchr(name, '\0', nameLength) != NULL ||
                memchr(value, '\0', valueLength) != NULL ||
                !network_api_valid_header_value(value)) {
                lua_pop(L, 2);
                network_api_ws_free(ws);
                lua_pushnil(L);
                lua_pushstring(L, "Invalid WebSocket header name or value");
                return 2;
            }
            int written = snprintf(ws->headers + used,
                                   NETWORK_API_MAX_HEADERS + 1 - used,
                                   "%s: %s\r\n", name, value);
            if (written < 0 || (size_t)written >= NETWORK_API_MAX_HEADERS + 1 - used) {
                lua_pop(L, 2);
                network_api_ws_free(ws);
                lua_pushnil(L);
                lua_pushstring(L, "WebSocket headers exceed the 16 KiB limit");
                return 2;
            }
            used += (size_t)written;
            lua_pop(L, 1);
        }
    }

    static const char* callbackNames[4] = { "on_open", "on_message", "on_close", "on_error" };
    for (int i = 0; i < 4; i++) {
        lua_getfield(L, 2, callbackNames[i]);
        if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TFUNCTION) {
            lua_pop(L, 1);
            for (int j = 0; j < 4; j++) {
                if (ws->callbackRefs[j] != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, ws->callbackRefs[j]); }
            }
            network_api_ws_free(ws);
            lua_pushnil(L);
            lua_pushfstring(L, "callbacks.%s must be a function or nil", callbackNames[i]);
            return 2;
        }
        if (lua_type(L, -1) == LUA_TFUNCTION) {
            ws->callbackRefs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
    }

    pthread_mutex_lock(&sNetworkApiMutex);
    ws->id = sNetworkApiWebSocketNextId++;
    if (ws->id == 0) { ws->id = sNetworkApiWebSocketNextId++; }
    ws->next = sNetworkWebSockets;
    sNetworkWebSockets = ws;
    sNetworkApiWebSocketCount++;
    pthread_mutex_unlock(&sNetworkApiMutex);

    if (pthread_create(&ws->thread, NULL, network_api_ws_worker, ws) != 0) {
        pthread_mutex_lock(&sNetworkApiMutex);
        sNetworkWebSockets = ws->next;
        if (sNetworkApiWebSocketCount > 0) { sNetworkApiWebSocketCount--; }
        pthread_mutex_unlock(&sNetworkApiMutex);
        for (int i = 0; i < 4; i++) {
            if (ws->callbackRefs[i] != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, ws->callbackRefs[i]); }
        }
        network_api_ws_free(ws);
        lua_pushnil(L);
        lua_pushstring(L, "Could not start WebSocket worker thread");
        return 2;
    }
    ws->threadStarted = true;
    lua_pushinteger(L, ws->id);
    return 1;
}

static int network_api_websocket_send(lua_State* L) {
    if ((lua_gettop(L) != 2 && lua_gettop(L) != 3) ||
        lua_type(L, 1) != LUA_TNUMBER || !lua_isinteger(L, 1) ||
        lua_type(L, 2) != LUA_TSTRING) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_Integer id = lua_tointeger(L, 1);
    struct Mod* owner = gLuaActiveMod;
    if (id <= 0 || id > UINT_MAX || owner == NULL || !owner->networkAccessAllowed) {
        lua_pushboolean(L, false);
        return 1;
    }
    size_t payloadLength = 0;
    const char* payload = lua_tolstring(L, 2, &payloadLength);
    bool binary = false;
    if (lua_gettop(L) == 3) {
        const char* kind = lua_tostring(L, 3);
        if (kind == NULL || (strcmp(kind, "text") != 0 && strcmp(kind, "binary") != 0)) {
            lua_pushboolean(L, false);
            return 1;
        }
        binary = strcmp(kind, "binary") == 0;
    }
    if (payloadLength > NETWORK_WS_MAX_MESSAGE) {
        lua_pushboolean(L, false);
        return 1;
    }
    struct NetworkWebSocketMessage* message = calloc(1, sizeof(*message));
    if (message == NULL) { lua_pushboolean(L, false); return 1; }
    if (payloadLength > 0) {
        message->payload = malloc(payloadLength);
        if (message->payload == NULL) { free(message); lua_pushboolean(L, false); return 1; }
        memcpy(message->payload, payload, payloadLength);
    }
    message->payloadLength = payloadLength;
    message->binary = binary;

    pthread_mutex_lock(&sNetworkApiMutex);
    struct NetworkWebSocket* ws = network_api_ws_find_locked((unsigned int)id, owner->relativePath);
    if (ws == NULL || !ws->connected || ws->closeRequested ||
        ws->sendCount >= 8 || payloadLength > NETWORK_WS_MAX_MESSAGE - ws->sendBytes) {
        pthread_mutex_unlock(&sNetworkApiMutex);
        free(message->payload);
        free(message);
        lua_pushboolean(L, false);
        return 1;
    }
    if (ws->sendTail == NULL) { ws->sendHead = message; }
    else { ws->sendTail->next = message; }
    ws->sendTail = message;
    ws->sendCount++;
    ws->sendBytes += payloadLength;
    pthread_cond_signal(&ws->condition);
    pthread_mutex_unlock(&sNetworkApiMutex);
    lua_pushboolean(L, true);
    return 1;
}

static int network_api_websocket_close(lua_State* L) {
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TNUMBER || !lua_isinteger(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_Integer id = lua_tointeger(L, 1);
    struct Mod* owner = gLuaActiveMod;
    if (id <= 0 || id > UINT_MAX || owner == NULL) {
        lua_pushboolean(L, false);
        return 1;
    }
    pthread_mutex_lock(&sNetworkApiMutex);
    struct NetworkWebSocket* ws = network_api_ws_find_locked((unsigned int)id, owner->relativePath);
    if (ws != NULL) {
        ws->closeRequested = true;
        pthread_cond_broadcast(&ws->condition);
    }
    pthread_mutex_unlock(&sNetworkApiMutex);
    lua_pushboolean(L, ws != NULL);
    return 1;
}

static int network_api_has_permission(lua_State* L) {
    struct Mod* mod = gLuaActiveMod;
    lua_pushboolean(L, mod != NULL && mod->networkAccessAllowed);
    return 1;
}

static int network_api_is_supported(lua_State* L) {
#ifdef _WIN32
    lua_pushboolean(L, true);
#else
    lua_pushboolean(L, sNetworkApiCurlAvailable);
#endif
    return 1;
}

static int network_api_websocket_is_supported(lua_State* L) {
    lua_pushboolean(L, network_api_load_websocket_functions());
    return 1;
}

static int network_api_cancel_request(lua_State* L) {
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TNUMBER || !lua_isinteger(L, 1)) {
        lua_pushboolean(L, false);
        return 1;
    }

    lua_Integer requestId = lua_tointeger(L, 1);
    struct Mod* owner = gLuaActiveMod;
    if (requestId <= 0 || requestId > UINT_MAX || owner == NULL) {
        lua_pushboolean(L, false);
        return 1;
    }

    bool found = false;
    pthread_mutex_lock(&sNetworkApiMutex);
    for (struct NetworkHttpRequest* request = sNetworkApiRequests; request != NULL; request = request->next) {
        if (request->id == (unsigned int)requestId && strcmp(request->ownerPath, owner->relativePath) == 0) {
            request->cancelled = true;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&sNetworkApiMutex);
    lua_pushboolean(L, found);
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
    if (!sNetworkApiCurlAvailable) {
        lua_pushnil(L);
        lua_pushstring(L, "The libcurl HTTPS transport could not be initialized");
        return 2;
    }
#endif

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
        !network_api_has_https_prefix(url)
#ifndef _WIN32
        || network_api_url_has_credentials(url)
#endif
        ) {
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
}

void smlua_network_api_bind(lua_State* L) {
#ifndef _WIN32
    pthread_once(&sNetworkApiCurlInitOnce, network_api_curl_global_init);
#endif
    lua_getglobal(L, "network");
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, network_api_has_permission);
    lua_setfield(L, -2, "has_permission");
    lua_pushcfunction(L, network_api_is_supported);
    lua_setfield(L, -2, "is_supported");
    lua_pushcfunction(L, network_api_websocket_is_supported);
    lua_setfield(L, -2, "websocket_is_supported");
    lua_pushcfunction(L, network_api_cancel_request);
    lua_setfield(L, -2, "cancel_request");
    lua_pushcfunction(L, network_api_https_request);
    lua_setfield(L, -2, "https_request");
    lua_pushcfunction(L, network_api_websocket_connect);
    lua_setfield(L, -2, "websocket_connect");
    lua_pushcfunction(L, network_api_websocket_send);
    lua_setfield(L, -2, "websocket_send");
    lua_pushcfunction(L, network_api_websocket_close);
    lua_setfield(L, -2, "websocket_close");
    lua_setglobal(L, "network");
}

void smlua_network_api_discard_mod_requests(const char* relativePath) {
    if (relativePath == NULL) { return; }
    pthread_mutex_lock(&sNetworkApiMutex);
    for (struct NetworkHttpRequest* request = sNetworkApiRequests; request != NULL; request = request->next) {
        if (strcmp(request->ownerPath, relativePath) == 0) {
            request->cancelled = true;
        }
    }
    for (struct NetworkWebSocket* ws = sNetworkWebSockets; ws != NULL; ws = ws->next) {
        if (strcmp(ws->ownerPath, relativePath) == 0) {
            ws->callbacksSuppressed = true;
            ws->closeRequested = true;
            pthread_cond_broadcast(&ws->condition);
        }
    }
    pthread_mutex_unlock(&sNetworkApiMutex);
}


static void network_api_ws_update(lua_State* L) {
    unsigned int processed = 0;
    while (processed < 32) {
        struct NetworkWebSocket* ws = NULL;
        struct NetworkWebSocketEvent* event = NULL;
        bool finished = false;
        bool callbacksSuppressed = false;

        pthread_mutex_lock(&sNetworkApiMutex);
        struct NetworkWebSocket** link = &sNetworkWebSockets;
        while (*link != NULL) {
            if ((*link)->eventHead != NULL) {
                ws = *link;
                event = ws->eventHead;
                ws->eventHead = event->next;
                if (ws->eventHead == NULL) { ws->eventTail = NULL; }
                if (ws->eventCount > 0) { ws->eventCount--; }
                if (ws->eventBytes >= event->payloadLength) { ws->eventBytes -= event->payloadLength; }
                callbacksSuppressed = ws->callbacksSuppressed;
                break;
            }
            if ((*link)->complete) {
                ws = *link;
                *link = ws->next;
                ws->next = NULL;
                finished = true;
                if (sNetworkApiWebSocketCount > 0) { sNetworkApiWebSocketCount--; }
                break;
            }
            link = &(*link)->next;
        }
        pthread_mutex_unlock(&sNetworkApiMutex);

        if (ws == NULL) { break; }
        if (finished) {
            if (ws->threadStarted) { pthread_join(ws->thread, NULL); }
            for (int i = 0; i < 4; i++) {
                if (ws->callbackRefs[i] != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, ws->callbackRefs[i]);
                }
            }
            network_api_ws_free(ws);
            continue;
        }

        processed++;
        if (!callbacksSuppressed && L != NULL) {
            struct Mod* owner = network_api_find_active_mod(ws->ownerPath);
            if (owner != NULL && owner->networkAccessAllowed) {
                int callbackIndex = (int)event->type;
                int callbackRef = ws->callbackRefs[callbackIndex];
                if (callbackRef != LUA_NOREF) {
                    int previousTop = lua_gettop(L);
                    lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);
                    lua_pushinteger(L, ws->id);
                    int argumentCount = 1;
                    if (event->type == NETWORK_WS_EVENT_MESSAGE) {
                        lua_pushlstring(L, event->payload != NULL ? event->payload : "",
                                        event->payloadLength);
                        lua_pushboolean(L, event->binary);
                        argumentCount = 3;
                    } else if (event->type == NETWORK_WS_EVENT_ERROR) {
                        lua_pushlstring(L, event->payload != NULL ? event->payload : "",
                                        event->payloadLength);
                        argumentCount = 2;
                    }
                    if (smlua_call_hook(L, argumentCount, 0, 0, owner, NULL) != LUA_OK) {
                        LOG_LUA("Failed to call network WebSocket callback");
                    }
                    lua_settop(L, previousTop);
                }
            }
        }
        free(event->payload);
        free(event);
    }
}

void smlua_network_api_update(lua_State* L) {
    unsigned int processed = 0;
    while (processed < NETWORK_API_MAX_REQUESTS) {
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
        processed++;

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
    network_api_ws_update(L);
}

void smlua_network_api_shutdown(lua_State* L) {
    pthread_mutex_lock(&sNetworkApiMutex);
    struct NetworkHttpRequest* requests = sNetworkApiRequests;
    sNetworkApiRequests = NULL;
    for (struct NetworkHttpRequest* request = requests; request != NULL; request = request->next) {
        request->cancelled = true;
    }
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

    pthread_mutex_lock(&sNetworkApiMutex);
    struct NetworkWebSocket* sockets = sNetworkWebSockets;
    sNetworkWebSockets = NULL;
    for (struct NetworkWebSocket* ws = sockets; ws != NULL; ws = ws->next) {
        ws->callbacksSuppressed = true;
        ws->closeRequested = true;
        pthread_cond_broadcast(&ws->condition);
    }
    pthread_mutex_unlock(&sNetworkApiMutex);

    while (sockets != NULL) {
        struct NetworkWebSocket* next = sockets->next;
        if (sockets->threadStarted) { pthread_join(sockets->thread, NULL); }
        if (L != NULL) {
            for (int i = 0; i < 4; i++) {
                if (sockets->callbackRefs[i] != LUA_NOREF) {
                    luaL_unref(L, LUA_REGISTRYINDEX, sockets->callbackRefs[i]);
                }
            }
        }
        network_api_ws_free(sockets);
        sockets = next;
    }
    sNetworkApiWebSocketCount = 0;
    sNetworkApiRequestCount = 0;
}
