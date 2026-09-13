/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Clamp cSC4Simulator::OnTick's per-tick busy-loop budget.
//
// OnTick (VA 0x00703E70) computes the budget as 1000/speedDivisor - elapsed,
// floors it at 15 ms, stores it at [this+0x70], then runs
//   do { SimulateTimePassage; SimulateAgents; SimulateIdleTime; }
//   while (cRZTimer::GetElapsedTime(this+0x190) < [this+0x70]);
//
// The floor-clamp + store live in 13 bytes at 0x00703EF1:
//   83 F8 0F           cmp eax, 0x0F
//   7F 05              jg  0x00703EFB
//   B8 0F 00 00 00     mov eax, 0x0F
//   89 46 70           mov [esi+0x70], eax
// We overwrite those with a jump to a stub that also applies an upper bound,
// then rejoins at 0x00703EFE.
//
// Target: SimCity 4 Deluxe 1.1.641 (Windows), image base 0x00400000.

#include "SimTickBudget.h"

#include "Diagnostics.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string.h>
#include <vector>

#include <windows.h>

namespace nSCD3D11::SimTickBudget {
	namespace {

		constexpr uintptr_t kImageBase = 0x00400000;
		constexpr uintptr_t kPatchVA = 0x00703EF1;   // cmp eax, 0x0F ...
		constexpr uintptr_t kRejoinVA = 0x00703EFE;  // mov al, [esi+0x24]
		constexpr size_t kPatchLen = 13;
		constexpr size_t kJumpSize = 5;

		// Exact bytes we replace, guarding against a different build / other hook.
		const uint8_t kExpected[kPatchLen] = {
			0x83, 0xF8, 0x0F, 0x7F, 0x05, 0xB8, 0x0F, 0x00, 0x00, 0x00, 0x89, 0x46, 0x70};

		constexpr int kDefaultCapMs = 32;
		constexpr int kFloorMs = 15;
		constexpr int kMaxCapMs = 500;

		int gCapMs = kDefaultCapMs;      // read by the stub
		uintptr_t gRejoin = 0;           // read by the stub

		uint8_t *gTarget = nullptr;
		uint8_t gSaved[kPatchLen]{};
		bool gInstalled = false;

		// EAX = raw budget, ESI = this. Clamp to [kFloorMs, gCapMs], store, rejoin.
		// ECX/EDX are dead here (IDIV leftovers).
		__declspec(naked) void BudgetStub() {
			__asm {
				cmp  eax, 15            // kFloorMs
				jge  above_floor
				mov  eax, 15
			above_floor:
				mov  ecx, dword ptr [gCapMs]
				cmp  eax, ecx
				jle  do_store
				mov  eax, ecx
			do_store:
				mov  dword ptr [esi + 0x70], eax
				jmp  dword ptr [gRejoin]
			}
		}

		bool IsSupportedGameVersion() {
			wchar_t path[MAX_PATH]{};
			if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
			DWORD ignored = 0;
			DWORD size = GetFileVersionInfoSizeW(path, &ignored);
			if (size == 0) return false;
			std::vector<uint8_t> data(size);
			if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
			VS_FIXEDFILEINFO *v = nullptr;
			UINT vs = 0;
			if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&v), &vs) ||
			    v == nullptr || vs < sizeof(VS_FIXEDFILEINFO))
				return false;
			return HIWORD(v->dwFileVersionMS) == 1 && LOWORD(v->dwFileVersionMS) == 1 &&
			       HIWORD(v->dwFileVersionLS) == 641;
		}

		// Returns the requested cap, or -1 to skip installation.
		int ReadCap() {
			const char *arg = std::strstr(GetCommandLineA(), "-SimTickCap:");
			if (arg == nullptr) return kDefaultCapMs;
			arg += sizeof("-SimTickCap:") - 1;
			if (_strnicmp(arg, "off", 3) == 0) return -1;
			int v = std::atoi(arg);
			if (v == 0) return -1;
			if (v < kFloorMs) v = kFloorMs;
			if (v > kMaxCapMs) v = kMaxCapMs;
			return v;
		}

	} // namespace

	bool Install() {
		if (gInstalled) return true;

		int cap = ReadCap();
		if (cap < 0) {
			Log(LogCategory::Initialization, "sim tick budget: disabled via -SimTickCap:off");
			return false;
		}
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization, "sim tick budget: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		auto module = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		gTarget = module + (kPatchVA - kImageBase);
		gRejoin = reinterpret_cast<uintptr_t>(module + (kRejoinVA - kImageBase));
		gCapMs = cap;

		if (std::memcmp(gTarget, kExpected, kPatchLen) != 0) {
			Log(LogCategory::Initialization, "sim tick budget: cSC4Simulator::OnTick already modified, skipping");
			gTarget = nullptr;
			return false;
		}

		DWORD prot = 0;
		if (!VirtualProtect(gTarget, kPatchLen, PAGE_EXECUTE_READWRITE, &prot)) {
			gTarget = nullptr;
			return false;
		}
		std::memcpy(gSaved, gTarget, kPatchLen);
		gTarget[0] = 0xE9;
		int32_t const disp = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&BudgetStub) -
		                                          (reinterpret_cast<uintptr_t>(gTarget) + kJumpSize));
		std::memcpy(gTarget + 1, &disp, sizeof(disp));
		std::memset(gTarget + kJumpSize, 0x90, kPatchLen - kJumpSize);
		DWORD ignored = 0;
		VirtualProtect(gTarget, kPatchLen, prot, &ignored);
		FlushInstructionCache(GetCurrentProcess(), gTarget, kPatchLen);

		gInstalled = true;
		Log(LogCategory::Initialization, "sim tick budget installed: cap=%d ms", gCapMs);
		return true;
	}

	void Uninstall() {
		if (!gInstalled) return;
		DWORD prot = 0;
		if (VirtualProtect(gTarget, kPatchLen, PAGE_EXECUTE_READWRITE, &prot)) {
			std::memcpy(gTarget, gSaved, kPatchLen);
			DWORD ignored = 0;
			VirtualProtect(gTarget, kPatchLen, prot, &ignored);
			FlushInstructionCache(GetCurrentProcess(), gTarget, kPatchLen);
		}
		gInstalled = false;
		Log(LogCategory::Initialization, "sim tick budget uninstalled");
	}

} // namespace nSCD3D11::SimTickBudget
