// ApiClient.h — HTTP (WinHTTP) client for the NPSync protocol, plus a
// WebSocket channel for realtime change notifications. Handles token refresh
// transparently and validates the X-NPSync-Protocol header on every response.
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace npsync
{

struct ApiResponse
{
    long status = 0;
    nlohmann::json body; // empty json if body wasn't JSON
    std::string rawBody;
    int serverProtocol = 0;   // from X-NPSync-Protocol response header
    bool transportOk = false; // false: DNS/TCP/TLS failure (offline)
    std::string transportError;
};

class ApiClient {
  public:
    ApiClient(std::string baseUrl, std::string deviceId);

    void setTokens(const std::string& access, const std::string& refresh);
    void clearTokens();
    bool hasTokens() const {
        return !refreshToken_.empty();
    }
    const std::string& accessToken() const {
        return accessToken_;
    }

    // Callback fired when tokens rotate (persist them via SettingsStore).
    std::function<void(const std::string& access, const std::string& refresh)> onTokensRotated;

    // ---- auth ----
    ApiResponse registerAccount(const std::string& email, const std::string& password,
                                const std::string& deviceName);
    ApiResponse login(const std::string& email, const std::string& password, const std::string& deviceName);
    // Google SSO: the server holds the OAuth client secret and PKCE verifier.
    // start returns authorization_url, state, and poll_secret. poll returns
    // 202 while the browser redirect is unfinished, then the usual token body.
    ApiResponse startGoogleLogin(const std::string& deviceName);
    ApiResponse pollGoogleLogin(const std::string& state, const std::string& pollSecret);
    ApiResponse logout();

    // ---- devices ----
    ApiResponse listDevices();
    ApiResponse revokeDevice(const std::string& deviceId);
    ApiResponse renameDevice(const std::string& deviceId, const std::string& name);
    ApiResponse pairRequest();
    ApiResponse pairApprove(const std::string& code, const std::string& wrappedKeyB64);
    ApiResponse pairPoll(const std::string& code);

    // ---- sync ----
    ApiResponse listFiles();
    ApiResponse getFile(const std::string& fileId);
    ApiResponse createFile(const nlohmann::json& filePayload);
    ApiResponse updateFile(const std::string& fileId, const nlohmann::json& filePayload);
    ApiResponse deleteFile(const std::string& fileId);
    ApiResponse listVersions(const std::string& fileId);
    ApiResponse restoreVersion(const std::string& fileId, int version);
    ApiResponse changesSince(int64_t seq);

    // ---- session ----
    ApiResponse getSession();
    ApiResponse putSession(const std::string& encryptedStateB64, int version);

    bool refreshAccessToken();

    // Unblocks any request() stuck in WinHTTP. Close joins those threads on
    // the Notepad++ UI thread; a 120s receive timeout would freeze WM_CLOSE.
    void cancelRequests();

  private:
    std::string baseUrl_;
    std::string deviceId_;
    std::string accessToken_;
    std::string refreshToken_;

    struct OpenHandles
    {
        void* session = nullptr;
        void* connect = nullptr;
        void* request = nullptr;
    };
    // WinHTTP handles for calls currently inside request(). cancelRequests
    // closes them from the shutdown thread so the caller returns.
    std::mutex inflightMu_;
    std::vector<OpenHandles> inflight_;
    bool cancel_ = false;

    bool trackSession(void* session);
    bool trackConnect(void* session, void* connect);
    bool trackRequest(void* session, void* request);
    // Closes the handles if this thread still owns them. False means
    // cancelRequests already closed them and the response must be discarded.
    bool releaseSession(void* session);

    ApiResponse request(const std::string& method, const std::string& path, const nlohmann::json* body,
                        bool authed, bool retryOn401 = true);
};

// ---- WebSocket ----

// Minimal WinHTTP-based WebSocket receiver. Runs its own thread; invokes
// onEvent(json) for each change event and onStateChange(connected) on
// connect/disconnect. Reconnects with backoff while running.
// stop() closes the socket before joining. WinHttpWebSocketReceive does not
// return just because the run flag changed, and it hides server ping frames.
class WsClient {
  public:
    using EventCallback = std::function<void(const nlohmann::json& ev)>;
    using StateCallback = std::function<void(bool connected)>;

    WsClient(std::string baseUrl, EventCallback onEvent, StateCallback onState);
    ~WsClient();

    void start(std::function<std::string()> accessTokenProvider);
    void stop();

  private:
    void run();
    // Impl is private. These stay members so MSVC allows them to touch it.
    // stop() closes the handles before join(); a free function cannot name Impl.
    static bool publishWsHandle(Impl* impl, void* handle);
    static std::vector<void*> takeWsHandles(Impl* impl);
    static void closeWsHandles(std::vector<void*> handles);
    static void closeOneWsHandle(Impl* impl, void* handle);

    std::string baseUrl_;
    EventCallback onEvent_;
    StateCallback onState_;
    std::function<std::string()> tokenProvider_;
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace npsync
