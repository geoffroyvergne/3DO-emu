#pragma once

#include <string_view>

#include "common/types.hpp"

namespace core {

// Human-readable name of a MADAM/CLIO register, for diagnostics.
// Names come from Portfolio OS `clio.h` and Opera's opera_madam.c.
// Returns an empty view for addresses without a known name.
std::string_view mmio_register_name(u32 address);

}  // namespace core
