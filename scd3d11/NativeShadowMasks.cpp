/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Native shadows for prebuilt network pieces and True3D props.
//
// SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000. Opt-in with
// -NativeShadowMasks:network, :props or :all. Every site is guarded by its
// exact bytes and restored during PreAppShutdown.
//
// What the Phase 1 instrumentation established, and why the patches are shaped
// this way (see docs/true3d-shadows-phase1.md):
//
//  * A prebuilt network piece casts a native shadow if and only if an FSH exists at
//    {0x7AB50E44, 0x2BC2759A, base + zoom}. Blanking those masks removed every
//    network shadow in the city, so the decal, its terrain following and its
//    lifetime need no patching at all. Nothing here creates decals.
//  * What does block pieces is the gate at 0x0061E35C: UpdateShadow requires
//    HasNetworkFlag(1) && HasNetworkFlag(4) before it will even look a texture
//    up. Pieces that fail it log `decal0 = -1 -> -1` and never reach the
//    mapping. That gate is the one thing worth relaxing, and it is site A.
//  * Relaxing it alone would ask for masks that may not exist, so site B takes
//    the mapped instance and, for a piece that only got through because of the
//    relaxation, returns 0 when no mask is available. A zero return is the
//    game's own "no shadow texture" path at 0x0061E5A0, so a missing mask falls
//    back to exactly the original behaviour.
// Verified sites:
//
//   A 0x0061E35C  35 bytes  the HasNetworkFlag(1)/(4)/quality gate
//   B 0x0061E589   7 bytes  TEST EAX,EAX / MOV ECX,-2 after MapShadowTexture
//   C 0x0049134B   5 bytes  the Is Ground Model property test
//   D 0x0049195E   6 bytes  the cSTEOverlayManager::AddShadow call
//
// Prebuilt-network Draw (site E) and the rejected True3D prop path identify the
// corresponding cGDriver draws. The driver still has the index buffer and can
// therefore project the actual alpha-tested triangles. Existing network mask
// decals are retained for non-prebuilt/fallback geometry.
// Sites C and D are shared with the diagnostics and research modules. This
// module installs first; their byte guards make them skip rather than overlap.
//
//   E 0x0061E860   5 bytes  cSC4NetworkOccupantWithPreBuiltModel::Draw wrapper

#include "NativeShadowMasks.h"

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <vector>

#include <windows.h>

namespace nSCD3D11::NativeShadowMasks {
	namespace {

		constexpr uintptr_t kImageBase = 0x00400000;

		constexpr uintptr_t kNetworkGateVA = 0x0061E35C;
		constexpr uintptr_t kNetworkGateAcceptVA = 0x0061E37F; // MOV BL,1
		constexpr uintptr_t kNetworkGateRejectVA = 0x0061E383; // XOR BL,BL
		constexpr uintptr_t kMapResultVA = 0x0061E589;
		constexpr uintptr_t kMapResultRejoinVA = 0x0061E590;
		constexpr uintptr_t kPrebuiltDrawVA = 0x0061E860;
		constexpr uintptr_t kPrebuiltDrawRejoinVA = 0x0061E865;
		constexpr uintptr_t kRenderPropertiesPointerVA = 0x00B43CC4;
		constexpr uintptr_t kPropGroundModelVA = 0x0049134B;
		constexpr uintptr_t kPropGroundModelRejoinVA = 0x0049136A;
		constexpr uintptr_t kGetBoolPropertyVA = 0x005FD390;
		constexpr uintptr_t kAddShadowSiteVA = 0x0049195E;
		constexpr uintptr_t kAddShadowRejoinVA = 0x00491964;
		constexpr ptrdiff_t kFrameOccupant = 0xE0;
		constexpr ptrdiff_t kAddShadowArgumentBytes = 0x14;
		// HasNetworkFlag on the base subobject at this+4.
		constexpr size_t kHasNetworkFlagSlot = 0x80 / sizeof(void *);

		constexpr size_t kMaxPatchLength = 40;

		enum class Mode { Off, Network, Props, All };

		struct PatchSite {
			uintptr_t virtualAddress = 0;
			size_t length = 0;
			std::array<uint8_t, kMaxPatchLength> expected{};
			std::array<uint8_t, kMaxPatchLength> replacement{};
			std::array<uint8_t, kMaxPatchLength> saved{};
			uint8_t *address = nullptr;
			bool installed = false;
		};

