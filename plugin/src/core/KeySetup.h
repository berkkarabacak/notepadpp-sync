// KeySetup.h — who holds the account master key.
//
// The server only relays an opaque wrapped blob. These functions wrap and
// unwrap that blob, and decide whether first-run setup may mint a new key.
// A recovery wrap created here stays on this device; nothing in this file
// sends it anywhere.
#pragma once

#include "Crypto.h"

#include <string>

namespace npsync
{

// "XXXX-XXXX" from the pairing alphabet (no 0/O/1/I/L). Accepts spaces,
// a missing dash, and lowercase so the computer that types the code and the
// computer that shows it derive the same wrapping key.
bool normalizePairingCode(const std::string& in, std::string& normalizedOut);

// AES-256-GCM wrap of the master key under a key derived from the code.
// The return value is what POST /devices/pair stores. Empty on failure.
// The raw master key is not in the string.
std::string wrapMasterKeyForPairing(const Bytes& masterKey, const std::string& code);

// Inverse of wrapMasterKeyForPairing. Fails closed on a bad blob or the
// wrong code.
bool installWrappedMasterKey(const std::string& wrappedPayloadB64, const std::string& code,
                             Bytes& masterKeyOut);

enum class WizardKeyOutcome
{
    KeptExisting, // a key is already on this device; do not mint another
    CreatedNew,   // this is the first computer; a new key was minted
    RefusedToMint // notes live on another computer and no key has arrived
};

// First-run decision used by the setup wizard.
// createIfMissing is true only when the user said this is the first computer.
// A key already installed — including one just unwrapped from pairing — is
// never replaced, even if createIfMissing is true.
WizardKeyOutcome applyWizardKeyStep(Bytes& masterKey, bool createIfMissing);

} // namespace npsync
