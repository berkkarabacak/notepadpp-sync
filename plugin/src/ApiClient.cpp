// ApiClient.cpp — WinHTTP implementation of the NPSync protocol client.
#include "ApiClient.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

using nlohmann::json;

namespace npsync
{

namespace
{

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

struct ParsedUrl
{
    std::wstring host;
    INTERNET_PORT port = 443;
    bool https = true;
    std::string basePath;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
    std::string u = url;
    while (!u.empty() && u.back() == '/')
        u.pop_back();
    if (u.rfind("https://", 0) == 0) {
        out.https = true;
        u = u.substr(8);
    }
    else if (u.rfind("http://", 0) == 0) {
        out.https = false;
        u = u.substr(7);
    }
    else
        return false;
    size_t slash = u.find('/');
    std::string hostPort = slash == std::string::npos ? u : u.substr(0, slash);
    out.basePath = slash == std::string::npos ? "" : u.substr(slash);
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        out.port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
        hostPort = hostPort.substr(0, colon);
    }
    else {
        out.port = out.https ? 443 : 80;
    }
    out.host = widen(hostPort);
    return !out.host.empty();
}

std::string headersAll(HINTERNET hReq) {
    DWORD size = 0;
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return {};
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(),
                             &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string headerValue(const std::string& allHeaders, const std::string& name) {
    std::string needle = "\r\n" + name + ":";
    size_t pos = allHeaders.find(needle);
    if (pos == std::string::npos) {
        if (allHeaders.rfind(name + ":", 0) == 0)
            pos = 0 - name.size() - 1; // first header
        else
            return {};
    }
    size_t start = pos + needle.size();
    size_t end = allHeaders.find("\r\n", start);
    std::string v = allHeaders.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.erase(v.begin());
    return v;
}

} // namespace

ApiClient::ApiClient(std::string baseUrl, std::string deviceId)
    : baseUrl_(std::move(baseUrl)), deviceId_(std::move(deviceId)) {}

void ApiClient::setTokens(const std::string& access, const std::string& refresh) {
    accessToken_ = access;
    refreshToken_ = refresh;
}

void ApiClient::clearTokens() {
    accessToken_.clear();
    refreshToken_.clear();
}

bool ApiClient::trackSession(void* session) {
    std::lock_guard<std::mutex> lk(inflightMu_);
    if (cancel_ || !session)
        return false;
    OpenHandles h;
    h.session = session;
    inflight_.push_back(h);
    return true;
}

bool ApiClient::trackConnect(void* session, void* connect) {
    std::lock_guard<std::mutex> lk(inflightMu_);
    for (auto& h : inflight_) {
        if (h.session == session) {
            h.connect = connect;
            return true;
        }
    }
    return false;
}

bool ApiClient::trackRequest(void* session, void* request) {
    std::lock_guard<std::mutex> lk(inflightMu_);
    for (auto& h : inflight_) {
        if (h.session == session) {
            h.request = request;
            return true;
        }
    }
    return false;
}

bool ApiClient::releaseSession(void* session) {
    OpenHandles owned{};
    {
        std::lock_guard<std::mutex> lk(inflightMu_);
        for (auto it = inflight_.begin(); it != inflight_.end(); ++it) {
            if (it->session == session) {
                owned = *it;
                inflight_.erase(it);
                break;
            }
        }
    }
    // Close children first. If cancelRequests already took this entry, the
    // handles are invalid and must not be closed again.
    if (!owned.session && !owned.connect && !owned.request)
        return false;
    if (owned.request)
        WinHttpCloseHandle(static_cast<HINTERNET>(owned.request));
    if (owned.connect)
        WinHttpCloseHandle(static_cast<HINTERNET>(owned.connect));
    if (owned.session)
        WinHttpCloseHandle(static_cast<HINTERNET>(owned.session));
    return true;
}

void ApiClient::cancelRequests() {
    std::vector<OpenHandles> doomed;
    {
        std::lock_guard<std::mutex> lk(inflightMu_);
        cancel_ = true;
        doomed.swap(inflight_);
    }
    // Do not hold inflightMu_ here. Closing the handle unblocks request(),
    // which takes the mutex to drop its copy.
    for (auto& h : doomed) {
        if (h.request)
            WinHttpCloseHandle(static_cast<HINTERNET>(h.request));
        if (h.connect)
            WinHttpCloseHandle(static_cast<HINTERNET>(h.connect));
        if (h.session)
            WinHttpCloseHandle(static_cast<HINTERNET>(h.session));
    }
}

ApiResponse ApiClient::request(const std::string& method, const std::string& path, const json* body,
                               bool authed, bool retryOn401) {
    ApiResponse resp;
    ParsedUrl pu;
    if (!parseUrl(baseUrl_, pu)) {
        resp.transportError = "bad base url";
        return resp;
    }
    {
        std::lock_guard<std::mutex> lk(inflightMu_);
        if (cancel_) {
            resp.transportError = "cancelled";
            return resp;
        }
    }

    HINTERNET hSession = WinHttpOpen(L"NPSync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        resp.transportError = "WinHttpOpen failed";
        return resp;
    }
    // Bounded timeouts: connect 10s, send 30s, receive 120s (large downloads).
    // Shutdown does not wait this out; cancelRequests closes the handles.
    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 120000);
    if (!trackSession(hSession)) {
        WinHttpCloseHandle(hSession);
        resp.transportError = "cancelled";
        return resp;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, pu.host.c_str(), pu.port, 0);
    if (!hConnect) {
        releaseSession(hSession);
        resp.transportError = "WinHttpConnect failed";
        return resp;
    }
    if (!trackConnect(hSession, hConnect)) {
        // Shutdown already closed the session, which closes this connect.
        resp.transportError = "cancelled";
        return resp;
    }