		PatchSite gNetworkGate;
		PatchSite gMapResult;
		PatchSite gPrebuiltDraw;
		PatchSite gPropGroundModel;
		PatchSite gAddShadowSite;

		Mode gMode = Mode::Off;
		bool gInstalled = false;

		uintptr_t gNetworkGateAccept = 0;
		uintptr_t gNetworkGateReject = 0;
		uintptr_t gMapResultRejoin = 0;
		uintptr_t gPrebuiltDrawRejoin = 0;
		uintptr_t gRenderPropertiesPointer = 0;
		uintptr_t gPropGroundModelRejoin = 0;
		uintptr_t gAddShadowRejoin = 0;
		uintptr_t gGetBoolProperty = 0;

		// Available mask instances, sorted for binary search, plus explicit
		// source -> generated remappings. Loaded once; never touched during a
		// frame, so lookups need no lock.
		std::vector<uint32_t> gMaskInstances;
		std::vector<std::pair<uint32_t, uint32_t>> gMaskRemaps;

		// Set by the gate stub, read by the mapping stub a few calls later on
		// the same thread inside the same UpdateShadow.
		thread_local bool tGatePassedOriginally = false;
		thread_local uint32_t tLiveNetworkDrawDepth = 0;
		thread_local void *tRelaxedPropOccupant = nullptr;

		std::mutex gPropMutex;
		struct LivePropMesh {
			uint64_t signature = 0;
		};
		std::vector<LivePropMesh> gLivePropMeshes;

		unsigned gNetworkGateRelaxed = 0;
		unsigned gNetworkMaskSupplied = 0;
		unsigned gNetworkFellBack = 0;
		unsigned gLiveNetworkDraws = 0;
		unsigned gPropMeshesRegistered = 0;
		unsigned gPropCallsSuppressed = 0;

