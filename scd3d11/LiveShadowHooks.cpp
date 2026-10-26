/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Live shadows for prebuilt network pieces and True3D props.
//
// SimCity 4 Deluxe 1.1.641 only. Every patch checks the bytes it replaces and
// is undone in PreAppShutdown. The game decides which draws cast shadows, and
// cGDriver captures their triangles for its own shadow map.
//
//   A 0x0061E35C  35 bytes  network shadow decal gate, sent to its reject branch
//                           (0x0061E383) unless NetworkShadowMasks is on
//   C 0x0049134B   5 bytes  Is Ground Model (0x8A5E5DB8) test in CreateOccupantShadow
//   D 0x0049195E   6 bytes  the cSTEOverlayManager::AddShadow call after it
//   E 0x0061E860   5 bytes  cSC4NetworkOccupantWithPreBuiltModel::Draw wrapper
//   F 0x0061E8A0   8 bytes  cSC4NetworkOccupantWithPreBuiltModel::GetModelInstances
//   G 0x006147D0  11 bytes  cS3DModelInstance drawable thunk (vtable +0x30)
//   H vtables 0x00A94608, 0x00A94B38, 0x00A95AC0, 0x00A97188, 0x00A991E0
//     +0x11C   4 bytes each  automaton PlaceInRenderer (0x00507F70)
//   I 0x007FB5E0  11 bytes  cS3DModelInstanceAnimated drawable thunk (vtable 0x00ABD048)
//
// Props. SC4 never shadows a prop without Is Ground Model. Site C lets such a
// prop through, and site D lets its AddShadow run so that ShadowRegistry keeps
// its mesh. Without the registry, site D records a signature of the mesh and
// skips the call, and cGDriver recognises the prop's draws by that signature.
//
// Prebuilt network pieces cast live, so their flat shadow decals (site A) would
// darken the same ground twice. A piece is drawn in two parts: the occupant
// itself (site E, which bridges use too) and its model, a cS3DModelInstance
// kept at occupant +0x1C4/+0x1C8 (sites F and G). Site F records the instances
// each occupant owns, and site G marks their draws. Ownership is checked again
// on every draw, because a freed instance's address can be reused by another
// model.
//
// Automata. PlaceInRenderer(bool) puts an occupant's models into the renderer
// or takes them out. Many occupant classes share it, so it is patched only in
// the five automaton vtables, the ones with the "State: %d" debug text
// (0x0050F0A0). The models are the list at occupant +0x110/+0x114, all
// cS3DModelInstanceAnimated. Slot 0 of each returns the drawable the renderer
// stores, which is what that class's draw thunk (site I) is called with. Their
// draws are bracketed like the prebuilt models and cast in the dynamic pass
// every frame.

#include "LiveShadowHooks.h"

