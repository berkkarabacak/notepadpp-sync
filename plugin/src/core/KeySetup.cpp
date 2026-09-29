#include "KeySetup.h"

#include <nlohmann/json.hpp>

namespace npsync
{

namespace
{

bool pairingAlphabet(char c) {
    // Matches the server generator: Crockford-ish, no 0/O/1/I/L.
    const char* alphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (const char* p = alphabet; *p; ++p) {
        if (*p == c)
            return true;
    }
    return false;
}

} // namespace

bool normalizePairingCode(const std::string& in, std::string& normalizedOut) {
    std::string raw;
    raw.reserve(8);
    for (unsigned char ch : in) {
        if (ch == ' ' || ch == '-' || ch == '\t')
            continue;
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<unsigned char>(ch - 'a' + 'A');
        if (ch > 127 || !pairingAlphabet(static_cast<char>(ch)))
            return false;
        raw.push_back(static_cast<char>(ch));
    }
    if (raw.size() != 8)
        return false;
    normalizedOut = raw.substr(0, 4) + "-" + raw.substr(4);
    return true;
}

std::string wrapMasterKeyForPairing(const Bytes& masterKey, const std::string& code) {
    std::string normalized;
    if (masterKey.size() != kMasterKeyLen || !normalizePairingCode(code, normalized))
        return {};
    Bytes salt = Crypto::random(16);
    Bytes wrapping = Crypto::deriveKeyFromCode(normalized, salt);
    Bytes wrapped = Crypto::wrapMasterKey(masterKey, wrapping);
    nlohmann::json payload = {{"salt", Crypto::base64UrlEncode(salt)},
                              {"wrapped", Crypto::base64UrlEncode(wrapped)}};
    std::string dumped = payload.dump();
    return Crypto::base64UrlEncode(Bytes(dumped.begin(), dumped.end()));
}

bool installWrappedMasterKey(const std::string& wrappedPayloadB64, const std::string& code,
                             Bytes& masterKeyOut) {
    std::string normalized;
    if (!normalizePairingCode(code, normalized))
        return false;
    Bytes blob;
    if (!Crypto::base64UrlDecode(wrappedPayloadB64, blob) || blob.empty())
        return false;
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(std::string(blob.begin(), blob.end()));
    }
    catch (...) {
        return false;
    }
    Bytes salt, wrapped;
    if (!Crypto::base64UrlDecode(payload.value("salt", ""), salt) ||
        !Crypto::base64UrlDecode(payload.value("wrapped", ""), wrapped))
        return false;
    Bytes wrapping = Crypto::deriveKeyFromCode(normalized, salt);
    return Crypto::unwrapMasterKey(wrapped, wrapping, masterKeyOut);
}

WizardKeyOutcome applyWizardKeyStep(Bytes& masterKey, bool createIfMissing) {
    if (masterKey.size() == kMasterKeyLen)
        return WizardKeyOutcome::KeptExisting;
    if (!createIfMissing)
        return WizardKeyOutcome::RefusedToMint;
    masterKey = Crypto::generateMasterKey();
    return WizardKeyOutcome::CreatedNew;
}

} // namespace npsync
