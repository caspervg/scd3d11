/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace nSCD3D11::CpuScheduling {

	struct LogicalProcessor {
		uint32_t cpuSetId;
		uint16_t group;
		uint8_t index;           // logical processor index within its group
		uint8_t efficiencyClass; // higher is faster (P-cores above E-cores)
	};

	// CPU set ids of the fastest efficiency class within the affinity mask. Empty when there
	// is nothing to narrow: a homogeneous CPU, or fewer than two fast processors allowed.
	std::vector<uint32_t> SelectPerformanceCpuSets(
		std::vector<LogicalProcessor> const &processors, uintptr_t affinityMask);

	// Keeps SimCity 4 off E-cores and out of Windows power throttling on hybrid CPUs: raises
	// the process to high priority (unless -CPUPriority: is on the command line), opts out of
	// EcoQoS, and makes the P-cores the default CPU sets for every thread. Returns how many
	// logical processors the process may run on afterwards.
	unsigned Apply(void);

}