    std::string fullPath = pu.basePath + path;
    std::wstring wPath = widen(fullPath);
    HINTERNET hReq =
        WinHttpOpenRequest(hConnect, widen(method).c_str(), wPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, pu.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        releaseSession(hSession);
        resp.transportError = "WinHttpOpenRequest failed";
        return resp;
    }
    if (!trackRequest(hSession, hReq)) {
        resp.transportError = "cancelled";
        return resp;
    }

    std::wstring headers = L"Content-Type: application/json\r\nX-NPSync-Protocol: 1\r\n";
    if (authed && !accessToken_.empty())
        headers += L"Authorization: Bearer " + widen(accessToken_) + L"\r\n";

    std::string payload = body ? body->dump() : std::string();
    BOOL sent = WinHttpSendRequest(hReq, headers.c_str(), (DWORD)headers.size(),
                                   (LPVOID)(payload.empty() ? nullptr : payload.data()),
                                   (DWORD)payload.size(), (DWORD)payload.size(), 0);
    if (sent)
        sent = WinHttpReceiveResponse(hReq, nullptr);
    if (!sent) {
        DWORD err = GetLastError();
        releaseSession(hSession);
        resp.transportError = "send/receive failed: " + std::to_string(err);
        return resp;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    resp.status = (long)status;
    resp.transportOk = true;

    std::string all = headersAll(hReq);
    std::string proto = headerValue(all, "X-NPSync-Protocol");
    if (!proto.empty())
        resp.serverProtocol = std::atoi(proto.c_str());

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail))
            break;
        if (avail == 0)
            break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hReq, chunk.data(), avail, &read))
            break;
        resp.rawBody.append(chunk, 0, read);
    }

    if (!releaseSession(hSession)) {
        // Shutdown closed the handles mid-read. A partial body is not a
        // successful response; the upload stays queued for the next launch.
        resp = ApiResponse{};
        resp.transportError = "cancelled";
        return resp;
    }

    if (!resp.rawBody.empty()) {
        try {
            resp.body = json::parse(resp.rawBody);
        }
        catch (...) {
            resp.body = json();
        }
    }

    // Transparent access-token refresh on 401 (single retry).
    if (resp.status == 401 && authed && retryOn401 && !refreshToken_.empty()) {
        if (refreshAccessToken())
            return request(method, path, body, authed, false);
    }
    return resp;
}

bool ApiClient::refreshAccessToken() {
    json body = {{"refresh_token", refreshToken_}};
    ApiResponse r = request("POST", "/auth/refresh", &body, false, false);
    if (r.status != 200)
        return false;
    std::string access = r.body.value("access_token", "");
    std::string refresh = r.body.value("refresh_token", "");
    if (access.empty() || refresh.empty())
        return false;
    accessToken_ = access;
    refreshToken_ = refresh;
    if (onTokensRotated)
        onTokensRotated(access, refresh);
    return true;
}

ApiResponse ApiClient::registerAccount(const std::string& email, const std::string& password,
                                       const std::string& deviceName) {
    json body = {{"email", email}, {"password", password}, {"device_name", deviceName}};
    auto r = request("POST", "/auth/register", &body, false);
    if (r.status == 200)
        setTokens(r.body.value("access_token", ""), r.body.value("refresh_token", ""));
    return r;
}

ApiResponse ApiClient::login(const std::string& email, const std::string& password,
                             const std::string& deviceName) {
    json body = {{"email", email}, {"password", password}, {"device_name", deviceName}};
    auto r = request("POST", "/auth/login", &body, false);
    if (r.status == 200)
        setTokens(r.body.value("access_token", ""), r.body.value("refresh_token", ""));
    return r;
}

