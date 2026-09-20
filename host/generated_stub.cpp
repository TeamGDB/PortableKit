#include "psprecomp/runtime.hpp"

namespace psprecomp {

// Linked only while the profile's generated directory is empty so the host builds
// before the first AOT generation.
void register_generated_functions(Runtime &) {}

} // namespace psprecomp
