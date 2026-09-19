/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Experimental game-side native shadow route.
//
// SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000:
//
//   AddModels                         0x00494390
//   AddModels power-pole case         0x00494458
//   CreateOccupantShadow              0x00491230
//   CreateOccupantShadow type gate    0x004913B1
//   c772 network case                 0x00494630
//
// The normal power-pole case in AddModels prepares the occupant with
// FUN_0048F4D0, calls CreateOccupantShadow, and then performs its normal
// state update. This experiment reuses only that native game path. It does
// not render a second shadow in the D3D11 driver.
//
// This is deliberately opt-in. The old AddShadow implementation projects a
// model's triangles into a terrain decal; it is correct for ground cards and
// can be visibly wrong for tall True3D geometry. Every write is guarded by
// the exact 1.1.641 bytes and can be restored during PreAppShutdown.

#include "NativeShadowExperiment.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include <windows.h>

namespace nSCD3D11::NativeShadowExperiment {
	namespace {

		constexpr uintptr_t kImageBase = 0x00400000;

		// Replace MOV EAX,[ESP+0x3C] + CMP EAX,0x6A11BD2B. The stub replays
		// the CMP and rejoins after it for every untouched notification.
		constexpr uintptr_t kAddModelsDispatchVA = 0x00494420;
		constexpr uintptr_t kAddModelsSwitchRejoinVA = 0x00494429;
		constexpr uintptr_t kAddModelsPowerPathVA = 0x00494458;
		constexpr uintptr_t kAddModelsFallThroughVA = 0x0049473C;
		constexpr uintptr_t kCreateOccupantShadowVA = 0x00491230;
		constexpr uintptr_t kCreateOccupantShadowTypeGateVA = 0x004913B1;
		constexpr uintptr_t kCreateOccupantShadowTypeRejoinVA = 0x004913B9;
		constexpr uintptr_t kPrepareModelVA = 0x0048F4D0;
		constexpr uintptr_t kC772NetworkShadowVA = 0x0049465A;
		constexpr uintptr_t kC772NetworkShadowRejoinVA = 0x00494660;

		// Class/type values seen in the Windows network factory and AddModels.
		constexpr uint32_t kPowerPoleType = 0x2890D4DE;
		constexpr uint32_t kPrebuiltNetworkClass = 0x49C1A034;
		constexpr uint32_t kBridgeNetworkClass = 0x49CC1BCD;
		constexpr uint32_t kTunnelNetworkClass = 0x08A4BD52;
		// cSC4NetworkOccupant's type value is present in the symbolized Mac
		// constructor and in the Windows AddModels dispatch. It is kept here
		// only as the explicit experimental network case; Windows runtime use
		// is still guarded by the surrounding byte checks.
		constexpr uint32_t kNetworkOccupantType = 0xC772BF98;

		constexpr size_t kMaxPatchLength = 16;

		enum class Mode {
			Off,
			Prebuilt,
			All,
		};

		struct PatchSite {
			uintptr_t virtualAddress = 0;
			size_t length = 0;
			std::array<uint8_t, kMaxPatchLength> expected{};
			std::array<uint8_t, kMaxPatchLength> replacement{};
			std::array<uint8_t, kMaxPatchLength> saved{};
			uint8_t *address = nullptr;
			bool installed = false;
		};

		PatchSite gAddModelsDispatch;
		PatchSite gTypeGate;
		PatchSite gC772NetworkShadow;
		PatchSite gPropQuality;
		PatchSite gPropGroundModel;
		PatchSite gPropMeshCount;
		PatchSite gPropHeight;

		Mode gMode = Mode::Off;
		bool gInstalled = false;

		// These are read by the x86 naked stubs below. They are initialized to
		// absolute addresses in the live SimCity 4 module, not link-time DLL VAs.
		uintptr_t gAddModelsPowerPath = 0;
		uintptr_t gAddModelsNetworkPath = 0;
		uintptr_t gAddModelsSwitchRejoin = 0;
		uintptr_t gAddModelsFallThrough = 0;
		uintptr_t gCreateOccupantShadow = 0;
		uintptr_t gPrepareModel = 0;
		uintptr_t gTypeGateRejoin = 0;
		uintptr_t gC772NetworkShadowRejoin = 0;

		// Set only while one of our network adapters is calling the native
		// shadow routine. This keeps the original type gate unchanged for all
		// ordinary CreateOccupantShadow callers.
		volatile uint32_t gAllowExperimentalNetworkType = 0;
		// In :all mode unknown AddModels notifications are treated as model
		// notifications after all known game cases have been excluded.
		volatile uint32_t gRouteUnknownNotifications = 0;