ApiResponse ApiClient::startGoogleLogin(const std::string& deviceName) {
    json body = {{"device_name", deviceName}};
    return request("POST", "/auth/google/start", &body, false);
}

ApiResponse ApiClient::pollGoogleLogin(const std::string& state, const std::string& pollSecret) {
    json body = {{"state", state}, {"poll_secret", pollSecret}};
    auto r = request("POST", "/auth/google/poll", &body, false);
    if (r.status == 200)
        setTokens(r.body.value("access_token", ""), r.body.value("refresh_token", ""));
    return r;
}

ApiResponse ApiClient::logout() {
    json empty = json::object();
    auto r = request("POST", "/auth/logout", &empty, true);
    clearTokens();
    return r;
}

ApiResponse ApiClient::listDevices() {
    return request("GET", "/devices", nullptr, true);
}

ApiResponse ApiClient::revokeDevice(const std::string& deviceId) {
    return request("DELETE", "/devices/" + deviceId, nullptr, true);
}

ApiResponse ApiClient::renameDevice(const std::string& deviceId, const std::string& name) {
    json body = {{"name", name}};
    return request("PATCH", "/devices/" + deviceId, &body, true);
}

ApiResponse ApiClient::pairRequest() {
    json body = {{"action", "request"}};
    return request("POST", "/devices/pair", &body, true);
}

ApiResponse ApiClient::pairApprove(const std::string& code, const std::string& wrappedKeyB64) {
    json body = {{"action", "approve"}, {"pairing_code", code}, {"wrapped_master_key", wrappedKeyB64}};
    return request("POST", "/devices/pair", &body, true);
}

ApiResponse ApiClient::pairPoll(const std::string& code) {
    json body = {{"action", "poll"}, {"pairing_code", code}};
    return request("POST", "/devices/pair", &body, true);
}

ApiResponse ApiClient::listFiles() {
    return request("GET", "/sync/files", nullptr, true);
}

ApiResponse ApiClient::getFile(const std::string& fileId) {
    return request("GET", "/sync/files/" + fileId, nullptr, true);
}

ApiResponse ApiClient::createFile(const json& filePayload) {
    return request("POST", "/sync/files", &filePayload, true);
}

ApiResponse ApiClient::updateFile(const std::string& fileId, const json& filePayload) {
    return request("PUT", "/sync/files/" + fileId, &filePayload, true);
}

ApiResponse ApiClient::deleteFile(const std::string& fileId) {
    return request("DELETE", "/sync/files/" + fileId, nullptr, true);
}

ApiResponse ApiClient::listVersions(const std::string& fileId) {
    return request("GET", "/sync/files/" + fileId + "/versions", nullptr, true);
}

ApiResponse ApiClient::restoreVersion(const std::string& fileId, int version) {
    json body = {{"version", version}};
    return request("POST", "/sync/files/" + fileId + "/restore", &body, true);
}

ApiResponse ApiClient::changesSince(int64_t seq) {
    return request("GET", "/sync/changes?since=" + std::to_string(seq), nullptr, true);
}

ApiResponse ApiClient::getSession() {
    return request("GET", "/session", nullptr, true);
}

ApiResponse ApiClient::putSession(const std::string& encryptedStateB64, int version) {
    json body = {{"encrypted_state", encryptedStateB64}, {"version", version}};
    return request("PUT", "/session", &body, true);
}

// ---- WebSocket ----

struct WsClient::Impl
{
    std::atomic<bool> running{false};
    std::thread thread;
    std::mutex mu;
    // Session, connect, request, and websocket handles for the attempt in
    // progress. stop() takes this list and closes it so a blocked WinHTTP
    // call returns. Only one side closes a given handle.
    std::vector<HINTERNET> handles;
};

namespace
{

bool publishWsHandle(WsClient::Impl* impl, HINTERNET h) {
    std::lock_guard<std::mutex> lk(impl->mu);
    if (!impl->running || !h)
        return false;
    impl->handles.push_back(h);
    return true;
}

std::vector<HINTERNET> takeWsHandles(WsClient::Impl* impl) {
    std::lock_guard<std::mutex> lk(impl->mu);
    std::vector<HINTERNET> mine;
    mine.swap(impl->handles);
    return mine;
}

void closeWsHandles(std::vector<HINTERNET> handles) {
    // Reverse order: websocket, then request, connect, session.
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        if (*it)
            WinHttpCloseHandle(*it);
    }
}