		// ------------------------------------------------------------------
		// Guarded reads
		// ------------------------------------------------------------------

#if defined(_MSC_VER)
		bool SafeCopy(void const *source, void *destination, size_t size) {
			__try {
				std::memcpy(destination, source, size);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeHasNetworkFlag(void *flags, uint32_t bit, bool &result) {
			__try {
				void **const vtable = *reinterpret_cast<void ***>(flags);
				using Query = uint8_t(__thiscall *)(void *, uint32_t);
				result = reinterpret_cast<Query>(vtable[kHasNetworkFlagSlot])(flags, bit) != 0;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeCopy(void const *, void *, size_t) { return false; }
		bool SafeHasNetworkFlag(void *, uint32_t, bool &) { return false; }
#endif

		bool HasGeneratedMask(uint32_t instance) {
			return std::binary_search(gMaskInstances.begin(), gMaskInstances.end(), instance);
		}

		uint32_t RemapInstance(uint32_t instance) {
			for (auto const &entry: gMaskRemaps) {
				if (entry.first == instance) return entry.second;
			}
			return 0;
		}

		void HashWord(uint64_t &hash, uint32_t word) {
			hash = (hash ^ word) * 0x100000001B3ull;
		}

		bool LiveShadowDiagnosticsEnabled() {
			static bool const enabled = std::strstr(GetCommandLineA(), "-LiveShadowDiag") != nullptr;
			return enabled;
		}

		// ------------------------------------------------------------------
		// Manifest
		// ------------------------------------------------------------------

		std::filesystem::path ManifestPath() {
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH]{};
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        reinterpret_cast<LPCWSTR>(&ManifestPath), &module) ||
			    GetModuleFileNameW(module, path, MAX_PATH) == 0) {
				return std::filesystem::path(L"SC4ShadowMasks.txt");
			}
			return std::filesystem::path(path).parent_path() / L"SC4ShadowMasks.txt";
		}

		// One available hex instance per line, or "source -> generated" to point
		// a piece at a mask baked under a different id. Blank lines and # are ignored.
		void LoadManifest() {
			gMaskInstances.clear();
			gMaskRemaps.clear();
			std::filesystem::path const path = ManifestPath();
			FILE *file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"r") != 0 || file == nullptr) {
				Log(LogCategory::Initialization,
				    "native shadow masks: no manifest at the DLL folder, relaxed pieces will fall back");
				return;
			}
			char line[256]{};
			while (std::fgets(line, sizeof(line), file) != nullptr) {
				char *cursor = line;
				while (*cursor == ' ' || *cursor == '\t') ++cursor;
				if (*cursor == '#' || *cursor == '\0' || *cursor == '\n' || *cursor == '\r') continue;
				// Old experimental prop-proxy records are ignored. Geometry-only
				// proxies are known to generate solid slabs and are no longer a
				// supported runtime mode.
				if (*cursor == 'P' || *cursor == 'p') continue;
				char *end = nullptr;
				unsigned long const source = std::strtoul(cursor, &end, 16);
				if (end == cursor) continue;
				while (*end == ' ' || *end == '\t') ++end;
				if (end[0] == '-' && end[1] == '>') {
					end += 2;
					char *target = nullptr;
					unsigned long const generated = std::strtoul(end, &target, 16);
					if (target != end) {
						gMaskRemaps.emplace_back(static_cast<uint32_t>(source),
						                         static_cast<uint32_t>(generated));
						gMaskInstances.push_back(static_cast<uint32_t>(generated));
						continue;
					}
				}
				gMaskInstances.push_back(static_cast<uint32_t>(source));
			}
			std::fclose(file);
			std::sort(gMaskInstances.begin(), gMaskInstances.end());
			gMaskInstances.erase(std::unique(gMaskInstances.begin(), gMaskInstances.end()),
			                     gMaskInstances.end());
			Log(LogCategory::Initialization,
			    "native shadow masks: manifest has %u instances and %u remappings",
			    static_cast<unsigned>(gMaskInstances.size()), static_cast<unsigned>(gMaskRemaps.size()));
		}

	} // namespace

	// ----------------------------------------------------------------------
	// Hook bodies
	// ----------------------------------------------------------------------

	// Replaces the whole HasNetworkFlag(1) && HasNetworkFlag(4) && quality > 2
	// gate. Returns the value the original would have produced unless a mask
	// manifest is loaded, in which case the two network-type bits stop being a
	// reason to skip the piece. ShadowQuality is never relaxed, so quality 0-2
	// behaves exactly as before.
	extern "C" uint32_t __cdecl SCD3D11_MaskNetworkGate(void *occupant, void *flags, int32_t quality) {
		(void) occupant;
		bool flagOne = false;
		bool flagFour = false;
		bool const readable =
			SafeHasNetworkFlag(flags, 1, flagOne) && SafeHasNetworkFlag(flags, 4, flagFour);
		bool const original = readable && flagOne && flagFour && quality > 2;
		tGatePassedOriginally = original;
		if (original) return 1;
		if (quality <= 2 || gMaskInstances.empty()) return 0;
		++gNetworkGateRelaxed;
		return 1;
	}

	// Runs on MapShadowTexture's return value. A piece that would have passed
	// the original gate keeps whatever the game mapped. A piece that is only
	// here because the gate was relaxed needs a mask we generated; without one
	// it returns 0, which is the game's own abort at 0x0061E5A0.
	extern "C" uint32_t __cdecl SCD3D11_MaskShadowTexture(uint32_t instance) {
		if (instance == 0) return 0;
		if (uint32_t const remapped = RemapInstance(instance); remapped != 0) {
			++gNetworkMaskSupplied;
			return remapped;
		}
		if (HasGeneratedMask(instance)) {
			++gNetworkMaskSupplied;
			return instance;
		}
		if (tGatePassedOriginally) return instance;
		++gNetworkFellBack;
		return 0;
	}

	extern "C" void __cdecl SCD3D11_MaskNoteRelaxedProp(void *occupant) {
		tRelaxedPropOccupant = occupant;
	}

	// The rejected prop has reached the point where SC4 extracted its position
	// and UV streams. Record a stable mesh signature for the indexed renderer,
	// then suppress AddShadow: its one affine projector is exactly the path that
	// produced the giant filled slabs.
	extern "C" uint32_t __cdecl SCD3D11_RegisterLiveProp(uint32_t *stackArguments) {
		uint8_t const *const frameBase =
			reinterpret_cast<uint8_t const *>(stackArguments) + kAddShadowArgumentBytes;
		uint32_t occupant = 0;
		if (!SafeCopy(frameBase + kFrameOccupant, &occupant, sizeof(occupant)) || occupant == 0 ||
		    reinterpret_cast<void *>(occupant) != tRelaxedPropOccupant) return 0;

		struct Vector3 { float x, y, z; };
		struct Vector2 { float u, v; };
		uint32_t const count = stackArguments[0];
		auto const *const positions = reinterpret_cast<Vector3 const *>(stackArguments[1]);
		auto const *const uvs = reinterpret_cast<Vector2 const *>(stackArguments[2]);
		if (count < 3 || count > 0x100000 || positions == nullptr || uvs == nullptr) return 2;
		uint64_t signature = 0xCBF29CE484222325ull;
		HashWord(signature, count);
		for (uint32_t index = 0; index < count; ++index) {
			Vector3 position{};
			Vector2 uv{};
			if (!SafeCopy(positions + index, &position, sizeof(position)) ||
			    !SafeCopy(uvs + index, &uv, sizeof(uv))) return 2;
			uint32_t words[5]{};
			std::memcpy(words, &position, sizeof(position));
			std::memcpy(words + 3, &uv, sizeof(uv));
			for (uint32_t word: words) HashWord(signature, word);
		}
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			auto const found = std::lower_bound(
				gLivePropMeshes.begin(), gLivePropMeshes.end(), signature,
				[](LivePropMesh const &mesh, uint64_t value) { return mesh.signature < value; });
			bool const inserted = found == gLivePropMeshes.end() || found->signature != signature;
			if (inserted) {
				LivePropMesh mesh{};
				mesh.signature = signature;
				gLivePropMeshes.insert(found, std::move(mesh));
				++gPropMeshesRegistered;
				Log(LogCategory::Initialization,
				    "native shadows: registered live prop mesh sig=%08X%08X verts=%u",
				    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature), count);
			}
			if (!inserted && LiveShadowDiagnosticsEnabled()) {
				Log(LogCategory::Initialization,
				    "liveshadow-reg reused sig=%08X%08X verts=%u",
				    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature), count);
			}
		}
		++gPropCallsSuppressed;
		return 2;
	}

	bool LivePropsEnabled() {
		return gInstalled && (gMode == Mode::Props || gMode == Mode::All);
	}

	bool LiveNetworkEnabled() {
		return gInstalled && (gMode == Mode::Network || gMode == Mode::All);
	}

	bool LiveNetworkDrawActive() {
		return LiveNetworkEnabled() && tLiveNetworkDrawDepth != 0;
	}

	bool RequiresCleanTranslatedRedraw() {
		return gInstalled && gMode != Mode::Off;
	}

	bool HasLivePropMeshes() {
		if (!LivePropsEnabled()) return false;
		std::lock_guard<std::mutex> lock(gPropMutex);
		return !gLivePropMeshes.empty();
	}

	bool MatchLivePropSignature(uint64_t signature) {
		if (!LivePropsEnabled()) return false;
		std::lock_guard<std::mutex> lock(gPropMutex);
		auto const found = std::lower_bound(
			gLivePropMeshes.begin(), gLivePropMeshes.end(), signature,
			[](LivePropMesh const &mesh, uint64_t value) { return mesh.signature < value; });
		return found != gLivePropMeshes.end() && found->signature == signature;
	}

	namespace {

#if defined(_MSC_VER) && defined(_M_IX86)

		// Windows 1.1.641 0x0061E860 is the two-argument __thiscall wrapper used
		// to draw cSC4NetworkOccupantWithPreBuiltModel. It refreshes UpdateShadow,
		// then calls the ordinary network-occupant renderer. Bracketing that call
		// lets cGDriver recognize its indexed S3D draws without admitting terrain,
		// water, UI or unrelated overlays into the live shadow map.
		__declspec(naked) void PrebuiltDrawOriginal() {
			__asm {
				push edx
				mov  edx, dword ptr [gRenderPropertiesPointer]
				mov  eax, dword ptr [edx]
				pop  edx
				jmp  dword ptr [gPrebuiltDrawRejoin]
			}
		}

		uint32_t __fastcall PrebuiltDrawLiveShadowHook(
			void *self, void *, void *drawContext, void *drawState) {
			using Original = uint32_t (__thiscall *)(void *, void *, void *);
			++tLiveNetworkDrawDepth;
			uint32_t const result =
				reinterpret_cast<Original>(&PrebuiltDrawOriginal)(self, drawContext, drawState);
			--tLiveNetworkDrawDepth;
			++gLiveNetworkDraws;
			return result;
		}

		// ESI is the occupant, EDI the flag subobject at ESI+4 and EBX holds
		// ShadowQuality. Rejoins at MOV BL,1 or XOR BL,BL, so the original
		// register contract is preserved either way.
		__declspec(naked) void NetworkGateStub() {
			__asm {
				push ecx
				push edx
				push edi
				push ebx
				push edi
				push esi
				call SCD3D11_MaskNetworkGate
				add  esp, 0xc
				pop  edi
				pop  edx
				pop  ecx
				test al, al
				jz   reject
				jmp  dword ptr [gNetworkGateAccept]
			reject:
				jmp  dword ptr [gNetworkGateReject]
			}
		}

		// EAX holds the mapped instance. The replayed TEST/MOV reproduce the
		// two displaced instructions, and the JZ at 0x0061E5A0 still reads the
		// flags the TEST sets.
		__declspec(naked) void MapResultStub() {
			__asm {
				push ecx
				push edx
				push eax
				call SCD3D11_MaskShadowTexture
				add  esp, 4
				pop  edx
				pop  ecx
				test eax, eax
				mov  ecx, 0xfffffffe
				jmp  dword ptr [gMapResultRejoin]
			}
		}

		__declspec(naked) void PropGroundModelStub() {
			__asm {
				push eax
				push ecx
				push edx
				push 0
				call SCD3D11_MaskNoteRelaxedProp
				add  esp, 4
				pop  edx
				pop  ecx
				pop  eax
				lea  ecx, [esp + 0x13]
				push ecx
				push 0x8a5e5db8
				push ebp
				call dword ptr [gGetBoolProperty]
				add  esp, 0xc
				test al, al
				jz   relaxed
				mov  al, byte ptr [esp + 0x13]
				test al, al
				jz   relaxed
				jmp  dword ptr [gPropGroundModelRejoin]
			relaxed:
				push eax
				push ecx
				push edx
				push esi
				call SCD3D11_MaskNoteRelaxedProp
				add  esp, 4
				pop  edx
				pop  ecx
				pop  eax
				jmp  dword ptr [gPropGroundModelRejoin]
			}
		}

		__declspec(naked) void AddShadowLivePropStub() {
			__asm {
				pushad
				pushfd
				lea  eax, [esp + 0x24]
				push eax
				call SCD3D11_RegisterLiveProp
				add  esp, 4
				mov  dword ptr [esp + 0x20], eax
				popfd
				popad
				cmp  eax, 2
				je   suppress
				mov  ecx, ebp
				push edi
				call dword ptr [edx + 0x2c]
				jmp  dword ptr [gAddShadowRejoin]
			suppress:
				add  esp, 0x14
				or   eax, 0xffffffff
				jmp  dword ptr [gAddShadowRejoin]
			}
		}

#endif // _MSC_VER && _M_IX86

		// ------------------------------------------------------------------
		// Patch plumbing
		// ------------------------------------------------------------------

		void ConfigureSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = expected.size();
			std::copy(expected.begin(), expected.end(), site.expected.begin());
		}

		void ConfigureJumpSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                       uint8_t *target, void *destination) {
			ConfigureSite(site, virtualAddress, expected);
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
			char const *argument = std::strstr(commandLine, "-NativeShadowMasks:");
			if (argument == nullptr) return Mode::Off;
			argument += sizeof("-NativeShadowMasks:") - 1;
			auto matches = [](char const *value, char const *expected) {
				size_t const length = std::strlen(expected);
				return _strnicmp(value, expected, length) == 0 &&
				       (value[length] == '\0' || value[length] == ' ' || value[length] == '\t');
			};
			if (matches(argument, "all")) return Mode::All;
			if (matches(argument, "props")) return Mode::Props;
			if (matches(argument, "network")) return Mode::Network;
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
			if (std::memcmp(site.address, site.replacement.data(), site.length) != 0) {
				Log(LogCategory::Initialization, "native shadow masks: not restoring modified site 0x%08lX",
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

	} // namespace

	bool Install() {
#if defined(_MSC_VER) && defined(_M_IX86)
		if (gInstalled) return true;
		Mode const mode = ReadMode();
		if (mode == Mode::Off) return false;
		if (!IsSupportedGameVersion()) {
			Log(LogCategory::Initialization, "native shadow masks: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		uint8_t *const module = reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		auto const resolve = [module](uintptr_t virtualAddress) {
			return module + (virtualAddress - kImageBase);
		};

		gNetworkGateAccept = reinterpret_cast<uintptr_t>(resolve(kNetworkGateAcceptVA));
		gNetworkGateReject = reinterpret_cast<uintptr_t>(resolve(kNetworkGateRejectVA));
		gMapResultRejoin = reinterpret_cast<uintptr_t>(resolve(kMapResultRejoinVA));
		gPrebuiltDrawRejoin = reinterpret_cast<uintptr_t>(resolve(kPrebuiltDrawRejoinVA));
		gRenderPropertiesPointer = reinterpret_cast<uintptr_t>(resolve(kRenderPropertiesPointerVA));
		gPropGroundModelRejoin = reinterpret_cast<uintptr_t>(resolve(kPropGroundModelRejoinVA));
		gAddShadowRejoin = reinterpret_cast<uintptr_t>(resolve(kAddShadowRejoinVA));
		gGetBoolProperty = reinterpret_cast<uintptr_t>(resolve(kGetBoolPropertyVA));
		ConfigureJumpSite(gNetworkGate, kNetworkGateVA,
		                  {0x6A, 0x01, 0x8B, 0xCF, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74,
		                   0x19, 0x8B, 0x07, 0x6A, 0x04, 0x8B, 0xCF, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00,
		                   0x84, 0xC0, 0x74, 0x09, 0x83, 0xFB, 0x02, 0x7E, 0x04},
		                  resolve(kNetworkGateVA), &NetworkGateStub);
		ConfigureJumpSite(gMapResult, kMapResultVA, {0x85, 0xC0, 0xB9, 0xFE, 0xFF, 0xFF, 0xFF},
		                  resolve(kMapResultVA), &MapResultStub);
		ConfigureJumpSite(gPrebuiltDraw, kPrebuiltDrawVA, {0xA1, 0xC4, 0x3C, 0xB4, 0x00},
		                  resolve(kPrebuiltDrawVA), &PrebuiltDrawLiveShadowHook);
		ConfigureJumpSite(gPropGroundModel, kPropGroundModelVA, {0x8D, 0x4C, 0x24, 0x13, 0x51},
		                  resolve(kPropGroundModelVA), &PropGroundModelStub);
		ConfigureJumpSite(gAddShadowSite, kAddShadowSiteVA, {0x8B, 0xCD, 0x57, 0xFF, 0x52, 0x2C},
		                  resolve(kAddShadowSiteVA), &AddShadowLivePropStub);
		PatchSite *const networkSites[]{&gNetworkGate, &gMapResult, &gPrebuiltDraw};
		PatchSite *const propSites[]{&gPropGroundModel, &gAddShadowSite};
		std::vector<PatchSite *> sites;
		if (mode == Mode::Network || mode == Mode::All)
			sites.insert(sites.end(), std::begin(networkSites), std::end(networkSites));
		if (mode == Mode::Props || mode == Mode::All)
			sites.insert(sites.end(), std::begin(propSites), std::end(propSites));
		for (PatchSite *site: sites) site->address = resolve(site->virtualAddress);
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow masks: byte guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}

		if (mode == Mode::Network || mode == Mode::All) LoadManifest();
		else {
			gMaskInstances.clear();
			gMaskRemaps.clear();
		}
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			gLivePropMeshes.clear();
		}
		gMode = mode;
		tLiveNetworkDrawDepth = 0;
		for (PatchSite *site: sites) {
			if (!WriteSite(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow masks: failed to write site 0x%08lX, restoring",
				    static_cast<unsigned long>(site->virtualAddress));
				for (PatchSite *written: sites) RestoreSite(*written);
				gMode = Mode::Off;
				return false;
			}
		}

		gInstalled = true;
		Log(LogCategory::Initialization, "native shadows installed (%s), %u manifest instances",
		    mode == Mode::All ? "all" : mode == Mode::Props ? "props" : "network",
		    static_cast<unsigned>(gMaskInstances.size()));
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		if (!gInstalled) return;
		RestoreSite(gAddShadowSite);
		RestoreSite(gPropGroundModel);
		RestoreSite(gPrebuiltDraw);
		RestoreSite(gMapResult);
		RestoreSite(gNetworkGate);
		gMode = Mode::Off;
		gInstalled = false;
		tLiveNetworkDrawDepth = 0;
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			gLivePropMeshes.clear();
		}
		Log(LogCategory::Initialization,
		    "native shadows uninstalled: %u live network draws, %u pieces relaxed, %u masks supplied, %u fell back, "
		    "%u live prop meshes, %u native prop calls suppressed",
		    gLiveNetworkDraws, gNetworkGateRelaxed, gNetworkMaskSupplied, gNetworkFellBack,
		    gPropMeshesRegistered, gPropCallsSuppressed);
	}

} // namespace nSCD3D11::NativeShadowMasks