		void ConfigureSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                  std::initializer_list<uint8_t> replacement) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = expected.size();
			std::copy(expected.begin(), expected.end(), site.expected.begin());
			std::copy(replacement.begin(), replacement.end(), site.replacement.begin());
		}

		void ConfigureJumpSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                       uint8_t *target, void *destination) {
			ConfigureSite(site, virtualAddress, expected, {});
			site.replacement.fill(0x90);
			site.replacement[0] = 0xE9;
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
			                                                   (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(site.replacement.data() + 1, &displacement, sizeof(displacement));
		}

		bool IsSupportedGameVersion() {
			wchar_t path[MAX_PATH]{};
			if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
			DWORD ignored = 0;
			DWORD const size = GetFileVersionInfoSizeW(path, &ignored);
			if (size == 0) return false;
			std::vector<uint8_t> data(size);
			if (!GetFileVersionInfoW(path, 0, size, data.data())) return false;
			VS_FIXEDFILEINFO *version = nullptr;
			UINT versionSize = 0;
			if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&version), &versionSize) ||
			    version == nullptr || versionSize < sizeof(VS_FIXEDFILEINFO))
				return false;
			return HIWORD(version->dwFileVersionMS) == 1 && LOWORD(version->dwFileVersionMS) == 1 &&
			       HIWORD(version->dwFileVersionLS) == 641;
		}

		Mode ReadMode() {
			char const *const commandLine = GetCommandLineA();
			char const *argument = std::strstr(commandLine, "-NativeShadowExperiment:");
			if (argument == nullptr) return Mode::Off;
			argument += sizeof("-NativeShadowExperiment:") - 1;
			auto matches = [](char const *value, char const *expected) {
				size_t const length = std::strlen(expected);
				return _strnicmp(value, expected, length) == 0 &&
				       (value[length] == '\0' || value[length] == ' ' || value[length] == '\t');
			};
			if (matches(argument, "all")) return Mode::All;
			if (matches(argument, "prebuilt") || matches(argument, "network"))
				return Mode::Prebuilt;
			return Mode::Off;
		}

		bool BytesMatch(PatchSite const &site) {
			return site.address != nullptr && std::memcmp(site.address, site.expected.data(), site.length) == 0;
		}

		bool WriteSite(PatchSite &site) {
			DWORD oldProtection = 0;
			if (!VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
			std::memcpy(site.saved.data(), site.address, site.length);
			std::memcpy(site.address, site.replacement.data(), site.length);
			DWORD ignored = 0;
			VirtualProtect(site.address, site.length, oldProtection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
			site.installed = true;
			return true;
		}

		void RestoreSite(PatchSite &site) {
			if (!site.installed || site.address == nullptr) return;
			// Do not overwrite a later patch that replaced this experiment while
			// the process was running.
			if (std::memcmp(site.address, site.replacement.data(), site.length) != 0) {
				Log(LogCategory::Initialization,
				    "native shadow experiment: not restoring modified site 0x%08lX",
				    static_cast<unsigned long>(site.virtualAddress));
				site.installed = false;
				return;
			}
			DWORD oldProtection = 0;
			if (VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &oldProtection)) {
				std::memcpy(site.address, site.saved.data(), site.length);
				DWORD ignored = 0;
				VirtualProtect(site.address, site.length, oldProtection, &ignored);
				FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
			}
			site.installed = false;
		}

		void RestoreAllSites() {
			RestoreSite(gPropHeight);
			RestoreSite(gPropMeshCount);
			RestoreSite(gPropGroundModel);
			RestoreSite(gPropQuality);
			RestoreSite(gC772NetworkShadow);
			RestoreSite(gTypeGate);
			RestoreSite(gAddModelsDispatch);
		}

		// AddModels reaches this site before the notification switch. The
		// original instructions load EAX from [ESP+0x3C] and compare it with
		// 0x6A11BD2B. The stub replays that compare for ordinary notifications,
		// while sending network class values through the native adapter.
		__declspec(naked) void AddModelsDispatchStub() {
			__asm {
				mov  eax, dword ptr [esp + 0x3c]
				cmp  eax, 0x2890d4de
				je   power_path
				cmp  eax, 0x49c1a034
				je   network_path
				cmp  eax, 0x49cc1bcd
				je   network_path
				cmp  eax, 0x08a4bd52
				je   network_path
				// The c772 case has existing bookkeeping and is augmented at
				// 0x0049465A instead of being replaced here.
				cmp  eax, 0xc772bf98
				je   original_path
				cmp  eax, 0x6a11bd2b
				je   original_path
				cmp  eax, 0x2a183167
				je   original_path
				cmp  eax, 0x278128a0
				je   original_path
				cmp  eax, 0x497f6d9d
				je   original_path
				cmp  eax, 0x68fd0c69
				je   original_path
				cmp  eax, 0x8a13918a
				je   original_path
				cmp  eax, 0x74758926
				je   original_path
				cmp  eax, 0x895d1169
				je   original_path
				cmp  eax, 0xa823821e
				je   original_path
				cmp  dword ptr [gRouteUnknownNotifications], 0
				jne  network_path
				cmp  eax, 0x6a11bd2b
				jmp  dword ptr [gAddModelsSwitchRejoin]
			power_path:
				jmp  dword ptr [gAddModelsPowerPath]
			network_path:
				jmp  dword ptr [gAddModelsNetworkPath]
			original_path:
				cmp  eax, 0x6a11bd2b
				jmp  dword ptr [gAddModelsSwitchRejoin]
			}
		}

		// Network class notifications do not have an existing AddModels case.
		// Reproduce the power-pole preparation and shadow call, but do not emit
		// the power-pole-specific ApplyLotStateEffect notification.
		__declspec(naked) void NetworkAddModelsPathStub() {
			__asm {
				push ebx
				push ebp
				push esi
				mov  ecx, edi
				call dword ptr [gPrepareModel]
				mov  dword ptr [gAllowExperimentalNetworkType], 1
				push ebx
				push ebp
				push esi
				mov  ecx, edi
				call dword ptr [gCreateOccupantShadow]
				mov  dword ptr [gAllowExperimentalNetworkType], 0
				jmp  dword ptr [gAddModelsFallThrough]
			}
		}

		// The cSC4NetworkOccupant case already performs its own state update.
		// Inject the same native shadow call immediately before its existing
		// resource/query continuation, while EBX/EBP still contain the original
		// model arguments. The first six bytes at the site are replayed here.
		__declspec(naked) void C772NetworkShadowStub() {
			__asm {
				push ebx
				push ebp
				push esi
				mov  ecx, edi
				mov  dword ptr [gAllowExperimentalNetworkType], 1
				call dword ptr [gCreateOccupantShadow]
				mov  dword ptr [gAllowExperimentalNetworkType], 0
				mov  ebx, dword ptr [esi]
				lea  ecx, [esp + 0x38]
				jmp  dword ptr [gC772NetworkShadowRejoin]
			}
		}

		// The original gate has just called occupant->GetType() and EAX holds
		// the result. Power poles retain their exact original acceptance rule;
		// our adapter sets the scoped flag around network calls.
		__declspec(naked) void ShadowTypeGateStub() {
			__asm {
				cmp  eax, 0x2890d4de
				je   accept_type
				cmp  dword ptr [gAllowExperimentalNetworkType], 0
				je   reject_type
				test eax, eax
			jz   reject_type
			accept_type:
				mov  eax, 1
				jmp  dword ptr [gTypeGateRejoin]
			reject_type:
				xor  eax, eax
				jmp  dword ptr [gTypeGateRejoin]
			}
		}

		void ConfigureSites(uint8_t *module) {
			gAddModelsPowerPath = reinterpret_cast<uintptr_t>(module + (kAddModelsPowerPathVA - kImageBase));
			gAddModelsNetworkPath = reinterpret_cast<uintptr_t>(&NetworkAddModelsPathStub);
			gAddModelsSwitchRejoin = reinterpret_cast<uintptr_t>(module + (kAddModelsSwitchRejoinVA - kImageBase));
			gAddModelsFallThrough = reinterpret_cast<uintptr_t>(module + (kAddModelsFallThroughVA - kImageBase));
			gCreateOccupantShadow = reinterpret_cast<uintptr_t>(module + (kCreateOccupantShadowVA - kImageBase));
			gPrepareModel = reinterpret_cast<uintptr_t>(module + (kPrepareModelVA - kImageBase));
			gTypeGateRejoin = reinterpret_cast<uintptr_t>(module + (kCreateOccupantShadowTypeRejoinVA - kImageBase));
			gC772NetworkShadowRejoin = reinterpret_cast<uintptr_t>(module + (kC772NetworkShadowRejoinVA - kImageBase));

			ConfigureJumpSite(
				gAddModelsDispatch, kAddModelsDispatchVA,
				{0x8B, 0x44, 0x24, 0x3C, 0x3D, 0x2B, 0xBD, 0x11, 0x6A},
				module + (kAddModelsDispatchVA - kImageBase), &AddModelsDispatchStub);
			ConfigureJumpSite(
				gTypeGate, kCreateOccupantShadowTypeGateVA,
				{0x3D, 0xDE, 0xD4, 0x90, 0x28, 0x0F, 0x94},
				module + (kCreateOccupantShadowTypeGateVA - kImageBase), &ShadowTypeGateStub);
			ConfigureJumpSite(
				gC772NetworkShadow, kC772NetworkShadowVA,
				{0x8B, 0x1E, 0x8D, 0x4C, 0x24, 0x38},
				module + (kC772NetworkShadowVA - kImageBase), &C772NetworkShadowStub);

			ConfigureSite(gPropQuality, 0x00491333, {0x7D, 0x16}, {0xEB, 0x16});
			ConfigureSite(gPropGroundModel, 0x0049134B, {0x8D, 0x4C, 0x24, 0x13}, {0xEB, 0x1D, 0x90, 0x90});
			ConfigureSite(gPropMeshCount, 0x00491406, {0x0F, 0x9D, 0x44, 0x24, 0x13},
			              {0xC6, 0x44, 0x24, 0x13, 0x01});
			ConfigureSite(gPropHeight, 0x0049156B, {0x7A, 0x32}, {0xEB, 0x32});

			PatchSite *const sites[] = {
				&gAddModelsDispatch, &gTypeGate, &gC772NetworkShadow,
				&gPropQuality, &gPropGroundModel, &gPropMeshCount, &gPropHeight,
			};
			for (PatchSite *site: sites) {
				site->address = module + (site->virtualAddress - kImageBase);
			}
		}

	} // namespace

	bool Install() {
		if (gInstalled) return true;
		Mode const mode = ReadMode();
		if (mode == Mode::Off) return false;
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization,
			    "native shadow experiment: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		uint8_t *const module = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		ConfigureSites(module);

		PatchSite *const baseSites[] = {&gAddModelsDispatch, &gTypeGate, &gC772NetworkShadow};
		PatchSite *const allSites[] = {
			&gAddModelsDispatch, &gTypeGate, &gC772NetworkShadow,
			&gPropQuality, &gPropGroundModel, &gPropMeshCount, &gPropHeight,
		};
		PatchSite *const *const sites = mode == Mode::All ? allSites : baseSites;
		size_t const siteCount = mode == Mode::All ? std::size(allSites) : std::size(baseSites);

		for (size_t i = 0; i < siteCount; ++i) {
			if (!BytesMatch(*sites[i])) {
				Log(LogCategory::Initialization,
				    "native shadow experiment: byte guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(sites[i]->virtualAddress));
				return false;
			}
		}

		gMode = mode;
		gAllowExperimentalNetworkType = 0;
		gRouteUnknownNotifications = mode == Mode::All ? 1U : 0U;
		for (size_t i = 0; i < siteCount; ++i) {
			if (!WriteSite(*sites[i])) {
				Log(LogCategory::Initialization,
				    "native shadow experiment: failed to write site 0x%08lX, restoring",
				    static_cast<unsigned long>(sites[i]->virtualAddress));
				RestoreAllSites();
				gMode = Mode::Off;
				gRouteUnknownNotifications = 0;
				return false;
			}
		}

		gInstalled = true;
		Log(LogCategory::Initialization,
		    "native shadow experiment installed (%s): AddModels network adapter + CreateOccupantShadow",
		    mode == Mode::All ? "all" : "prebuilt/network");
		if (mode == Mode::All) {
			Log(LogCategory::Initialization,
			    "native shadow experiment: prop quality/ground-model/mesh/height gates are relaxed; legacy decal artifacts expected");
		}
		return true;
	}

	void Uninstall() {
		if (!gInstalled) return;
		gAllowExperimentalNetworkType = 0;
		gRouteUnknownNotifications = 0;
		RestoreAllSites();
		gMode = Mode::Off;
		gInstalled = false;
		Log(LogCategory::Initialization, "native shadow experiment uninstalled");
	}

} // namespace nSCD3D11::NativeShadowExperiment
