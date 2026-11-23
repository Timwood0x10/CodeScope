#include "engine_index_sched.h"

namespace engine_index_sched
{

std::atomic<bool> g_parse_done{ false };
std::atomic<uint32_t> g_active_parse_threads{ 0 };

} // namespace engine_index_sched
