#include "CpuScheduling.h"

#include <cassert>

using nSCD3D11::CpuScheduling::LogicalProcessor;
using nSCD3D11::CpuScheduling::SelectPerformanceCpuSets;

int main() {
	// 2 P-cores with hyperthreading (class 1) followed by 4 E-cores (class 0).
	std::vector<LogicalProcessor> const hybrid{
		{0x100, 0, 0, 1}, {0x101, 0, 1, 1}, {0x102, 0, 2, 1}, {0x103, 0, 3, 1},
		{0x104, 0, 4, 0}, {0x105, 0, 5, 0}, {0x106, 0, 6, 0}, {0x107, 0, 7, 0},
	};

	auto selected = SelectPerformanceCpuSets(hybrid, 0xFF);
	assert((selected == std::vector<uint32_t>{0x100, 0x101, 0x102, 0x103}));

	// Affinity narrows the candidates.
	selected = SelectPerformanceCpuSets(hybrid, 0x36);
	assert((selected == std::vector<uint32_t>{0x101, 0x102}));

	// Pinned to one P-core, or only E-cores allowed: nothing to narrow.
	assert(SelectPerformanceCpuSets(hybrid, 0x01).empty());
	assert(SelectPerformanceCpuSets(hybrid, 0xF0).empty());

	// Homogeneous CPU.
	std::vector<LogicalProcessor> const uniform{{0x100, 0, 0, 0}, {0x101, 0, 1, 0}, {0x102, 0, 2, 0}};
	assert(SelectPerformanceCpuSets(uniform, 0x07).empty());

	// Processors outside group 0 are unreachable for a 32-bit affinity mask.
	std::vector<LogicalProcessor> const grouped{
		{0x100, 0, 0, 0}, {0x101, 0, 1, 0}, {0x200, 1, 0, 1}, {0x201, 1, 1, 1},
	};
	assert(SelectPerformanceCpuSets(grouped, 0x03).empty());

	assert(SelectPerformanceCpuSets({}, 0xFF).empty());

	// Calls the runtime-resolved Windows APIs; Debug /RTC1 aborts on a mismatched stdcall signature.
	assert(nSCD3D11::CpuScheduling::Apply() >= 1);
	return 0;
}
