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

// Where a thread spends its time, without a profiler (WPR needs an elevated prompt): another thread stops it about
// every millisecond and notes the function it is in, named from the PDB at the end. For the tools only
// (SCD3D12Bench, SCD3D12Server replay --profile). Link dbghelp and winmm.

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace nSCDTools {
	class Sampler {
	public:
		// Samples the calling thread until Report.
		void Start() {
			DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE,
			                DUPLICATE_SAME_ACCESS);
			thread = std::thread([this] {
				timeBeginPeriod(1);
				while (!stop.load()) {
					if (SuspendThread(target) != static_cast<DWORD>(-1)) {
						CONTEXT context{};
						context.ContextFlags = CONTEXT_CONTROL;
#if defined(_M_X64)
						if (GetThreadContext(target, &context)) ++hits[context.Rip];
#else
						if (GetThreadContext(target, &context)) ++hits[context.Eip];
#endif
						ResumeThread(target);
					}
					Sleep(1);
				}
				timeEndPeriod(1);
			});
		}

		// Stops and prints the functions with the most samples.
		void Report(size_t count = 40) {
			if (target == nullptr) return;
			stop = true;
			thread.join();
			CloseHandle(target);
			target = nullptr;
			HANDLE const process = GetCurrentProcess();
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
			SymInitialize(process, nullptr, TRUE);
			std::map<std::string, uint64_t> functions;
			uint64_t total = 0;
			for (auto const &[address, samples]: hits) {
				alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
				auto *const symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
				symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen = 255;
				DWORD64 displacement = 0;
				functions[SymFromAddr(process, address, &displacement, symbol) ? symbol->Name : "?"] += samples;
				total += samples;
			}
			std::vector<std::pair<uint64_t, std::string>> sorted;
			for (auto const &[name, samples]: functions) sorted.emplace_back(samples, name);
			std::sort(sorted.rbegin(), sorted.rend());
			std::printf("samples: %llu\n", static_cast<unsigned long long>(total));
			for (size_t i = 0; i < sorted.size() && i < count; ++i) {
				std::printf("  %5.1f%%  %s\n", 100.0 * static_cast<double>(sorted[i].first) / static_cast<double>(total),
				            sorted[i].second.substr(0, 110).c_str());
			}
		}

	private:
		std::atomic<bool> stop{false};
		std::thread thread;
		HANDLE target = nullptr;
		std::map<uint64_t, uint64_t> hits;
	};
}
