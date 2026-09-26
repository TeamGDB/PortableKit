// host/crypto_keys_builtin.cpp under other names, for a test that also links
// the desktop app's keys file (which defines the usual ones): the values stay
// written in exactly one place.
#define crypto_keys builtin_crypto_keys
#define keys_come_from_keys_file builtin_keys_come_from_keys_file
#include "crypto_keys_builtin.cpp"
