#pragma once

#include <vector>

#include "common/common_types.h"

const std::vector<u32>& get_jak2_tpage_dir();
// NOTE: EXTRA_PC_PORT_TEXTURE_COUNT is defined in jak1_tpage_dir.h (as upstream does).
// It was temporarily moved here while jak1 was removed from the fork.
