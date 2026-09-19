/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Shadow masks for prebuilt network pieces and True3D props.
//
// SimCity 4 Deluxe 1.1.641, Windows x86, image base 0x00400000. Opt-in with
// -NativeShadowMasks:network, :props or :all. Every site is guarded by its
// exact bytes and restored during PreAppShutdown.
//
// What the Phase 1 instrumentation established, and why the patches are shaped
// this way (see docs/true3d-shadows-phase1.md):
//
//  * A prebuilt network piece casts a shadow if and only if an FSH exists at
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
//  * True3D props fail for three measured reasons: geometry sitting above the
//    decal plane, UVs outside [0,1], and one decal per mesh. Site C reruns the
//    Is Ground Model test itself and records the occupants it rejects; site D
//    then acts only on those occupants and skips the call outright for a mesh
//    with no generated proxy. The render model is never touched, the property
//    test is never disabled, and a prop without a proxy keeps the no-shadow
//    behaviour it has today.
//
//    INCOMPLETE: the proxy currently substitutes geometry only - the source
//    mesh flattened onto its base plane, with UVs wrapped into the unit
//    square - and that is not enough. Tested in-game on an 837-vertex
//    catenary prop it produces one solid dark parallelogram: flattening
//    removes the height smear, but AddShadow still stamps every triangle, so
//    the result is the model's filled footprint rather than a silhouette, and
//    wrapping the UVs destroys the mapping that would otherwise let the
//    material's alpha cut it.
//
//    A correct proxy needs both halves that the design calls for: a single
//    quad with 0..1 UVs *and* a generated alpha mask bound as the texture.
//    The missing piece is a cS3DTextureBinding for a generated FSH, which
//    means finding the binding factory on Windows (the Mac reference has
//    GZCOM_cS3DTextureBindingFactoryCLSID) and creating one from the key
//    {0x7AB50E44, 0x2BC2759A, <generated instance>}. Until that exists,
//    :props is inert unless a signature is added to the manifest by hand, and
//    doing so currently looks worse than no shadow.
//
// Verified sites:
//
//   A 0x0061E35C  35 bytes  the HasNetworkFlag(1)/(4)/quality gate
//   B 0x0061E589   7 bytes  TEST EAX,EAX / MOV ECX,-2 after MapShadowTexture
//   C 0x0049134B   5 bytes  LEA ECX,[ESP+0x13] / PUSH ECX, the Is Ground Model test
//   D 0x0049195E   6 bytes  MOV ECX,EBP / PUSH EDI / CALL [EDX+0x2C]
//
// Sites C and D are shared with other modules: D is also the diagnostics
// AddShadow hook and C is also NativeShadowExperiment's patch B. This module
// installs first, and the others' byte guards make them skip rather than
// fight over the bytes.

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
		constexpr uintptr_t kPropGroundModelVA = 0x0049134B;
		constexpr uintptr_t kPropGroundModelRejoinVA = 0x0049136A;
		constexpr uintptr_t kGetBoolPropertyVA = 0x005FD390; // __cdecl(exemplar, id, out)
		constexpr uintptr_t kIsGroundModelProperty = 0x8A5E5DB8;
		constexpr uintptr_t kAddShadowSiteVA = 0x0049195E;
		constexpr uintptr_t kAddShadowRejoinVA = 0x00491964;

		// CreateOccupantShadow frame, relative to the body stack pointer B.
		// At the AddShadow site B is ESP + 0x14, the five pushed arguments.
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
		PatchSite gPropGroundModel;
		PatchSite gAddShadowSite;

		Mode gMode = Mode::Off;
		bool gInstalled = false;

		uintptr_t gNetworkGateAccept = 0;
		uintptr_t gNetworkGateReject = 0;
		uintptr_t gMapResultRejoin = 0;
		uintptr_t gAddShadowRejoin = 0;
		uintptr_t gPropGroundModelRejoin = 0;
		uintptr_t gGetBoolProperty = 0;

		// Generated mask instances, sorted for binary search, plus explicit
		// source -> generated remappings. Loaded once; never touched during a
		// frame, so lookups need no lock.
		std::vector<uint32_t> gMaskInstances;
		std::vector<std::pair<uint32_t, uint32_t>> gMaskRemaps;
		// Mesh signatures with a generated proxy, from "P <hex>" manifest lines.
		std::vector<uint64_t> gPropSignatures;

		// Set by the gate stub, read by the mapping stub a few calls later on
		// the same thread inside the same UpdateShadow.
		thread_local bool tGatePassedOriginally = false;

		// The occupant whose Is Ground Model test just failed. Only that one
		// occupant's meshes may be proxied or suppressed at the AddShadow site,
		// so buildings, flora, power poles and genuine ground-model props keep
		// running through the original path untouched.
		thread_local void *tRelaxedPropOccupant = nullptr;

		struct Vector3 {
			float x, y, z;
		};

		struct Vector2 {
			float u, v;
		};

		thread_local std::vector<Vector3> tProxyPositions;
		thread_local std::vector<Vector2> tProxyUVs;

		unsigned gNetworkGateRelaxed = 0;
		unsigned gNetworkMaskSupplied = 0;
		unsigned gNetworkFellBack = 0;
		unsigned gPropProxies = 0;
		unsigned gPropSuppressed = 0;

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

		bool HasPropProxy(uint64_t signature) {
			return std::binary_search(gPropSignatures.begin(), gPropSignatures.end(), signature);
		}

		// Identifies a mesh without needing its TGI, which Phase 1 showed is not
		// recoverable at this call site: the resolver never writes the resolved
		// instance back into the key buffer. Vertex count plus the bounding box
		// extents quantised to 1/16 of a unit is stable across runs, and sorting
		// the two horizontal extents makes it survive the four map rotations.
		uint64_t MeshSignature(uint32_t vertexCount, float extentX, float extentY, float extentZ) {
			float const flat = (std::min)(extentX, extentZ);
			float const deep = (std::max)(extentX, extentZ);
			uint64_t parts[4] = {
				vertexCount,
				static_cast<uint64_t>(static_cast<int64_t>(flat * 16.0f + 0.5f)),
				static_cast<uint64_t>(static_cast<int64_t>(extentY * 16.0f + 0.5f)),
				static_cast<uint64_t>(static_cast<int64_t>(deep * 16.0f + 0.5f)),
			};
			uint64_t hash = 0xCBF29CE484222325ull;
			for (uint64_t const part: parts) hash = (hash ^ part) * 0x100000001B3ull;
			return hash;
		}

		uint32_t RemapInstance(uint32_t instance) {
			for (auto const &entry: gMaskRemaps) {
				if (entry.first == instance) return entry.second;
			}
			return 0;
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

		// One hex instance per line, or "source -> generated" to point a piece
		// at a mask baked under a different id. Blank lines and # are ignored.
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
				if (*cursor == 'P' || *cursor == 'p') {
					char *signatureEnd = nullptr;
					unsigned long long const signature = _strtoui64(cursor + 1, &signatureEnd, 16);
					if (signatureEnd != cursor + 1) gPropSignatures.push_back(signature);
					continue;
				}
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
			std::sort(gPropSignatures.begin(), gPropSignatures.end());
			gPropSignatures.erase(std::unique(gPropSignatures.begin(), gPropSignatures.end()),
			                      gPropSignatures.end());
			std::sort(gMaskInstances.begin(), gMaskInstances.end());
			gMaskInstances.erase(std::unique(gMaskInstances.begin(), gMaskInstances.end()),
			                     gMaskInstances.end());
			Log(LogCategory::Initialization,
			    "native shadow masks: manifest has %u instances, %u remappings, %u prop proxies",
			    static_cast<unsigned>(gMaskInstances.size()), static_cast<unsigned>(gMaskRemaps.size()),
			    static_cast<unsigned>(gPropSignatures.size()));
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

	// Called only when the Is Ground Model test has just rejected this prop.
	// Recording the occupant, rather than a bare flag, is what keeps the
	// suppression below scoped: the AddShadow site is shared by buildings,
	// flora and power poles, and a stale flag would have silenced those too.
	extern "C" void __cdecl SCD3D11_MaskNoteRelaxedProp(void *occupant) {
		tRelaxedPropOccupant = occupant;
	}

	// Runs just before cSTEOverlayManager::AddShadow. Returns 0 to let the call
	// proceed untouched, 1 after substituting a flattened proxy, or 2 to skip
	// the call entirely.
	//
	// Only meshes belonging to a prop the Is Ground Model test rejected are
	// eligible, so nothing else on this shared call site changes behaviour. Of
	// those, a mesh with no generated proxy is skipped rather than stamped,
	// which leaves the prop with no shadow - exactly what it has today.
	extern "C" uint32_t __cdecl SCD3D11_MaskAddShadowProxy(uint32_t *stackArguments) {
		uint8_t const *const frameBase =
			reinterpret_cast<uint8_t const *>(stackArguments) + kAddShadowArgumentBytes;
		uint32_t occupant = 0;
		if (!SafeCopy(frameBase + kFrameOccupant, &occupant, sizeof(occupant))) return 0;
		if (occupant == 0 || reinterpret_cast<void *>(occupant) != tRelaxedPropOccupant) return 0;

		uint32_t const vertexCount = stackArguments[0];
		auto *const positions = reinterpret_cast<Vector3 *>(stackArguments[1]);
		auto *const uvs = reinterpret_cast<Vector2 *>(stackArguments[2]);
		if (vertexCount < 3 || vertexCount > 0xFFFF || positions == nullptr || uvs == nullptr) return 0;

		tProxyPositions.resize(vertexCount);
		if (!SafeCopy(positions, tProxyPositions.data(), vertexCount * sizeof(Vector3))) return 0;

		// Extents come from the vertices, not from the bounding-box argument:
		// the ShadowQuality >= 5 branch at 0x00491941 passes a different box,
		// and a signature that changed with the quality setting would match the
		// manifest at some settings and not others.
		Vector3 lowest = tProxyPositions[0];
		Vector3 highest = tProxyPositions[0];
		for (Vector3 const &vertex: tProxyPositions) {
			lowest.x = (std::min)(lowest.x, vertex.x);
			lowest.y = (std::min)(lowest.y, vertex.y);
			lowest.z = (std::min)(lowest.z, vertex.z);
			highest.x = (std::max)(highest.x, vertex.x);
			highest.y = (std::max)(highest.y, vertex.y);
			highest.z = (std::max)(highest.z, vertex.z);
		}
		uint64_t const signature = MeshSignature(vertexCount, highest.x - lowest.x,
		                                         highest.y - lowest.y, highest.z - lowest.z);
		if (!HasPropProxy(signature)) {
			// Logged on the first occurrence and then sparsely: a run that is
			// killed rather than shut down never reaches Uninstall's summary,
			// and without this there is no evidence the path ran at all.
			if ((gPropSuppressed++ & 0xFF) == 0) {
				Log(LogCategory::Initialization,
				    "native shadow masks: no proxy for prop mesh sig=%08X%08X verts=%u "
				    "extent=%.2f/%.2f/%.2f, leaving it shadowless",
				    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature),
				    vertexCount, highest.x - lowest.x, highest.y - lowest.y, highest.z - lowest.z);
			}
			return 2;
		}

		tProxyUVs.resize(vertexCount);
		if (!SafeCopy(uvs, tProxyUVs.data(), vertexCount * sizeof(Vector2))) return 2;

		// Flatten onto the mesh's own base plane. The measured failure is that
		// geometry above the plane is displaced along the shadow direction in
		// proportion to its height, so removing the height removes the smear
		// while leaving the silhouette where the object stands.
		for (Vector3 &vertex: tProxyPositions) vertex.y = lowest.y;

		// Wrap UVs into the unit square. Every broken case measured in Phase 1
		// had v running negative across one or two texture repeats, which a
		// clamped decal sampler cannot represent.
		for (Vector2 &coordinate: tProxyUVs) {
			coordinate.u -= std::floor(coordinate.u);
			coordinate.v -= std::floor(coordinate.v);
		}

		stackArguments[1] = reinterpret_cast<uint32_t>(tProxyPositions.data());
		stackArguments[2] = reinterpret_cast<uint32_t>(tProxyUVs.data());
		if ((gPropProxies++ & 0x3F) == 0) {
			Log(LogCategory::Initialization,
			    "native shadow masks: prop proxy applied sig=%08X%08X verts=%u flattened to y=%.2f",
			    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature), vertexCount,
			    lowest.y);
		}
		return 1;
	}

	namespace {

#if defined(_MSC_VER) && defined(_M_IX86)

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

		// Reruns the Is Ground Model test that the patch displaced, so a prop
		// that genuinely sets the property keeps its original behaviour and is
		// never marked. Only a prop the test rejects is recorded, and 0x0049136A
		// is where both outcomes rejoin.
		__declspec(naked) void PropGroundModelStub() {
			__asm {
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

		// Replaces MOV ECX,EBP / PUSH EDI / CALL [EDX+0x2C]. The five pushed
		// arguments start at ESP, and the helper may rewrite the position and
		// UV pointers in place before the original call runs. Its verdict is
		// written into the pushad frame's EAX slot so it survives POPAD.
		__declspec(naked) void AddShadowProxyStub() {
			__asm {
				pushad
				pushfd
				lea  eax, [esp + 0x24]
				push eax
				call SCD3D11_MaskAddShadowProxy
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
				// AddShadow is callee-clean, so skipping it means dropping its
				// five arguments here. -1 is the value the original returns when
				// it refuses a decal, and the next instruction tests for it.
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
		gAddShadowRejoin = reinterpret_cast<uintptr_t>(resolve(kAddShadowRejoinVA));
		gPropGroundModelRejoin = reinterpret_cast<uintptr_t>(resolve(kPropGroundModelRejoinVA));
		gGetBoolProperty = reinterpret_cast<uintptr_t>(resolve(kGetBoolPropertyVA));

		ConfigureJumpSite(gNetworkGate, kNetworkGateVA,
		                  {0x6A, 0x01, 0x8B, 0xCF, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74,
		                   0x19, 0x8B, 0x07, 0x6A, 0x04, 0x8B, 0xCF, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00,
		                   0x84, 0xC0, 0x74, 0x09, 0x83, 0xFB, 0x02, 0x7E, 0x04},
		                  resolve(kNetworkGateVA), &NetworkGateStub);
		ConfigureJumpSite(gMapResult, kMapResultVA, {0x85, 0xC0, 0xB9, 0xFE, 0xFF, 0xFF, 0xFF},
		                  resolve(kMapResultVA), &MapResultStub);
		ConfigureJumpSite(gPropGroundModel, kPropGroundModelVA, {0x8D, 0x4C, 0x24, 0x13, 0x51},
		                  resolve(kPropGroundModelVA), &PropGroundModelStub);
		ConfigureJumpSite(gAddShadowSite, kAddShadowSiteVA, {0x8B, 0xCD, 0x57, 0xFF, 0x52, 0x2C},
		                  resolve(kAddShadowSiteVA), &AddShadowProxyStub);

		PatchSite *const networkSites[] = {&gNetworkGate, &gMapResult};
		PatchSite *const propSites[] = {&gPropGroundModel, &gAddShadowSite};
		std::vector<PatchSite *> sites;
		if (mode == Mode::Network || mode == Mode::All) {
			sites.insert(sites.end(), std::begin(networkSites), std::end(networkSites));
		}
		if (mode == Mode::Props || mode == Mode::All) {
			sites.insert(sites.end(), std::begin(propSites), std::end(propSites));
		}
		for (PatchSite *site: sites) site->address = resolve(site->virtualAddress);
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization,
				    "native shadow masks: byte guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}

		LoadManifest();
		gMode = mode;
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
		Log(LogCategory::Initialization, "native shadow masks installed (%s), %u manifest instances",
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
		RestoreSite(gMapResult);
		RestoreSite(gNetworkGate);
		gMode = Mode::Off;
		gInstalled = false;
		Log(LogCategory::Initialization,
		    "native shadow masks uninstalled: %u pieces relaxed, %u masks supplied, %u fell back, "
		    "%u prop proxies, %u prop calls suppressed",
		    gNetworkGateRelaxed, gNetworkMaskSupplied, gNetworkFellBack, gPropProxies, gPropSuppressed);
	}

} // namespace nSCD3D11::NativeShadowMasks
