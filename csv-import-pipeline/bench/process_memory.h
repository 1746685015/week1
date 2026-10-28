#ifndef CSVP_BENCH_PROCESS_MEMORY_H_
#define CSVP_BENCH_PROCESS_MEMORY_H_

#include <cstdint>

namespace csvp_bench {

struct ProcessMemorySnapshot
{
	std::uint64_t peakWorkingSetBytes = 0U;
	std::uint64_t workingSetBytes = 0U;
	std::uint64_t privateUsageBytes = 0U;
	bool available = false;
};

ProcessMemorySnapshot ReadProcessMemory();

} // namespace csvp_bench

#endif // CSVP_BENCH_PROCESS_MEMORY_H_
