// Close used to freeze Notepad++. WM_CLOSE runs NPPN_SHUTDOWN on the UI
// thread, which joins the sync threads. Two of those waits do not end on
// their own:
//   * WinHttpWebSocketReceive stays blocked while the socket is idle. Server
//     pings do not wake it; WinHTTP answers them inside the call.
//   * An HTTP read waits out a 120 second timeout.
// stop() has to close those handles first. This test fails if either wait
// is still running two seconds after cancel.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <thread>

#include "ApiClient.h"

#pragma comment(lib, "ws2_32.lib")
#endif

static int g_failures = 0;

static void expect(bool cond, const std::string& msg) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
    else {
        std::printf("ok: %s\n", msg.c_str());
    }
}

namespace
{

uint32_t rol32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    uint64_t bits = static_cast<uint64_t>(len) * 8u;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            }
            else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(hs[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(hs[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(hs[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i]);
    }
}

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        unsigned n = static_cast<unsigned>(data[i]) << 16;
        if (i + 1 < len)
            n |= static_cast<unsigned>(data[i + 1]) << 8;
        if (i + 2 < len)
            n |= static_cast<unsigned>(data[i + 2]);
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? alphabet[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? alphabet[n & 63] : '=');
    }
    return out;
}

std::string websocketAccept(const std::string& key) {
    const std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t dig[20];
    sha1(reinterpret_cast<const uint8_t*>(src.data()), src.size(), dig);
    return base64Encode(dig, 20);
}

#ifdef _WIN32

struct Listener
{
    SOCKET sock = INVALID_SOCKET;
    int port = 0;
};

Listener listenLoopback() {
    Listener out;
    out.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out.sock == INVALID_SOCKET)
        return out;
    BOOL exclusive = TRUE;
    setsockopt(out.sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive),
               sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(out.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(out.sock);
        out.sock = INVALID_SOCKET;
        return out;
    }
    if (listen(out.sock, 1) != 0) {
        closesocket(out.sock);
        out.sock = INVALID_SOCKET;
        return out;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(out.sock, reinterpret_cast<sockaddr*>(&bound), &len);
    out.port = ntohs(bound.sin_port);
    return out;
}

SOCKET acceptOnce(SOCKET listenSock, int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(listenSock, &fds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(0, &fds, nullptr, nullptr, &tv) <= 0)
        return INVALID_SOCKET;
    return accept(listenSock, nullptr, nullptr);
}

void giveUp(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::fflush(stderr);
    ExitProcess(1);
}

// A peer that accepts and never answers. request() must return when cancelled,
// not after the 120s receive timeout.
void testCancelHttp() {
    Listener lst = listenLoopback();
    expect(lst.sock != INVALID_SOCKET, "http listen");
    if (lst.sock == INVALID_SOCKET)
        return;

    std::atomic<bool> accepted{false};
    std::atomic<bool> clientDone{false};
    std::thread server([&] {
        SOCKET client = acceptOnce(lst.sock, 8000);
        if (client == INVALID_SOCKET)
            return;
        accepted = true;
        DWORD slice = 200;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&slice), sizeof(slice));
        char buf[1024];
        while (!clientDone.load()) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n == 0)
                break;
            if (n < 0 && WSAGetLastError() != WSAETIMEDOUT && WSAGetLastError() != WSAEWOULDBLOCK)
                break;
        }
        closesocket(client);
    });

    npsync::ApiClient api("http://127.0.0.1:" + std::to_string(lst.port), "dev");
    npsync::ApiResponse response;
    std::thread caller([&] {
        response = api.getSession();
        clientDone = true;
    });

    for (int i = 0; i < 80 && !accepted.load(); ++i)
        Sleep(100);
    if (!accepted.load()) {
        clientDone = true;
        api.cancelRequests();
        caller.join();
        server.join();
        closesocket(lst.sock);
        expect(false, "http client never connected");
        return;
    }

    Sleep(100);
    ULONGLONG started = GetTickCount64();
    api.cancelRequests();
    for (int i = 0; i < 20 && !clientDone.load(); ++i)
        Sleep(100);
    ULONGLONG elapsed = GetTickCount64() - started;
    if (!clientDone.load()) {
        closesocket(lst.sock);
        giveUp("HTTP request still blocked 2s after cancelRequests");
    }
    caller.join();
    server.join();
    closesocket(lst.sock);
    expect(elapsed < 2000, "cancelRequests unblocked HTTP in " + std::to_string(elapsed) + "ms");
    expect(!response.transportOk, "cancelled HTTP is a transport failure, not a server answer");
}

