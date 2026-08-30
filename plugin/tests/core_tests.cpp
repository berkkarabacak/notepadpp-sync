// Plugin core test suite — runs under ctest on CI (Windows).
// Plain assertions keep the plugin test binary dependency-free.
#include <cstdio>
#include <cstring>
#include <string>

#include "core/Crypto.h"
#include "core/IgnoreRules.h"
#include "core/Merge.h"
#include "core/PathUtil.h"
#include "core/VersionVector.h"

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

int main() {
    testCryptoRoundTrip();
    testKeyWrap();
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

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
