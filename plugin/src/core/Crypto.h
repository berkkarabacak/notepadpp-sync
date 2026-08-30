// Crypto.h — client-side encryption for Notepad++ Sync.
//
// All encryption happens locally in the plugin. The server only ever sees
// ciphertext. Default cipher: AES-256-GCM via Windows CNG (BCrypt), an
// authenticated AEAD construction from the platform crypto library — no
// custom primitives. The layout leaves room for XChaCha20-Poly1305 via
// libsodium as an alternative provider (see CMake option NPSYNC_WITH_SODIUM).
//
// Envelope format (binary):
//   magic   4 bytes   "NPS1"
//   alg     1 byte    0x01 = AES-256-GCM
//   nonce   12 bytes
//   tag     16 bytes  (GCM auth tag)
//   data    N bytes   ciphertext
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace npsync
{

using Bytes = std::vector<uint8_t>;

// Master key: 32 bytes, generated locally on first setup, never sent anywhere.
constexpr size_t kMasterKeyLen = 32;
constexpr size_t kGcmNonceLen = 12;
constexpr size_t kGcmTagLen = 16;

class Crypto {
  public:
    // Generate `len` cryptographically secure random bytes (BCryptGenRandom).
    static Bytes random(size_t len);

    static Bytes generateMasterKey();

    // AEAD encrypt/decrypt with the 32-byte master key.
    // `aad` binds context (e.g. "file" / "metadata") so ciphertexts cannot be
    // transplanted between fields.
    static Bytes encrypt(const Bytes& key, const Bytes& plaintext, const std::string& aad);
    static bool decrypt(const Bytes& key, const Bytes& envelope, const std::string& aad, Bytes& plaintextOut);

    // SHA-256 hex digest (used for content hashes of ciphertext).
    static std::string sha256Hex(const Bytes& data);
    static std::string sha256Hex(const std::string& data);

    // ---- key wrapping for device pairing & recovery ----

    // Derive a 32-byte wrapping key from a pairing/recovery code using
    // PBKDF2-HMAC-SHA256 (CNG) with a random salt; returned alongside.
    static Bytes deriveKeyFromCode(const std::string& code, const Bytes& salt, uint32_t iterations = 200000);

    // Wrap (encrypt) the master key under a code-derived key.
    static Bytes wrapMasterKey(const Bytes& masterKey, const Bytes& wrappingKey);
    static bool unwrapMasterKey(const Bytes& wrapped, const Bytes& wrappingKey, Bytes& masterKeyOut);

    // ---- encoding helpers ----
    static std::string base64UrlEncode(const Bytes& data);
    static bool base64UrlDecode(const std::string& in, Bytes& out);

    // ---- recovery key ----
    // Human-storable offline recovery key: "NPSYNC-XXXX-XXXX-XXXX-XXXX-XXXX".
    // Encodes 128 bits of entropy; derives a wrapping key for the master key.
    static std::string generateRecoveryKey();
    static bool normalizeRecoveryKey(const std::string& in, std::string& normalizedOut);

  private:
    static Bytes aesGcmCrypt(bool encrypting, const Bytes& key, const Bytes& nonce, const Bytes& input,
                             const std::string& aad, Bytes& tagOut);
};

} // namespace npsync
