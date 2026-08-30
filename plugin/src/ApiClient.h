// ApiClient.h — HTTP (WinHTTP) client for the NPSync protocol, plus a
// WebSocket channel for realtime change notifications. Handles token refresh
// transparently and validates the X-NPSync-Protocol header on every response.
#pragma once

#include <functional>
#include <optional>
#include <string>

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

  private:
    std::string baseUrl_;
    std::string deviceId_;
    std::string accessToken_;
    std::string refreshToken_;

    ApiResponse request(const std::string& method, const std::string& path, const nlohmann::json* body,
                        bool authed, bool retryOn401 = true);
};

// ---- WebSocket ----

// Minimal WinHTTP-based WebSocket receiver. Runs its own thread; invokes
// onEvent(json) for each change event and onStateChange(connected) on
// connect/disconnect. Reconnects with backoff while running.
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

    std::string baseUrl_;
    EventCallback onEvent_;
    StateCallback onState_;
    std::function<std::string()> tokenProvider_;
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace npsync