// Drop one published handle and close it if stop() has not taken it.
void closePublishedWsHandle(WsClient::Impl* impl, HINTERNET h) {
    bool own = false;
    {
        std::lock_guard<std::mutex> lk(impl->mu);
        auto it = std::find(impl->handles.begin(), impl->handles.end(), h);
        if (it != impl->handles.end()) {
            impl->handles.erase(it);
            own = true;
        }
    }
    if (own)
        WinHttpCloseHandle(h);
}

} // namespace

WsClient::WsClient(std::string baseUrl, EventCallback onEvent, StateCallback onState)
    : baseUrl_(std::move(baseUrl)), onEvent_(std::move(onEvent)), onState_(std::move(onState)) {}

WsClient::~WsClient() {
    stop();
}

void WsClient::start(std::function<std::string()> accessTokenProvider) {
    stop();
    tokenProvider_ = std::move(accessTokenProvider);
    if (!impl_)
        impl_ = new Impl();
    impl_->running = true;
    impl_->thread = std::thread([this] { run(); });
}

void WsClient::stop() {
    if (!impl_)
        return;
    std::vector<HINTERNET> handles;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->running = false;
        handles.swap(impl_->handles);
    }
    // Close outside the mutex. WinHttpWebSocketReceive returns only after the
    // socket handle is closed; the receive thread then takes the mutex.
    closeWsHandles(std::move(handles));
    if (impl_->thread.joinable())
        impl_->thread.join();
}

void WsClient::run() {
    int backoffSec = 2;
    while (impl_->running) {
        std::string token = tokenProvider_ ? tokenProvider_() : std::string();
        if (token.empty()) {
            for (int i = 0; i < 20 && impl_->running; ++i)
                Sleep(500);
            continue;
        }

        ParsedUrl pu;
        if (!parseUrl(baseUrl_, pu))
            return;

        HINTERNET hSession = WinHttpOpen(L"NPSync/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession)
            WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 15000);
        if (hSession && !publishWsHandle(impl_, hSession)) {
            WinHttpCloseHandle(hSession);
            return;
        }

        HINTERNET hConnect = hSession ? WinHttpConnect(hSession, pu.host.c_str(), pu.port, 0) : nullptr;
        if (hConnect && !publishWsHandle(impl_, hConnect)) {
            // stop() is closing the session, which closes this connect.
            return;
        }

        HINTERNET hReq = nullptr;
        HINTERNET hWs = nullptr;
        do {
            if (!hConnect || !impl_->running)
                break;
            hReq = WinHttpOpenRequest(hConnect, L"GET", widen(pu.basePath + "/ws?token=" + token).c_str(),
                                      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      (pu.https ? WINHTTP_FLAG_SECURE : 0));
            if (!hReq)
                break;
            if (!publishWsHandle(impl_, hReq)) {
                // stop() is closing the session, which closes this request.
                hReq = nullptr;
                return;
            }
            if (!WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
                break;
            std::wstring headers = L"X-NPSync-Protocol: 1\r\n";
            if (!WinHttpSendRequest(hReq, headers.c_str(), (DWORD)headers.size(), nullptr, 0, 0, 0))
                break;
            if (!WinHttpReceiveResponse(hReq, nullptr))
                break;
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status != 101)
                break;
            hWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
            if (!hWs)
                break;
            if (!publishWsHandle(impl_, hWs)) {
                WinHttpCloseHandle(hWs);
                hWs = nullptr;
                return;
            }
            // Upgrade consumes the request. Close our copy when we still own it.
            closePublishedWsHandle(impl_, hReq);
            hReq = nullptr;
            if (!impl_->running)
                break;

            backoffSec = 2;
            if (onState_)
                onState_(true);

            // Receive loop: text events; server pings are answered by WinHTTP
            // and do not wake this call. stop() closes hWs to unblock it.
            while (impl_->running) {
                uint8_t buf[8192];
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                DWORD err = WinHttpWebSocketReceive(hWs, buf, sizeof(buf), &read, &type);
                if (err != 0 || !impl_->running)
                    break;
                if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                    break;
                if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                    type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
                    std::string msg((char*)buf, read);
                    try {
                        json ev = json::parse(msg);
                        if (onEvent_)
                            onEvent_(ev);
                    }
                    catch (...) {
                    }
                }
            }
            if (onState_)
                onState_(false);
        } while (false);

        // Handles stop() already closed are not in the list.
        closeWsHandles(takeWsHandles(impl_));
        hWs = nullptr;
        hReq = nullptr;

        if (!impl_->running)
            break;
        // Exponential backoff with cap, interruptible by stop().
        for (int i = 0; i < backoffSec * 2 && impl_->running; ++i)
            Sleep(500);
        backoffSec = (std::min)(backoffSec * 2, 60);
    }
}

} // namespace npsync
