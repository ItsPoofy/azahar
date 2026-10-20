// Local debugger integration of Luma3DS LayeredFS (GPL-3.0-or-later).
#pragma once

#include <vector>
#include "common/common_types.h"

struct ExHeader_CodeSetInfo;

namespace FileSys {

// An SD title directory opts this title into the guest SDK file redirection path.
bool HasLumaLayeredFs(u64 program_id);
bool ApplyLumaLayeredFs(std::vector<u8>& code, const ExHeader_CodeSetInfo& info, u64 program_id);

} // namespace FileSys