std::string headerValue(const std::string& req, const char* name) {
    std::string lower = req;
    for (char& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::string needle = std::string(name) + ":";
    for (char& c : needle)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t pos = lower.find(needle);
    if (pos == std::string::npos)
        return {};
    size_t start = pos + needle.size();
    while (start < req.size() && (req[start] == ' ' || req[start] == '\t'))
        ++start;
    size_t end = req.find("\r\n", start);
    if (end == std::string::npos)
        end = req.size();
    return req.substr(start, end - start);
}

// Completes the WebSocket handshake and then sends nothing. stop() must
// return while WinHttpWebSocketReceive is blocked.
void testStopIdleWebSocket() {
    Listener lst = listenLoopback();
    expect(lst.sock != INVALID_SOCKET, "websocket listen");
    if (lst.sock == INVALID_SOCKET)
        return;

    std::atomic<bool> accepted{false};
    std::string seen;
    std::thread server([&] {
        SOCKET client = acceptOnce(lst.sock, 8000);
        if (client == INVALID_SOCKET)
            return;
        accepted = true;
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            req.append(buf, static_cast<size_t>(n));
            if (req.size() > 65536)
                break;
        }
        seen = req;
        std::string key = headerValue(req, "Sec-WebSocket-Key");
        if (!key.empty()) {
            std::string accept = websocketAccept(key);
            std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Accept: " +
                               accept + "\r\n\r\n";
            send(client, resp.data(), static_cast<int>(resp.size()), 0);
        }
        // Stay open. A timeout is not the peer leaving; closing here would
        // wake WinHttpWebSocketReceive and hide a stop() that still hangs.
        DWORD slice = 200;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&slice), sizeof(slice));
        for (;;) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n == 0)
                break;
            if (n < 0 && WSAGetLastError() != WSAETIMEDOUT && WSAGetLastError() != WSAEWOULDBLOCK)
                break;
        }
        closesocket(client);
    });

    std::atomic<bool> connected{false};
    npsync::WsClient ws(
        "http://127.0.0.1:" + std::to_string(lst.port), [](const nlohmann::json&) {},
        [&](bool up) {
            if (up)
                connected = true;
        });
    ws.start([] { return std::string("tok"); });

    for (int i = 0; i < 50 && !connected.load(); ++i)
        Sleep(100);
    if (!connected.load()) {
        ws.stop();
        server.join();
        closesocket(lst.sock);
        expect(false, "websocket did not finish the handshake; request was: " + seen);
        return;
    }

    // onState(true) runs before the receive call. Give that call time to block.
    Sleep(200);
    ULONGLONG started = GetTickCount64();
    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        ws.stop();
        stopped = true;
    });
    for (int i = 0; i < 20 && !stopped.load(); ++i)
        Sleep(100);
    ULONGLONG elapsed = GetTickCount64() - started;
    if (!stopped.load()) {
        closesocket(lst.sock);
        giveUp("WsClient::stop still blocked 2s into an idle WebSocket read");
    }
    stopper.join();
    server.join();
    closesocket(lst.sock);
    expect(elapsed < 2000, "stop() returned during an idle read in " + std::to_string(elapsed) + "ms");
}

#endif // _WIN32

} // namespace

int main() {
    expect(websocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
           "RFC 6455 handshake accept value");
#ifdef _WIN32
    WSADATA wsa{};
    expect(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup");
    testCancelHttp();
    testStopIdleWebSocket();
    WSACleanup();
#else
    std::printf("live WinHTTP checks run on Windows CI\n");
#endif
    std::printf("%d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