#include "Diagnostics.h"
#include "GameVersion.h"
#include "ShadowRegistry.h"
#include "Settings.h"
#include "SunPath.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace nSCD3D11::LiveShadowHooks {
	namespace {

		constexpr uintptr_t kNetworkGateVA = 0x0061E35C;
		constexpr uintptr_t kNetworkGateRejectVA = 0x0061E383;
		constexpr uintptr_t kPrebuiltDrawVA = 0x0061E860;
		constexpr uintptr_t kPrebuiltDrawRejoinVA = 0x0061E865;
		constexpr uintptr_t kPrebuiltModelsVA = 0x0061E8A0;
		constexpr uintptr_t kPrebuiltModelsRejoinVA = 0x0061E8A8;
		constexpr uintptr_t kModelInstanceDrawThunkVA = 0x006147D0;
		constexpr uintptr_t kModelInstanceDrawVA = 0x00801750;
		constexpr uintptr_t kAnimatedDrawThunkVA = 0x007FB5E0;
		// The two cS3DModelInstance interface pointers a prebuilt piece owns.
		constexpr ptrdiff_t kPrebuiltModelSlots[]{0x1C4, 0x1C8};
		constexpr uintptr_t kRenderPropertiesPointerVA = 0x00B43CC4;
		constexpr uintptr_t kPropGroundModelVA = 0x0049134B;
		constexpr uintptr_t kPropGroundModelRejoinVA = 0x0049136A;
		constexpr uintptr_t kGetBoolPropertyVA = 0x005FD390;
		constexpr uintptr_t kAddShadowSiteVA = 0x0049195E;
		constexpr uintptr_t kAddShadowRejoinVA = 0x00491964;
		constexpr uintptr_t kPlaceInRendererVA = 0x00507F70;
		constexpr uintptr_t kAutomatonVtables[]{0x00A94608, 0x00A94B38, 0x00A95AC0, 0x00A97188, 0x00A991E0};
		constexpr ptrdiff_t kPlaceInRendererSlot = 0x11C;
		constexpr ptrdiff_t kOccupantModelsBegin = 0x110;
		// The automaton's fade: a tween of from, to, duration and elapsed that
		// PlaceInRenderer reads through TweenStruct::GetCurrent (0x004FA960).
		constexpr ptrdiff_t kAutomatonFade = 0x1B0;
		// More models than this on one automaton means the list is not one.
		constexpr size_t kMaxAutomatonModels = 16;
		constexpr ptrdiff_t kFrameOccupant = 0xE0;
		constexpr ptrdiff_t kAddShadowArgumentBytes = 0x14;

		// SC4 renderer. +0x24 returns the lighting manager (DrawShadows 0x00736C49).
		constexpr uintptr_t kRendererPointerVA = 0x00B43DD0;
		constexpr size_t kRendererLightingSlot = 0x24 / sizeof(void *);
		// Lighting manager vtable 0x00ABB530 +0x34, GetShadowDirection 0x007D6B50.
		constexpr size_t kLightingShadowDirectionSlot = 0x34 / sizeof(void *);
		// +0x5C, GetColor(normal) 0x007D7460: float *(this, float result[3], float const normal[3]).
		constexpr size_t kLightingColourSlot = 0x5C / sizeof(void *);
		// GetColor turns the normal's x and z by the 2x2 matrix at +0x7C and
		// dots it with the sun at +0x58 (x, y, z in that turned frame).
		constexpr ptrdiff_t kLightingSun = 0x58;
		constexpr ptrdiff_t kLightingTurn = 0x7C;
		// SetTimeOfDay (0x007D9DF0) writes max((lighting red - 0.8) / 0.2, 0)
		// here and sends it as the daylight transition parameter.
		constexpr ptrdiff_t kLightingDaylight = 0xBC;
		// GetShadowParams (0x007D6B70) only copies these globals out.
		constexpr uintptr_t kShadowStrengthVA = 0x00B0DC78;
		constexpr uintptr_t kShadowColourVA = 0x00B4E34C;

		constexpr size_t kMaxPatchLength = 35;

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
		PatchSite gPrebuiltDraw;
		PatchSite gPrebuiltModels;
		PatchSite gModelInstanceDraw;
		PatchSite gAnimatedDraw;
		PatchSite gPropGroundModel;
		PatchSite gAddShadowSite;
		PatchSite gPlaceInRenderer[std::size(kAutomatonVtables)];

		bool gInstalled = false;

		uintptr_t gPrebuiltDrawRejoin = 0;
		uintptr_t gPrebuiltModelsRejoin = 0;
		uintptr_t gModelInstanceDrawTarget = 0;
		uintptr_t gRenderPropertiesPointer = 0;
		uintptr_t gPropGroundModelRejoin = 0;
		uintptr_t gAddShadowRejoin = 0;
		uintptr_t gGetBoolProperty = 0;
		uintptr_t gRendererPointer = 0;
		uintptr_t gShadowStrength = 0;
		uintptr_t gShadowColour = 0;

		thread_local uint32_t tNetworkDrawDepth = 0;
		thread_local uint32_t tAutomatonDrawDepth = 0;
		thread_local float tAutomatonOpacity = 1.0f;
		thread_local void *tRelaxedPropOccupant = nullptr;

		// Sorted signatures of the relaxed props' meshes.
		std::mutex gPropMutex;
		std::vector<uint64_t> gPropMeshes;
		// The vertex counts in gPropMeshes, folded modulo 4096 into a bit set
		// readable without the lock: MayMatchPropMesh runs on every draw the
		// game makes, and only draws of a registered size are worth hashing.
		std::atomic<uint32_t> gPropCounts[128]{};

		void ClearPropCounts() {
			for (auto &word: gPropCounts) word.store(0, std::memory_order_relaxed);
		}

		// Model instances owned by prebuilt network pieces, mapped to their
		// occupant so a match can be checked again. It is read on every model
		// draw and rarely written, hence the shared lock. The count lets draws
		// skip the lock while nothing is registered.
		std::shared_mutex gLiveModelMutex;
		std::unordered_map<void const *, void const *> gLiveModelInstances;
		std::atomic<size_t> gLiveModelInstanceCount{0};
		// Entries are only replaced, never erased, because SC4 does not tell us
		// when an occupant is deleted. The cap limits the map's size, and the
		// check in IsPrebuiltModelInstance makes stale entries harmless.
		constexpr size_t kMaxLiveModelInstances = 1u << 16;

		// Drawables of placed automata, mapped to the automaton and the model
		// that handed them out, so a match can be checked again. Placement runs
		// on the simulation thread, drawing on the render thread.
		struct AutomatonModel {
			void const *automaton = nullptr;
			void const *model = nullptr;
		};
		std::shared_mutex gAutomatonMutex;
		std::unordered_map<void const *, AutomatonModel> gAutomatonModels;
		std::atomic<size_t> gAutomatonModelCount{0};

		unsigned gNetworkDraws = 0;
		unsigned gAutomatonModelsRegistered = 0;
		unsigned gAutomatonDraws = 0;
		unsigned gAutomatonModelsRejected = 0;
		unsigned gLiveModelsRegistered = 0;
		unsigned gLiveModelDraws = 0;
		unsigned gLiveModelsRejected = 0;
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

		void *SafeLighting(uintptr_t rendererPointer) {
			__try {
				void *const renderer = *reinterpret_cast<void **>(rendererPointer);
				if (renderer == nullptr) return nullptr;
				using GetLighting = void *(__thiscall *)(void *);
				return reinterpret_cast<GetLighting>((*reinterpret_cast<void ***>(renderer))[kRendererLightingSlot])(
					renderer);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// Slot 0 of an automaton's model: the drawable it puts in the renderer.
		void const *SafeModelDrawable(void *model) {
			__try {
				using GetDrawable = void *(__thiscall *)(void *);
				return reinterpret_cast<GetDrawable>((*reinterpret_cast<void ***>(model))[0])(model);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		bool SafeLightingColour(void *lighting, float const normal[3], float colour[3]) {
			__try {
				using GetColour = float *(__thiscall *)(void *, float *, float const *);
				float const *const result = reinterpret_cast<GetColour>(
					(*reinterpret_cast<void ***>(lighting))[kLightingColourSlot])(lighting, colour, normal);
				if (result != colour) std::memcpy(colour, result, sizeof(float) * 3);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeShadowDirection(void *lighting, float direction[3]) {
			__try {
				using GetDirection = float *(__thiscall *)(void *, float *);
				reinterpret_cast<GetDirection>(
					(*reinterpret_cast<void ***>(lighting))[kLightingShadowDirectionSlot])(lighting, direction);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeCopy(void const *, void *, size_t) { return false; }
		void *SafeLighting(uintptr_t) { return nullptr; }
		void const *SafeModelDrawable(void *) { return nullptr; }
		bool SafeLightingColour(void *, float const *, float *) { return false; }
		bool SafeShadowDirection(void *, float *) { return false; }
#endif

		void HashWord(uint64_t &hash, uint32_t word) {
			hash = (hash ^ word) * 0x100000001B3ull;
		}

		void RecordPropMesh(uint32_t count, void const *positionStream, void const *uvStream) {
			struct Vector3 { float x, y, z; };
			struct Vector2 { float u, v; };
			auto const *const positions = static_cast<Vector3 const *>(positionStream);
			auto const *const uvs = static_cast<Vector2 const *>(uvStream);
			uint64_t signature = 0xCBF29CE484222325ull;
			HashWord(signature, count);
			for (uint32_t index = 0; index < count; ++index) {
				Vector3 position{};
				Vector2 uv{};
				if (!SafeCopy(positions + index, &position, sizeof(position)) ||
				    !SafeCopy(uvs + index, &uv, sizeof(uv))) return;
				uint32_t words[5]{};
				std::memcpy(words, &position, sizeof(position));
				std::memcpy(words + 3, &uv, sizeof(uv));
				for (uint32_t word: words) HashWord(signature, word);
			}
			std::lock_guard<std::mutex> lock(gPropMutex);
			auto const found = std::lower_bound(gPropMeshes.begin(), gPropMeshes.end(), signature);
			if (found != gPropMeshes.end() && *found == signature) return;
			gPropMeshes.insert(found, signature);
			gPropCounts[(count >> 5) & 127].fetch_or(1u << (count & 31), std::memory_order_relaxed);
			++gPropMeshesRegistered;
			Log(LogCategory::Initialization, "live shadows: registered prop mesh sig=%08X%08X verts=%u",
			    static_cast<unsigned>(signature >> 32), static_cast<unsigned>(signature), count);
		}

		// ------------------------------------------------------------------
		// Prebuilt model instances
		// ------------------------------------------------------------------

		// Records the model instances a prebuilt piece owns, read straight back
		// out of the occupant once GetModelInstances has refreshed both slots.
		void RegisterPrebuiltModelInstances(void const *occupant) {
			if (occupant == nullptr) return;
			for (ptrdiff_t const slot: kPrebuiltModelSlots) {
				void const *instance = nullptr;
				if (!SafeCopy(static_cast<uint8_t const *>(occupant) + slot, &instance, sizeof(instance)) ||
				    instance == nullptr)
					continue;
				std::unique_lock<std::shared_mutex> lock(gLiveModelMutex);
				if (gLiveModelInstances.size() >= kMaxLiveModelInstances) {
					gLiveModelInstances.clear();
					Log(LogCategory::Initialization,
					    "live shadows: prebuilt model registry hit its cap, cleared");
				}
				if (gLiveModelInstances.insert_or_assign(instance, occupant).second) ++gLiveModelsRegistered;
				gLiveModelInstanceCount.store(gLiveModelInstances.size(), std::memory_order_relaxed);
			}
		}

		// True while this instance is still one of its occupant's prebuilt
		// models. A freed instance's address can be reused by another model, but
		// a live occupant's slot only ever points at its own instance.
		bool IsPrebuiltModelInstance(void const *instance) {
			if (instance == nullptr) return false;
			if (gLiveModelInstanceCount.load(std::memory_order_relaxed) == 0) return false;
			void const *occupant = nullptr;
			{
				std::shared_lock<std::shared_mutex> lock(gLiveModelMutex);
				auto const found = gLiveModelInstances.find(instance);
				if (found == gLiveModelInstances.end()) return false;
				occupant = found->second;
			}
			for (ptrdiff_t const slot: kPrebuiltModelSlots) {
				void const *current = nullptr;
				if (SafeCopy(static_cast<uint8_t const *>(occupant) + slot, &current, sizeof(current)) &&
				    current == instance)
					return true;
			}
			++gLiveModelsRejected;
			return false;
		}

		// ------------------------------------------------------------------
		// Automata
		// ------------------------------------------------------------------

		// The models in an automaton's list, or false when it cannot be read.
		bool AutomatonModels(void const *automaton, std::array<void *, kMaxAutomatonModels> &models, size_t &count) {
			uintptr_t range[2]{};
			if (!SafeCopy(static_cast<uint8_t const *>(automaton) + kOccupantModelsBegin, range, sizeof(range)) ||
			    range[1] < range[0] || (range[1] - range[0]) % sizeof(void *) != 0)
				return false;
			count = (range[1] - range[0]) / sizeof(void *);
			return count <= kMaxAutomatonModels &&
			       SafeCopy(reinterpret_cast<void const *>(range[0]), models.data(), count * sizeof(void *));
		}

		// Records or forgets the drawables an automaton puts in the renderer.
		void TrackAutomaton(void const *automaton, bool placed) {
			std::array<void *, kMaxAutomatonModels> models{};
			size_t count = 0;
			if (automaton == nullptr || !AutomatonModels(automaton, models, count)) return;
			for (size_t index = 0; index < count; ++index) {
				if (models[index] == nullptr) continue;
				void const *const drawable = SafeModelDrawable(models[index]);
				if (drawable == nullptr) continue;
				std::unique_lock<std::shared_mutex> lock(gAutomatonMutex);
				if (!placed) {
					gAutomatonModels.erase(drawable);
				} else {
					if (gAutomatonModels.size() >= kMaxLiveModelInstances) {
						gAutomatonModels.clear();
						Log(LogCategory::Initialization, "live shadows: automaton registry hit its cap, cleared");
					}
					if (gAutomatonModels.insert_or_assign(drawable, AutomatonModel{automaton, models[index]}).second)
						++gAutomatonModelsRegistered;
				}
				gAutomatonModelCount.store(gAutomatonModels.size(), std::memory_order_relaxed);
			}
		}

		// TweenStruct::GetCurrent, read rather than called.
		float AutomatonFade(void const *automaton) {
			float tween[4]{};
			if (!SafeCopy(static_cast<uint8_t const *>(automaton) + kAutomatonFade, tween, sizeof(tween)))
				return 1.0f;
			float const value = tween[2] <= 0.0f
				                    ? tween[1]
				                    : tween[0] + (tween[1] - tween[0]) * (std::clamp)(tween[3] / tween[2], 0.0f, 1.0f);
			return std::isfinite(value) ? (std::clamp)(value, 0.0f, 1.0f) : 1.0f;
		}

		// True while this drawable still belongs to a placed automaton: the
		// occupant is still an automaton and still lists the model.
		bool IsAutomatonModel(void const *drawable, void const *&automaton) {
			if (drawable == nullptr || gAutomatonModelCount.load(std::memory_order_relaxed) == 0) return false;
			AutomatonModel entry;
			{
				std::shared_lock<std::shared_mutex> lock(gAutomatonMutex);
				auto const found = gAutomatonModels.find(drawable);
				if (found == gAutomatonModels.end()) return false;
				entry = found->second;
			}
			uintptr_t vtable = 0;
			std::array<void *, kMaxAutomatonModels> models{};
			size_t count = 0;
			if (SafeCopy(entry.automaton, &vtable, sizeof(vtable)) &&
			    std::find(std::begin(kAutomatonVtables), std::end(kAutomatonVtables), vtable) !=
			    std::end(kAutomatonVtables) &&
			    AutomatonModels(entry.automaton, models, count)) {
				auto const end = models.begin() + static_cast<ptrdiff_t>(count);
				if (std::find(models.begin(), end, entry.model) != end) {
					automaton = entry.automaton;
					return true;
				}
			}
			++gAutomatonModelsRejected;
			return false;
		}

	} // namespace

	// ----------------------------------------------------------------------
	// Hook bodies
	// ----------------------------------------------------------------------

	extern "C" void __cdecl SCD3D11_LiveShadowNoteRelaxedProp(void *occupant) {
		tRelaxedPropOccupant = occupant;
	}

	// The rejected prop has reached the point where SC4 extracted its position
	// and UV streams. Record its mesh signature, then suppress AddShadow (2).
	// 0 lets any other call through untouched.
	extern "C" uint32_t __cdecl SCD3D11_LiveShadowRegisterProp(uint32_t *stackArguments) {
		uint8_t const *const frameBase =
			reinterpret_cast<uint8_t const *>(stackArguments) + kAddShadowArgumentBytes;
		uint32_t occupant = 0;
		if (!SafeCopy(frameBase + kFrameOccupant, &occupant, sizeof(occupant)) || occupant == 0 ||
		    reinterpret_cast<void *>(occupant) != tRelaxedPropOccupant) return 0;

		uint32_t const count = stackArguments[0];
		void const *const positions = reinterpret_cast<void const *>(stackArguments[1]);
		void const *const uvs = reinterpret_cast<void const *>(stackArguments[2]);
		if (count < 3 || count > 0x100000 || positions == nullptr || uvs == nullptr) return 2;
		if (ShadowRegistry::Enabled()) {
			ShadowRegistry::ExpectMeshCaster();
			return 0;
		}
		RecordPropMesh(count, positions, uvs);
		++gPropCallsSuppressed;
		return 2;
	}

	void RegisterPropMesh(uint32_t count, void const *positions, void const *uvs) {
		if (count >= 3 && count <= 0x100000 && positions != nullptr && uvs != nullptr)
			RecordPropMesh(count, positions, uvs);
	}

	bool Enabled() {
		return gInstalled;
	}

	bool NetworkDrawActive() {
		return gInstalled && tNetworkDrawDepth != 0;
	}

	bool AutomatonDrawActive() {
		return gInstalled && tAutomatonDrawDepth != 0;
	}

	float AutomatonOpacity() {
		return AutomatonDrawActive() ? tAutomatonOpacity : 1.0f;
	}

	bool MayMatchPropMesh(uint32_t vertexCount) {
		return gInstalled &&
		       (gPropCounts[(vertexCount >> 5) & 127].load(std::memory_order_relaxed) & (1u << (vertexCount & 31))) != 0;
	}

	bool MatchPropSignature(uint64_t signature) {
		if (!gInstalled) return false;
		std::lock_guard<std::mutex> lock(gPropMutex);
		return std::binary_search(gPropMeshes.begin(), gPropMeshes.end(), signature);
	}

	bool SunDirection(float direction[3]) {
		if (gInstalled && SunPath::Current(direction)) return true;
		float sun[3]{};
		void *const lighting = gInstalled ? SafeLighting(gRendererPointer) : nullptr;
		if (lighting == nullptr || !SafeShadowDirection(lighting, sun) || !std::isfinite(sun[0]) ||
		    !std::isfinite(sun[1]) || !std::isfinite(sun[2]))
			return false;
		std::memcpy(direction, sun, sizeof(sun));
		return true;
	}

	bool ShadowParams(float colour[3], float &strength) {
		float tone[3]{};
		float value = 0.0f;
		if (!gInstalled || !SafeCopy(reinterpret_cast<void const *>(gShadowColour), tone, sizeof(tone)) ||
		    !SafeCopy(reinterpret_cast<void const *>(gShadowStrength), &value, sizeof(value)))
			return false;
		std::memcpy(colour, tone, sizeof(tone));
		strength = value;
		return true;
	}

	bool TerrainLight(float const normal[3], float colour[3]) {
		void *const lighting = gInstalled ? SafeLighting(gRendererPointer) : nullptr;
		return lighting != nullptr && SafeLightingColour(lighting, normal, colour) && std::isfinite(colour[0]) &&
		       std::isfinite(colour[1]) && std::isfinite(colour[2]);
	}

	bool LightingSun(float toLight[3]) {
		void *const lighting = gInstalled ? SafeLighting(gRendererPointer) : nullptr;
		float sun[3]{}, turn[4]{};
		if (lighting == nullptr ||
		    !SafeCopy(static_cast<uint8_t const *>(lighting) + kLightingSun, sun, sizeof(sun)) ||
		    !SafeCopy(static_cast<uint8_t const *>(lighting) + kLightingTurn, turn, sizeof(turn)))
			return false;
		// GetColor dots (a x + b z, y, c x + d z) with the sun, which is the
		// world normal dotted with this.
		toLight[0] = turn[0] * sun[0] + turn[2] * sun[2];
		toLight[1] = sun[1];
		toLight[2] = turn[1] * sun[0] + turn[3] * sun[2];
		float const length = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
		if (!(length > 1.0e-6f)) return false;
		for (unsigned axis = 0; axis < 3; ++axis) toLight[axis] /= length;
		return true;
	}

	bool ShadowDaylight(float &daylight) {
		float value = 0.0f;
		void *const lighting = gInstalled ? SafeLighting(gRendererPointer) : nullptr;
		if (lighting == nullptr ||
		    !SafeCopy(static_cast<uint8_t const *>(lighting) + kLightingDaylight, &value, sizeof(value)) ||
		    !std::isfinite(value))
			return false;
		daylight = (std::clamp)(value, 0.0f, 1.0f);
		return true;
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
			++tNetworkDrawDepth;
			uint32_t const result =
				reinterpret_cast<Original>(&PrebuiltDrawOriginal)(self, drawContext, drawState);
			--tNetworkDrawDepth;
			++gNetworkDraws;
			return result;
		}

		// Site F. 0x0061E8A0 is __thiscall(self, 5 stack args, RET 0x14). The two
		// displaced MOVs read arguments off ESP, so the trampoline has to be
		// entered by a call carrying the same arguments.
		__declspec(naked) void PrebuiltModelsOriginal() {
			__asm {
				mov eax, dword ptr [esp + 0x14]
				mov edx, dword ptr [esp + 0x0c]
				jmp dword ptr [gPrebuiltModelsRejoin]
			}
		}

		void __fastcall PrebuiltModelsHook(
			void *self, void *, int zoom, int rotation, void **instances, int capacity, int *count) {
			using Original = void (__thiscall *)(void *, int, int, void **, int, int *);
			reinterpret_cast<Original>(&PrebuiltModelsOriginal)(
				self, zoom, rotation, instances, capacity, count);
			// Read the instances back out of the occupant rather than out of the
			// array: the array also carries whatever the base network occupant
			// contributed, and +0x1C4/+0x1C8 are the slots the validation checks.
			RegisterPrebuiltModelInstances(self);
		}

		// Site G. Replaces the whole this-adjusting thunk, so ECX here is still
		// the interface pointer the drawable list stores - the same value
		// GetModelInstances handed out - and no pointer arithmetic is needed to
		// compare the two. The trampoline replays the adjustment the thunk did.
		__declspec(naked) void ModelInstanceDrawOriginal() {
			__asm {
				sub ecx, dword ptr [ecx - 4]
				sub ecx, 0x40
				jmp dword ptr [gModelInstanceDrawTarget]
			}
		}

		uint32_t __fastcall ModelInstanceDrawHook(void *self, void *, void *drawContext, uint32_t lit) {
			using Original = uint32_t (__thiscall *)(void *, void *, uint32_t);
			bool const caster = IsPrebuiltModelInstance(self);
			if (caster) ++tNetworkDrawDepth;
			uint32_t const result =
				reinterpret_cast<Original>(&ModelInstanceDrawOriginal)(self, drawContext, lit);
			if (caster) {
				--tNetworkDrawDepth;
				++gLiveModelDraws;
				static bool loggedFirst = false;
				if (!loggedFirst) {
					loggedFirst = true;
					Log(LogCategory::Initialization,
					    "live shadows: first prebuilt model bracketed (instance %p)", self);
				}
			}
			return result;
		}

		// Site I. The same thunk for cS3DModelInstanceAnimated, whose drawable
		// sits 0x24 into its virtual base rather than 0x40.
		__declspec(naked) void AnimatedDrawOriginal() {
			__asm {
				sub ecx, dword ptr [ecx - 4]
				sub ecx, 0x24
				jmp dword ptr [gModelInstanceDrawTarget]
			}
		}

		uint32_t __fastcall AnimatedDrawHook(void *self, void *, void *drawContext, uint32_t lit) {
			using Original = uint32_t (__thiscall *)(void *, void *, uint32_t);
			void const *owner = nullptr;
			bool const automaton = IsAutomatonModel(self, owner);
			if (automaton) {
				tAutomatonOpacity = AutomatonFade(owner);
				++tAutomatonDrawDepth;
			}
			uint32_t const result = reinterpret_cast<Original>(&AnimatedDrawOriginal)(self, drawContext, lit);
			if (automaton) {
				--tAutomatonDrawDepth;
				++gAutomatonDraws;
				static bool loggedFirst = false;
				if (!loggedFirst) {
					loggedFirst = true;
					Log(LogCategory::Initialization, "live shadows: first automaton model bracketed (drawable %p)",
					    self);
				}
			}
			return result;
		}

		// Site H, __thiscall(self, bool place), RET 4. Called through the
		// automaton vtables only, so self is always an automaton.
		uint8_t __fastcall PlaceInRendererHook(void *self, void *, uint8_t place) {
			using Original = uint8_t (__thiscall *)(void *, uint8_t);
			if (!place) TrackAutomaton(self, false);
			uint8_t const result = reinterpret_cast<Original>(kPlaceInRendererVA)(self, place);
			if (place) TrackAutomaton(self, true);
			return result;
		}

		__declspec(naked) void PropGroundModelStub() {
			__asm {
				push eax
				push ecx
				push edx
				push 0
				call SCD3D11_LiveShadowNoteRelaxedProp
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
				call SCD3D11_LiveShadowNoteRelaxedProp
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
				call SCD3D11_LiveShadowRegisterProp
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

		void ConfigureJumpSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                       uint8_t *target, void *destination) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = expected.size();
			std::copy(expected.begin(), expected.end(), site.expected.begin());
			site.replacement.fill(0x90);
			site.replacement[0] = 0xE9;
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
			                                                 (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(site.replacement.data() + 1, &displacement, sizeof(displacement));
		}

		// A vtable slot: it holds the original function's address.
		void ConfigurePointerSite(PatchSite &site, uintptr_t virtualAddress, uintptr_t expected, void *destination) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = sizeof(uint32_t);
			uint32_t const original = static_cast<uint32_t>(expected);
			uint32_t const replacement = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destination));
			std::memcpy(site.expected.data(), &original, sizeof(original));
			std::memcpy(site.replacement.data(), &replacement, sizeof(replacement));
		}

		bool Requested() {
			return Settings::Get().liveShadows;
		}

		bool KeepNetworkShadowMasks() {
			return Settings::Get().networkShadowMasks;
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
				Log(LogCategory::Initialization, "live shadows: not restoring modified site 0x%08lX",
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
		if (!Requested()) return false;
		if (!IsGameVersion641()) {
			Log(LogCategory::Initialization, "live shadows: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		// IsGameVersion641 only accepts an executable at its preferred base, so
		// the game's addresses are used as they are.
		auto const resolve = [](uintptr_t virtualAddress) { return reinterpret_cast<uint8_t *>(virtualAddress); };

		gPrebuiltDrawRejoin = reinterpret_cast<uintptr_t>(resolve(kPrebuiltDrawRejoinVA));
		gPrebuiltModelsRejoin = reinterpret_cast<uintptr_t>(resolve(kPrebuiltModelsRejoinVA));
		gModelInstanceDrawTarget = reinterpret_cast<uintptr_t>(resolve(kModelInstanceDrawVA));
		gRenderPropertiesPointer = reinterpret_cast<uintptr_t>(resolve(kRenderPropertiesPointerVA));
		gPropGroundModelRejoin = reinterpret_cast<uintptr_t>(resolve(kPropGroundModelRejoinVA));
		gAddShadowRejoin = reinterpret_cast<uintptr_t>(resolve(kAddShadowRejoinVA));
		gGetBoolProperty = reinterpret_cast<uintptr_t>(resolve(kGetBoolPropertyVA));
		gRendererPointer = reinterpret_cast<uintptr_t>(resolve(kRendererPointerVA));
		gShadowStrength = reinterpret_cast<uintptr_t>(resolve(kShadowStrengthVA));
		gShadowColour = reinterpret_cast<uintptr_t>(resolve(kShadowColourVA));
		ConfigureJumpSite(gNetworkGate, kNetworkGateVA,
		                  {0x6A, 0x01, 0x8B, 0xCF, 0xFF, 0x92, 0x80, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74,
		                   0x19, 0x8B, 0x07, 0x6A, 0x04, 0x8B, 0xCF, 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00,
		                   0x84, 0xC0, 0x74, 0x09, 0x83, 0xFB, 0x02, 0x7E, 0x04},
		                  resolve(kNetworkGateVA), resolve(kNetworkGateRejectVA));
		ConfigureJumpSite(gPrebuiltDraw, kPrebuiltDrawVA, {0xA1, 0xC4, 0x3C, 0xB4, 0x00},
		                  resolve(kPrebuiltDrawVA), &PrebuiltDrawLiveShadowHook);
		ConfigureJumpSite(gPrebuiltModels, kPrebuiltModelsVA,
		                  {0x8B, 0x44, 0x24, 0x14, 0x8B, 0x54, 0x24, 0x0C},
		                  resolve(kPrebuiltModelsVA), &PrebuiltModelsHook);
		// The displacement in the thunk's own JMP is image-base independent, so
		// it is part of the guard.
		ConfigureJumpSite(gModelInstanceDraw, kModelInstanceDrawThunkVA,
		                  {0x2B, 0x49, 0xFC, 0x83, 0xE9, 0x40, 0xE9, 0x75, 0xCF, 0x1E, 0x00},
		                  resolve(kModelInstanceDrawThunkVA), &ModelInstanceDrawHook);
		ConfigureJumpSite(gAnimatedDraw, kAnimatedDrawThunkVA,
		                  {0x2B, 0x49, 0xFC, 0x83, 0xE9, 0x24, 0xE9, 0x65, 0x61, 0x00, 0x00},
		                  resolve(kAnimatedDrawThunkVA), &AnimatedDrawHook);
		ConfigureJumpSite(gPropGroundModel, kPropGroundModelVA, {0x8D, 0x4C, 0x24, 0x13, 0x51},
		                  resolve(kPropGroundModelVA), &PropGroundModelStub);
		ConfigureJumpSite(gAddShadowSite, kAddShadowSiteVA, {0x8B, 0xCD, 0x57, 0xFF, 0x52, 0x2C},
		                  resolve(kAddShadowSiteVA), &AddShadowLivePropStub);
		std::vector<PatchSite *> sites{&gPrebuiltDraw, &gPrebuiltModels, &gModelInstanceDraw, &gPropGroundModel,
		                               &gAddShadowSite, &gAnimatedDraw};
		for (size_t index = 0; index < std::size(kAutomatonVtables); ++index) {
			ConfigurePointerSite(gPlaceInRenderer[index], kAutomatonVtables[index] + kPlaceInRendererSlot,
			                     kPlaceInRendererVA, &PlaceInRendererHook);
			sites.push_back(&gPlaceInRenderer[index]);
		}
		if (!KeepNetworkShadowMasks()) sites.push_back(&gNetworkGate);
		for (PatchSite *site: sites) site->address = resolve(site->virtualAddress);
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization, "live shadows: byte guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}

		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			gPropMeshes.clear();
			ClearPropCounts();
		}
		{
			std::unique_lock<std::shared_mutex> lock(gLiveModelMutex);
			gLiveModelInstances.clear();
			gLiveModelInstanceCount.store(0, std::memory_order_relaxed);
		}
		{
			std::unique_lock<std::shared_mutex> lock(gAutomatonMutex);
			gAutomatonModels.clear();
			gAutomatonModelCount.store(0, std::memory_order_relaxed);
		}
		tNetworkDrawDepth = 0;
		tAutomatonDrawDepth = 0;
		for (PatchSite *site: sites) {
			if (!WriteSite(*site)) {
				Log(LogCategory::Initialization, "live shadows: failed to write site 0x%08lX, restoring",
				    static_cast<unsigned long>(site->virtualAddress));
				for (PatchSite *written: sites) RestoreSite(*written);
				return false;
			}
		}

		gInstalled = true;
		Log(LogCategory::Initialization, "live shadows installed: prebuilt network pieces, True3D props and automata, %s",
		    KeepNetworkShadowMasks() ? "network shadow masks kept" : "network shadow masks removed");
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		if (!gInstalled) return;
		for (PatchSite &site: gPlaceInRenderer) RestoreSite(site);
		RestoreSite(gAnimatedDraw);
		RestoreSite(gAddShadowSite);
		RestoreSite(gPropGroundModel);
		RestoreSite(gModelInstanceDraw);
		RestoreSite(gPrebuiltModels);
		RestoreSite(gPrebuiltDraw);
		RestoreSite(gNetworkGate);
		gInstalled = false;
		tNetworkDrawDepth = 0;
		tAutomatonDrawDepth = 0;
		{
			std::lock_guard<std::mutex> lock(gPropMutex);
			gPropMeshes.clear();
			ClearPropCounts();
		}
		{
			std::unique_lock<std::shared_mutex> lock(gLiveModelMutex);
			gLiveModelInstances.clear();
			gLiveModelInstanceCount.store(0, std::memory_order_relaxed);
		}
		{
			std::unique_lock<std::shared_mutex> lock(gAutomatonMutex);
			gAutomatonModels.clear();
			gAutomatonModelCount.store(0, std::memory_order_relaxed);
		}
		Log(LogCategory::Initialization,
		    "automata: %u models registered, %u bracketed draws, %u stale entries rejected",
		    gAutomatonModelsRegistered, gAutomatonDraws, gAutomatonModelsRejected);
		Log(LogCategory::Initialization,
		    "live shadows uninstalled: %u network draws, %u prop meshes, %u native prop calls suppressed",
		    gNetworkDraws, gPropMeshesRegistered, gPropCallsSuppressed);
		Log(LogCategory::Initialization,
		    "prebuilt models: %u instances registered, %u bracketed draws, %u stale entries rejected",
		    gLiveModelsRegistered, gLiveModelDraws, gLiveModelsRejected);
	}
} // namespace nSCD3D11::LiveShadowHooks
