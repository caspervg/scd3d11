/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "CpuScheduling.h"

#include "Diagnostics.h"

#include <cstring>
#include <windows.h>

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace nSCD3D11::CpuScheduling {
	namespace {

		unsigned CountBits(uintptr_t mask) {
			unsigned count = 0;
			for (; mask != 0; mask &= mask - 1) ++count;
			return count;
		}

		// Windows 8 / 10 APIs, resolved at runtime so the DLL still loads on Windows 7. The type comes
		// from the SDK declaration: a hand-written stdcall signature with the wrong parameter count
		// unbalances the stack.
#define KERNEL32_FUNCTION(name) \
	reinterpret_cast<decltype(&::name)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), #name))

		void RaisePriority() {
			if (std::strstr(GetCommandLineA(), "-CPUPriority:") != nullptr) {
				Log(LogCategory::Initialization, "cpu: priority left to -CPUPriority");
				return;
			}
			DWORD const current = GetPriorityClass(GetCurrentProcess());
			if (current == HIGH_PRIORITY_CLASS || current == REALTIME_PRIORITY_CLASS) return;
			if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
				Log(LogCategory::Initialization, "cpu: process priority raised to high");
			} else {
				Log(LogCategory::Initialization, "cpu: SetPriorityClass failed (Win32 error %lu)", ::GetLastError());
			}
		}

		// Without this, Windows 11 treats the game as EcoQoS whenever it is not the foreground
		// window and steers its threads onto E-cores, and ignores its timer-resolution requests.
		void DisablePowerThrottling() {
			auto const setProcessInformation = KERNEL32_FUNCTION(SetProcessInformation);
			if (setProcessInformation == nullptr) return;

			PROCESS_POWER_THROTTLING_STATE state{};
			state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
			state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
			state.StateMask = 0;
			if (!setProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
				// Windows 10 rejects the timer-resolution bit.
				state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
				if (!setProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
					Log(LogCategory::Initialization, "cpu: power throttling opt-out failed (Win32 error %lu)",
					    ::GetLastError());
					return;
				}
			}
			Log(LogCategory::Initialization, "cpu: EcoQoS power throttling disabled");
		}

		unsigned PreferPerformanceCores(uintptr_t affinity) {
			auto const getInformation = KERNEL32_FUNCTION(GetSystemCpuSetInformation);
			auto const setDefaults = KERNEL32_FUNCTION(SetProcessDefaultCpuSets);
			unsigned const allowed = CountBits(affinity);
			if (getInformation == nullptr || setDefaults == nullptr) return allowed;

			ULONG size = 0;
			getInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
			if (size == 0) return allowed;
			std::vector<uint8_t> buffer(size);
			if (!getInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), size, &size,
			                    GetCurrentProcess(), 0)) {
				return allowed;
			}

			std::vector<LogicalProcessor> processors;
			for (ULONG offset = 0; offset < size;) {
				auto const *entry = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION const *>(buffer.data() + offset);
				if (entry->Size == 0) break;
				if (entry->Type == CpuSetInformation) {
					processors.push_back({
						entry->CpuSet.Id, entry->CpuSet.Group, entry->CpuSet.LogicalProcessorIndex,
						entry->CpuSet.EfficiencyClass
					});
				}
				offset += entry->Size;
			}

			std::vector<uint32_t> const selected = SelectPerformanceCpuSets(processors, affinity);
			if (selected.empty()) return allowed;
			if (!setDefaults(GetCurrentProcess(), reinterpret_cast<ULONG const *>(selected.data()),
			                 static_cast<USHORT>(selected.size()))) {
				Log(LogCategory::Initialization, "cpu: SetProcessDefaultCpuSets failed (Win32 error %lu)",
				    ::GetLastError());
				return allowed;
			}
			Log(LogCategory::Initialization, "cpu: threads limited to %u performance-core logical processors of %u",
			    static_cast<unsigned>(selected.size()), allowed);
			return static_cast<unsigned>(selected.size());
		}

	} // namespace

	std::vector<uint32_t> SelectPerformanceCpuSets(
		std::vector<LogicalProcessor> const &processors, uintptr_t affinityMask) {
		auto const allowed = [affinityMask](LogicalProcessor const &processor) {
			return processor.group == 0 && processor.index < sizeof(uintptr_t) * 8 &&
			       ((affinityMask >> processor.index) & 1) != 0;
		};

		uint8_t fastest = 0;
		uint8_t slowest = UINT8_MAX;
		for (auto const &processor: processors) {
			if (!allowed(processor)) continue;
			if (processor.efficiencyClass > fastest) fastest = processor.efficiencyClass;
			if (processor.efficiencyClass < slowest) slowest = processor.efficiencyClass;
		}
		if (fastest <= slowest) return {};

		std::vector<uint32_t> selected;
		for (auto const &processor: processors) {
			if (allowed(processor) && processor.efficiencyClass == fastest) selected.push_back(processor.cpuSetId);
		}
		if (selected.size() < 2) selected.clear();
		return selected;
	}

	unsigned Apply(void) {
		RaisePriority();
		DisablePowerThrottling();
		DWORD_PTR processMask = 0;
		DWORD_PTR systemMask = 0;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || processMask == 0) return 1;
		return PreferPerformanceCores(processMask);
	}

}
