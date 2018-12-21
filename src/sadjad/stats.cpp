#include "sadjad/stats.h"

namespace global {
std::unordered_map<const void *, size_t> _bvhs_;
thread_local SadjadProfile _sfp_;
}  // namespace global
