// Plugin test suite — runs under ctest on CI (Windows).
// Plain assertions. Settings persistence is linked in so the shipping
// Backend URL can be checked against a real settings.json.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Settings.h"
#include "core/Crypto.h"
#include "core/IgnoreRules.h"
#include "core/KeySetup.h"
#include "core/Merge.h"
#include "core/PathUtil.h"
#include "core/VersionVector.h"

#include <map>

using namespace npsync;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        ++g_checks;                                                                                          \
        if (!(cond)) {                                                                                       \
            ++g_failures;                                                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                      \
        }                                                                                                    \
    } while (0)

// ---- crypto ----

void testCryptoRoundTrip() {
    Bytes key = Crypto::generateMasterKey();
    std::string msg = "hello synchronized world — 你好";
    Bytes plain(msg.begin(), msg.end());

    Bytes env = Crypto::encrypt(key, plain, "file");
    Bytes out;
    CHECK(Crypto::decrypt(key, env, "file", out));
    CHECK(out == plain);

    // Wrong AAD must fail authentication (no ciphertext transplant).
    Bytes out2;
    CHECK(!Crypto::decrypt(key, env, "metadata", out2));

    // Wrong key must fail.
    Bytes other = Crypto::generateMasterKey();
    Bytes out3;
    CHECK(!Crypto::decrypt(other, env, "file", out3));

    // Tampered ciphertext must fail.
    Bytes bad = env;
    bad[bad.size() - 1] ^= 0x01;
    Bytes out4;
    CHECK(!Crypto::decrypt(key, bad, "file", out4));
}

void testEmptyCiphertextAuth() {
    Bytes key = Crypto::generateMasterKey();
    // Empty plaintext is a real payload: the tag still has to match.
    Bytes env = Crypto::encrypt(key, Bytes{}, "file");
    const size_t headerLen = 4 + 1 + 12 + 16;
    CHECK(env.size() == headerLen);
    Bytes out;
    CHECK(Crypto::decrypt(key, env, "file", out));
    CHECK(out.empty());

    // A header with an empty ciphertext and a flipped tag used to decrypt
    // as success, because auth failure and empty plaintext both looked empty.
    Bytes forged = env;
    forged[5 + 12] ^= 0xff; // first tag byte
    Bytes leftover = {0xab};
    CHECK(!Crypto::decrypt(key, forged, "file", leftover));

    Bytes truncated(env.begin(), env.begin() + 8);
    CHECK(!Crypto::decrypt(key, truncated, "file", leftover));
}

void testKeyWrap() {
    Bytes mk = Crypto::generateMasterKey();
    Bytes salt = Crypto::random(16);
    Bytes wk = Crypto::deriveKeyFromCode("ABCD-EFGH", salt);
    Bytes wk2 = Crypto::deriveKeyFromCode("ABCD-EFGH", salt);
    CHECK(wk == wk2); // deterministic for same code+salt

    Bytes wrapped = Crypto::wrapMasterKey(mk, wk);
    Bytes unwrapped;
    CHECK(Crypto::unwrapMasterKey(wrapped, wk2, unwrapped));
    CHECK(unwrapped == mk);

    Bytes wrong = Crypto::deriveKeyFromCode("XXXX-YYYY", salt);
    Bytes out;
    CHECK(!Crypto::unwrapMasterKey(wrapped, wrong, out));
}

void testRecoveryKey() {
    std::string rk = Crypto::generateRecoveryKey();
    CHECK(rk.rfind("NPSYNC-", 0) == 0);
    std::string norm;
    CHECK(Crypto::normalizeRecoveryKey(rk, norm));
    CHECK(norm == rk);
    // Accept sloppy input (lowercase, missing dashes).
    std::string messy = "npsync " + rk.substr(7);
    std::string norm2;
    if (Crypto::normalizeRecoveryKey(messy, norm2)) {
        CHECK(norm2 == rk);
    }
    CHECK(!Crypto::normalizeRecoveryKey("NPSYNC-AAA", norm));
}

