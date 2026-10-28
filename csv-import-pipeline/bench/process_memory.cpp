// process_memory.cpp
// Reads process working-set and private-memory counters for benchmark evidence.

#include "process_memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace csvp_bench {

ProcessMemorySnapshot ReadProcessMemory()
{
	ProcessMemorySnapshot snapshot;
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters{};
	counters.cb = static_cast<DWORD>(sizeof(counters));
	// The Windows API uses the base pointer type while cb selects the extended layout.
	const BOOL succeeded = GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), counters.cb);
	if (succeeded == FALSE)
		return snapshot;

	snapshot.peakWorkingSetBytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
	snapshot.workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
	snapshot.privateUsageBytes = static_cast<std::uint64_t>(counters.PrivateUsage);
	snapshot.available = true;
#endif
	return snapshot;
}

} // namespace csvp_bench
