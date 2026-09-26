// The PGD tests pass their own keys; savedata_crypto.cpp still asks here.
#include "crypto_keys.hpp"

namespace portablekit {
const CryptoKeys *crypto_keys() { return nullptr; }
} // namespace portablekit
