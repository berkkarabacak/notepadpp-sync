// Crypto.cpp — AES-256-GCM via Windows CNG (BCrypt). No custom primitives.
#include "Crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <bcrypt.h>
#include <windows.h>

#include <cctype>
#include <cstring>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace npsync
{

namespace
{

constexpr uint8_t kEnvelopeMagic[4] = {'N', 'P', 'S', '1'};
constexpr uint8_t kAlgAes256Gcm = 0x01;

struct AlgHandle
{
    BCRYPT_ALG_HANDLE h = nullptr;
    AlgHandle() {
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
            throw std::runtime_error("BCryptOpenAlgorithmProvider(AES) failed");
        if (BCryptSetProperty(h, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            BCryptCloseAlgorithmProvider(h, 0);
            throw std::runtime_error("BCryptSetProperty(GCM) failed");
        }
    }
    ~AlgHandle() {
        if (h)
            BCryptCloseAlgorithmProvider(h, 0);
    }
};

BCRYPT_ALG_HANDLE aesProvider() {
    static AlgHandle inst;
    return inst.h;
}

} // namespace

Bytes Crypto::random(size_t len) {
    Bytes out(len);
    if (len == 0)
        return out;
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("BCryptGenRandom failed");
    return out;
}

Bytes Crypto::generateMasterKey() {
    return random(kMasterKeyLen);
}

Bytes Crypto::aesGcmCrypt(bool encrypting, const Bytes& key, const Bytes& nonce, const Bytes& input,
                          const std::string& aad, Bytes& tagOut) {
    if (key.size() != kMasterKeyLen || nonce.size() != kGcmNonceLen)
        throw std::invalid_argument("bad key/nonce size");
    if (!encrypting && tagOut.size() != kGcmTagLen)
        throw std::invalid_argument("bad tag size");

    BCRYPT_KEY_HANDLE kh = nullptr;
    if (BCryptGenerateSymmetricKey(aesProvider(), &kh, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                   static_cast<ULONG>(key.size()), 0) != 0)
        throw std::runtime_error("BCryptGenerateSymmetricKey failed");

    Bytes producedTag;
    if (encrypting)
        producedTag.resize(kGcmTagLen);
    // For encryption CNG writes the tag; for decryption it reads the expected
    // tag from the same field and fails with STATUS_AUTH_TAG_MISMATCH on
    // mismatch.
    PUCHAR tagPtr = encrypting ? producedTag.data() : tagOut.data();

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = aad.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tagPtr;
    info.cbTag = kGcmTagLen;

    Bytes out(input.size());
    ULONG produced = 0;
    NTSTATUS st;
    if (encrypting) {
        st = BCryptEncrypt(kh, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &info,
                           nullptr, 0, out.data(), static_cast<ULONG>(out.size()), &produced, 0);
    }
    else {
        st = BCryptDecrypt(kh, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &info,
                           nullptr, 0, out.data(), static_cast<ULONG>(out.size()), &produced, 0);
    }
    BCryptDestroyKey(kh);
    if (st != 0) {
        if (!encrypting)
            return {}; // auth failure -> empty
        throw std::runtime_error("BCryptEncrypt failed");
    }
    out.resize(produced);
    if (encrypting)
        tagOut = std::move(producedTag);
    return out;
}

Bytes Crypto::encrypt(const Bytes& key, const Bytes& plaintext, const std::string& aad) {
    Bytes nonce = random(kGcmNonceLen);
    Bytes tag;
    Bytes ct = aesGcmCrypt(true, key, nonce, plaintext, aad, tag);

    Bytes env;
    env.reserve(4 + 1 + kGcmNonceLen + kGcmTagLen + ct.size());
    env.insert(env.end(), std::begin(kEnvelopeMagic), std::end(kEnvelopeMagic));
    env.push_back(kAlgAes256Gcm);
    env.insert(env.end(), nonce.begin(), nonce.end());
    env.insert(env.end(), tag.begin(), tag.end());
    env.insert(env.end(), ct.begin(), ct.end());
    return env;
}

bool Crypto::decrypt(const Bytes& key, const Bytes& envelope, const std::string& aad, Bytes& plaintextOut) {
    const size_t headerLen = 4 + 1 + kGcmNonceLen + kGcmTagLen;
    if (envelope.size() < headerLen)
        return false;
    if (std::memcmp(envelope.data(), kEnvelopeMagic, 4) != 0)
        return false;
    if (envelope[4] != kAlgAes256Gcm)
        return false;

    Bytes nonce(envelope.begin() + 5, envelope.begin() + 5 + kGcmNonceLen);
    Bytes tag(envelope.begin() + 5 + kGcmNonceLen, envelope.begin() + headerLen);
    Bytes ct(envelope.begin() + headerLen, envelope.end());

    Bytes out = aesGcmCrypt(false, key, nonce, ct, aad, tag);
    if (out.empty() && !ct.empty())
        return false;
    plaintextOut = std::move(out);
    return true;
}

std::string Crypto::sha256Hex(const Bytes& data) {
    BCRYPT_ALG_HANDLE ah = nullptr;
    if (BCryptOpenAlgorithmProvider(&ah, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("sha256 provider failed");
    BCRYPT_HASH_HANDLE hh = nullptr;
    uint8_t digest[32];
    NTSTATUS st = BCryptCreateHash(ah, &hh, nullptr, 0, nullptr, 0, 0);
    if (st == 0)
        st = BCryptHashData(hh, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
    if (st == 0)
        st = BCryptFinishHash(hh, digest, sizeof(digest), 0);
    if (hh)
        BCryptDestroyHash(hh);
    BCryptCloseAlgorithmProvider(ah, 0);
    if (st != 0)
        throw std::runtime_error("sha256 failed");

    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(hexd[b >> 4]);
        out.push_back(hexd[b & 0x0f]);
    }
    return out;
}

std::string Crypto::sha256Hex(const std::string& data) {
    return sha256Hex(Bytes(data.begin(), data.end()));
}

Bytes Crypto::deriveKeyFromCode(const std::string& code, const Bytes& salt, uint32_t iterations) {
    BCRYPT_ALG_HANDLE ah = nullptr;
    if (BCryptOpenAlgorithmProvider(&ah, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        throw std::runtime_error("hmac provider failed");
    Bytes key(kMasterKeyLen);
    NTSTATUS st = BCryptDeriveKeyPBKDF2(ah, reinterpret_cast<PUCHAR>(const_cast<char*>(code.data())),
                                        static_cast<ULONG>(code.size()), const_cast<PUCHAR>(salt.data()),
                                        static_cast<ULONG>(salt.size()), iterations, key.data(),
                                        static_cast<ULONG>(key.size()), 0);
    BCryptCloseAlgorithmProvider(ah, 0);
    if (st != 0)
        throw std::runtime_error("PBKDF2 failed");
    return key;
}

Bytes Crypto::wrapMasterKey(const Bytes& masterKey, const Bytes& wrappingKey) {
    return encrypt(wrappingKey, masterKey, "npsync-keywrap");
}

bool Crypto::unwrapMasterKey(const Bytes& wrapped, const Bytes& wrappingKey, Bytes& masterKeyOut) {
    return decrypt(wrappingKey, wrapped, "npsync-keywrap", masterKeyOut) &&
           masterKeyOut.size() == kMasterKeyLen;
}

std::string Crypto::base64UrlEncode(const Bytes& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
    }
    if (i < data.size()) {
        uint32_t v = data[i] << 16;
        bool two = (i + 1 < data.size());
        if (two)
            v |= data[i + 1] << 8;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        if (two)
            out.push_back(tbl[(v >> 6) & 63]);
    }
    return out;
}

bool Crypto::base64UrlDecode(const std::string& in, Bytes& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '-')
            return 62;
        if (c == '_')
            return 63;
        return -1;
    };
    out.clear();
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0)
            return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return true;
}

std::string Crypto::generateRecoveryKey() {
    Bytes raw = random(20);                                          // one byte per character
    static const char* alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // no 0O1IL
    std::string out = "NPSYNC-";
    for (size_t i = 0; i < 20; ++i) {
        if (i > 0 && i % 4 == 0)
            out.push_back('-');
        out.push_back(alphabet[raw[i] % 32]);
    }
    return out;
}

bool Crypto::normalizeRecoveryKey(const std::string& in, std::string& normalizedOut) {
    std::string body;
    for (char c : in) {
        if (c == '-' || c == ' ')
            continue;
        body.push_back(static_cast<char>(::toupper(static_cast<unsigned char>(c))));
    }
    const std::string prefix = "NPSYNC";
    if (body.rfind(prefix, 0) == 0)
        body = body.substr(prefix.size());
    if (body.size() != 20)
        return false;
    normalizedOut = "NPSYNC-";
    for (size_t i = 0; i < body.size(); ++i) {
        if (i > 0 && i % 4 == 0)
            normalizedOut.push_back('-');
        normalizedOut.push_back(body[i]);
    }
    return true;
}

} // namespace npsync