void testSha256() {
    // RFC 4231 / well-known vector: sha256("abc").
    CHECK(Crypto::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void testBase64Url() {
    Bytes data = {0x00, 0x10, 0x83, 0xff, 0xee, 0x01};
    std::string enc = Crypto::base64UrlEncode(data);
    Bytes dec;
    CHECK(Crypto::base64UrlDecode(enc, dec));
    CHECK(dec == data);
    CHECK(enc.find('=') == std::string::npos); // paddingless
    CHECK(enc.find('+') == std::string::npos);
    CHECK(enc.find('/') == std::string::npos);
}

// ---- merge ----

void testMergeClean() {
    std::string base = "alpha\nbravo\ncharlie\ndelta\n";
    // Local edits line 2, remote edits line 4 -> disjoint, auto-merge.
    std::string local = "alpha\nBRAVO-local\ncharlie\ndelta\n";
    std::string remote = "alpha\nbravo\ncharlie\nDELTA-remote\n";
    auto r = ThreeWayMerge::merge(base, local, remote);
    CHECK(r.clean);
    CHECK(r.merged == "alpha\nBRAVO-local\ncharlie\nDELTA-remote\n");
}

void testMergeIdenticalEdits() {
    std::string base = "a\nb\nc\n";
    std::string local = "a\nX\nc\n";
    std::string remote = "a\nX\nc\n";
    auto r = ThreeWayMerge::merge(base, local, remote);
    CHECK(r.clean);
    CHECK(r.merged == "a\nX\nc\n");
}

void testMergeOneSideUnchanged() {
    std::string base = "a\nb\n";
    auto r1 = ThreeWayMerge::merge(base, "a\nCHANGED\n", base);
    CHECK(r1.clean && r1.merged == "a\nCHANGED\n");
    auto r2 = ThreeWayMerge::merge(base, base, "NEW\na\nb\n");
    CHECK(r2.clean && r2.merged == "NEW\na\nb\n");
}

void testMergeConflict() {
    std::string base = "alpha\nbravo\ncharlie\n";
    std::string local = "alpha\nbravo-local\ncharlie\n";
    std::string remote = "alpha\nbravo-remote\ncharlie\n";
    auto r = ThreeWayMerge::merge(base, local, remote);
    CHECK(!r.clean);
    CHECK(r.hunks.size() == 1);
    // Both versions preserved in the hunk — nothing lost.
    CHECK(!r.hunks[0].localLines.empty());
    CHECK(!r.hunks[0].remoteLines.empty());
    CHECK(r.hunks[0].localLines[0] == "bravo-local");
    CHECK(r.hunks[0].remoteLines[0] == "bravo-remote");
}

void testMergeAppends() {
    std::string base = "one\ntwo\n";
    std::string local = "one\ntwo\nthree-local\n";
    std::string remote = "one\ntwo\nthree-remote\n";
    auto r = ThreeWayMerge::merge(base, local, remote);
    CHECK(!r.clean); // same region appended differently -> conflict, both kept
    CHECK(r.hunks.size() >= 1);
}

void testMergeCrlf() {
    std::string base = "a\r\nb\r\n";
    std::string local = "a\r\nB\r\n";
    auto r = ThreeWayMerge::merge(base, local, base);
    CHECK(r.clean);
    CHECK(r.merged.find("\r\n") != std::string::npos);
}

// ---- ignore rules ----

void testIgnoreRules() {
    auto rules = IgnoreRules::parse("# comment\n"
                                    "*.tmp\n"
                                    "*.log\n"
                                    ".git/\n"
                                    "node_modules/\n"
                                    "/root-only.txt\n"
                                    "build/output/\n"
                                    "!keep.tmp\n");

    CHECK(rules.ignored("notes.tmp", false));
    CHECK(rules.ignored("deep/dir/x.log", false));
    CHECK(rules.ignored(".git", true));
    CHECK(rules.ignored(".git/config", false));
    CHECK(rules.ignored("node_modules", true));
    CHECK(rules.ignored("node_modules/pkg/index.js", false));
    CHECK(rules.ignored("root-only.txt", false));
    CHECK(!rules.ignored("sub/root-only.txt", false));
    CHECK(rules.ignored("build/output", true));
    CHECK(rules.ignored("build/output/f.bin", false));
    CHECK(!rules.ignored("keep.tmp", false)); // negated
    CHECK(!rules.ignored("notes.txt", false));
    CHECK(!rules.ignored("src/main.cpp", false));
}

// ---- path safety ----

void testPathNormalization() {
    std::string out;
    CHECK(PathUtil::normalizeRelative("notes/todo.txt", out) && out == "notes/todo.txt");
    CHECK(PathUtil::normalizeRelative("notes\\win\\style.txt", out) && out == "notes/win/style.txt");
    CHECK(PathUtil::normalizeRelative("./a/./b.txt", out) && out == "a/b.txt");
    CHECK(PathUtil::normalizeRelative("a/sub/../b.txt", out) && out == "a/b.txt");

    // Traversal attacks must all fail.
    CHECK(!PathUtil::normalizeRelative("../escape.txt", out));
    CHECK(!PathUtil::normalizeRelative("a/../../escape.txt", out));
    CHECK(!PathUtil::normalizeRelative("/abs/path.txt", out));
    CHECK(!PathUtil::normalizeRelative("C:/windows/win.ini", out));
    CHECK(!PathUtil::normalizeRelative("..\\..\\win.ini", out));
    // Windows hazards.
    CHECK(!PathUtil::normalizeRelative("CON", out));
    CHECK(!PathUtil::normalizeRelative("dir/file<>.txt", out));
    CHECK(!PathUtil::normalizeRelative("endswithdot.", out));
}

void testJoinInsideRoot() {
    std::wstring abs;
    CHECK(PathUtil::joinInsideRoot(L"C:\\Users\\me\\Notes", "sub/file.txt", abs));
    CHECK(PathUtil::isInsideRoot(L"C:\\Users\\me\\Notes", abs));
    CHECK(!PathUtil::joinInsideRoot(L"C:\\Users\\me\\Notes", "../evil.txt", abs));
    CHECK(!PathUtil::isInsideRoot(L"C:\\Users\\me\\Notes", L"C:\\Users\\me\\NotesEvil\\x.txt"));
    CHECK(PathUtil::isInsideRoot(L"c:\\users\\me\\notes\\", L"C:\\Users\\me\\Notes\\a.txt"));
}

// ---- version vectors ----

void testVersionVectors() {
    VersionVector a, b;
    a.entries["A"] = 5;
    a.entries["B"] = 3;
    b.entries["A"] = 5;
    b.entries["B"] = 3;
    CHECK(a.equal(b));
    CHECK(!a.concurrent(b));

    b.entries["B"] = 4;
    CHECK(b.dominates(a));
    CHECK(!a.dominates(b));
    CHECK(!a.concurrent(b));

    a.entries["A"] = 6; // now each dominates in one component
    CHECK(a.concurrent(b));

    VersionVector m = a;
    m.merge(b);
    CHECK(m.entries["A"] == 6 && m.entries["B"] == 4);

    std::string json = m.toJson();
    VersionVector parsed = VersionVector::fromJson(json);
    CHECK(parsed.equal(m));
}

// ---- settings: shipping Backend URL ----

void testDefaultBackendUrl() {
    Settings fresh;
    CHECK(fresh.backendUrl == "https://sync.berkkarabacak.com");
    CHECK(std::string(kDefaultBackendUrl) == "https://sync.berkkarabacak.com");
    CHECK(fresh.backendUrl == kDefaultBackendUrl);
    CHECK(fresh.backendUrl.find("example.com") == std::string::npos);

    // No settings file: the in-memory default is what a fresh install uses.
    CHECK(resolveBackendUrl("", false) == kDefaultBackendUrl);
    // Omitted, blank, or the retired placeholder must not stick.
    CHECK(resolveBackendUrl("https://sync.example.com", true) == kDefaultBackendUrl);
    CHECK(resolveBackendUrl("https://sync.example.com", false) == kDefaultBackendUrl);
    CHECK(resolveBackendUrl("", true) == kDefaultBackendUrl);
    // Self-host and local development overrides stay.
    CHECK(resolveBackendUrl("http://localhost:8080", true) == "http://localhost:8080");
    CHECK(resolveBackendUrl("http://127.0.0.1:8080", true) == "http://127.0.0.1:8080");
    CHECK(resolveBackendUrl("https://sync.myserver.com", true) == "https://sync.myserver.com");

    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    CHECK(n > 0 && n < MAX_PATH);
    std::wstring dir = std::wstring(temp) + L"npsync-backend-url-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring settingsFile = dir + L"\\settings.json";
    DeleteFileW(settingsFile.c_str());

    SettingsStore store(dir);
    Settings missingFile;
    CHECK(!store.load(missingFile));
    CHECK(missingFile.backendUrl == "https://sync.berkkarabacak.com");

    auto writeJson = [&](const std::string& body) {
        std::ofstream f(settingsFile, std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(f));
        f << body;
    };

    writeJson("{\n  \"device_name\": \"Laptop\"\n}\n");
    Settings omitted;
    omitted.backendUrl = "https://sync.example.com";
    CHECK(store.load(omitted));
    CHECK(omitted.backendUrl == "https://sync.berkkarabacak.com");
    CHECK(omitted.deviceName == "Laptop");

    writeJson("{\n  \"backend_url\": \"https://sync.example.com\"\n}\n");
    Settings retired;
    CHECK(store.load(retired));
    CHECK(retired.backendUrl == "https://sync.berkkarabacak.com");

    writeJson("{\n  \"backend_url\": \"\"\n}\n");
    Settings blank;
    CHECK(store.load(blank));
    CHECK(blank.backendUrl == "https://sync.berkkarabacak.com");

    Settings custom;
    custom.backendUrl = "http://localhost:8080";
    custom.deviceName = "Dev";
    CHECK(store.save(custom));
    Settings roundTrip;
    roundTrip.backendUrl = "https://sync.example.com";
    CHECK(store.load(roundTrip));
    CHECK(roundTrip.backendUrl == "http://localhost:8080");
    CHECK(roundTrip.deviceName == "Dev");

    DeleteFileW(settingsFile.c_str());
    RemoveDirectoryW(dir.c_str());
}

// In-memory stand-in for POST /devices/pair. It stores the wrapped blob
// opaquely and enforces the live server's rules: the device that requested
// the code cannot approve it, only that device can poll, and the code is
// single-use. Two Notepad++ windows that share a device id are one device.
struct PairingRelay
{
    struct Slot
    {
        std::string requester;
        std::string wrapped;
        bool consumed = false;
    };
    std::map<std::string, Slot> codes;
    int issued = 0;

    std::string request(const std::string& deviceId) {
        static const char* samples[] = {"ABCD-EFGH", "MNPQ-RSTU", "WXYZ-2345"};
        std::string code = samples[issued++ % 3];
        codes[code] = Slot{deviceId, "", false};
        return code;
    }

    bool approve(const std::string& deviceId, const std::string& code, const std::string& wrapped,
                 std::string& error) {
        std::string norm;
        if (!normalizePairingCode(code, norm) || !codes.count(norm) || codes[norm].consumed) {
            error = "not found";
            return false;
        }
        if (codes[norm].requester == deviceId) {
            error = "cannot approve your own pairing request";
            return false;
        }
        if (wrapped.empty()) {
            error = "wrapped required";
            return false;
        }
        codes[norm].wrapped = wrapped;
        return true;
    }

    bool poll(const std::string& deviceId, const std::string& code, std::string& wrapped,
              std::string& error) {
        std::string norm;
        if (!normalizePairingCode(code, norm) || !codes.count(norm) || codes[norm].requester != deviceId) {
            error = "not found";
            return false;
        }
        if (codes[norm].consumed) {
            error = "pairing code expired";
            return false;
        }
        if (codes[norm].wrapped.empty()) {
            error = "pending";
            return false;
        }
        wrapped = codes[norm].wrapped;
        codes[norm].consumed = true;
        return true;
    }
};

// Product path: PC1 mints the account key, PC2 must not mint while it waits,
// PC1 allows the code, PC2 polls and installs that same key, then the wizard
// still does not mint. A note encrypted on PC1 decrypts on PC2.
void testSecondComputerReceivesKeyAndWizardDoesNotMint() {
    Bytes pc1;
    CHECK(applyWizardKeyStep(pc1, true) == WizardKeyOutcome::CreatedNew);
    CHECK(pc1.size() == kMasterKeyLen);
    const std::string note = "grocery list from the first PC";
    Bytes cipher = Crypto::encrypt(pc1, Bytes(note.begin(), note.end()), "file");

    Bytes pc2;
    CHECK(applyWizardKeyStep(pc2, false) == WizardKeyOutcome::RefusedToMint);
    CHECK(pc2.empty());

    const std::string id1 = "pc1";
    const std::string id2 = "pc2";
    PairingRelay server;
    std::string code = server.request(id2);

    std::string err;
    CHECK(!server.approve(id2, code, "opaque-blob-not-a-key", err));
    CHECK(err.find("own pairing") != std::string::npos);

    std::string wrapped;
    CHECK(!server.poll(id2, code, wrapped, err));
    CHECK(err == "pending");
    CHECK(applyWizardKeyStep(pc2, false) == WizardKeyOutcome::RefusedToMint);
    CHECK(pc2.empty());

    // Typed the way a person types it. The wrap is what the server would store.
    const std::string typed = "abcd efgh";
    std::string blob = wrapMasterKeyForPairing(pc1, typed);
    CHECK(!blob.empty());
    CHECK(server.approve(id1, typed, blob, err));
    CHECK(!server.poll(id1, code, wrapped, err));
    CHECK(server.poll(id2, code, wrapped, err));
    CHECK(wrapped == blob);
    CHECK(installWrappedMasterKey(wrapped, code, pc2));
    CHECK(pc2 == pc1);
    CHECK(!server.poll(id2, code, wrapped, err));
    CHECK(err == "pairing code expired");

    Bytes beforeWizard = pc2;
    CHECK(applyWizardKeyStep(pc2, false) == WizardKeyOutcome::KeptExisting);
    CHECK(applyWizardKeyStep(pc2, true) == WizardKeyOutcome::KeptExisting);
    CHECK(pc2 == beforeWizard);
    CHECK(pc2 == pc1);

    Bytes plain;
    CHECK(Crypto::decrypt(pc2, cipher, "file", plain));
    CHECK(std::string(plain.begin(), plain.end()) == note);

    Bytes pc3;
    CHECK(applyWizardKeyStep(pc3, true) == WizardKeyOutcome::CreatedNew);
    CHECK(pc3 != pc1);
    Bytes nope;
    CHECK(!Crypto::decrypt(pc3, cipher, "file", nope));
}

int main() {
    testCryptoRoundTrip();
    testEmptyCiphertextAuth();
    testKeyWrap();
    testSecondComputerReceivesKeyAndWizardDoesNotMint();
    testRecoveryKey();
    testSha256();
    testBase64Url();

    testMergeClean();
    testMergeIdenticalEdits();
    testMergeOneSideUnchanged();
    testMergeConflict();
    testMergeAppends();
    testMergeCrlf();

    testIgnoreRules();
    testPathNormalization();
    testJoinInsideRoot();
    testVersionVectors();
    testDefaultBackendUrl();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
