/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Shadow records for the live shadow map.
//
// SimCity 4 Deluxe 1.1.641 only. Every patch checks the bytes or vtable slot it
// replaces and is undone in PreAppShutdown.
//
//   0x00AB3A14  cSTEOverlayManager vtable +0x2C, AddShadow 0x0073C0D0
//   0x00AB3A24  cSTEOverlayManager vtable +0x3C, RemoveOverlay 0x007388C0
//   0x00736BF0  DrawShadows entry, SUB ESP,0x1C4
//   0x00737870  DrawShadowsRough entry, SUB ESP,0x9C
//   0x00491894  CreateOccupantShadow, the last point the current mesh is in EDI
//
// The DrawShadows hooks sit on the functions rather than on their calls in
// DrawOverlays, because SC4RenderServices rewrites those calls and would drop a
// hook there. The hooks take the ids this registry owns out of the list and let
// SC4 draw the rest, such as the network decals stored in the same records.
// Their first argument is the pass's eye-to-world matrix.
//
// AddShadow (__thiscall, RET 0x18) returns the record's slot index. A record is
// 0xB4 bytes in the slot array at manager +0x98: +0x00 is -1 while it is in
// use, +0x04 holds the flags, +0x1C the CalcShadowProjection matrix and +0xAC
// the texture binding. The binding's first word points at the texture object,
// whose first word is the cGDriver texture name.
//
// AddShadow gets positions and UVs but no indices, so the site at 0x00491894
// records the mesh CreateOccupantShadow is working on. Its layout matches
// SC4DrawContext::RenderMesh (0x007D4A80): +0 vertex buffer {+0 format, +6 u16
// count, +8 data}, +4 index holder whose first word is the u16 index array
// (null without indices), +8 primitive list {+4 begin, +8 end} of {primitive,
// start, count}.
//
// Props without Is Ground Model reach AddShadow through LiveShadowHooks' site D.
// One floating above the terrain, such as a building zot, stays unshadowed. One
// whose UVs the projector reproduces is a prerendered BAT prop and casts
// through the projector. Any other casts through its own UVs.
//
// RedisplayWorldRect does what RedisplayStaticOverlay (0x00737D40) does: it
// calls the terrain's RedisplayTerrain (vtable +0x110, 0x00749850) for a
// rectangle of cells, which queues a partial static pass.

#include "ShadowRegistry.h"

#include "Diagnostics.h"
#include "GameVersion.h"
#include "LiveShadowHooks.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <intrin.h>
#include <windows.h>

namespace nSCD3D11::ShadowRegistry {
	namespace {
		constexpr uintptr_t kAddShadowSlotVA = 0x00AB3A14;
		constexpr uintptr_t kAddShadowVA = 0x0073C0D0;
		constexpr uintptr_t kRemoveOverlaySlotVA = 0x00AB3A24;
		constexpr uintptr_t kRemoveOverlayVA = 0x007388C0;
		constexpr uintptr_t kDrawShadowsVA = 0x00736BF0;
		constexpr uintptr_t kDrawShadowsRejoinVA = 0x00736BF6;
		constexpr uintptr_t kDrawShadowsRoughVA = 0x00737870;
		constexpr uintptr_t kDrawShadowsRoughRejoinVA = 0x00737876;
		constexpr uintptr_t kMeshStashVA = 0x00491894;
		constexpr uintptr_t kMeshStashRejoinVA = 0x0049189F;
		// SC4 renderer. +0x24 returns the lighting manager (DrawShadows 0x00736C49).
		constexpr uintptr_t kRendererPointerVA = 0x00B43DD0;
		constexpr size_t kRendererLightingSlot = 0x24 / sizeof(void *);
		// Lighting manager vtable 0x00ABB530 +0x34, GetShadowDirection 0x007D6B50.
		constexpr size_t kLightingShadowDirectionSlot = 0x34 / sizeof(void *);
		// GetShadowParams (0x007D6B70) only copies these globals out.
		constexpr uintptr_t kShadowStrengthVA = 0x00B0DC78;
		constexpr uintptr_t kShadowAlphaScaleVA = 0x00B4E330;
		constexpr uintptr_t kShadowColourVA = 0x00B4E34C;

		constexpr ptrdiff_t kManagerRecords = 0x98;
		constexpr size_t kRecordSize = 0xB4;
		constexpr ptrdiff_t kRecordFlags = 0x04;
		constexpr ptrdiff_t kRecordProjector = 0x1C;
		constexpr ptrdiff_t kRecordBinding = 0xAC;
		// RedisplayStaticOverlay (0x00737D40) redisplays a record through the
		// terrain at manager +0x88, passing manager +0x16 as updateView.
		constexpr ptrdiff_t kManagerTerrain = 0x88;
		constexpr ptrdiff_t kManagerUpdateView = 0x16;
		// cSTETerrain vtable 0x00AB3D18 (cISTETerrainMap first): +0x18 CellWidth
		// (float, 16), +0x1C CellCountX, +0x20 CellCountZ, +0x34 GetAltitude(x, z)
		// (after GetAltitude(index): MSVC reverses overloads), +0x110
		// RedisplayTerrain(updateTerrain, updateView, rect, flags) 0x00749850,
		// whose rect is {x1, z1, x2, z2} in cells.
		constexpr size_t kTerrainCellWidthSlot = 0x18 / sizeof(void *);
		constexpr size_t kTerrainCellCountXSlot = 0x1C / sizeof(void *);
		constexpr size_t kTerrainCellCountZSlot = 0x20 / sizeof(void *);
		constexpr size_t kTerrainAltitudeSlot = 0x34 / sizeof(void *);
		constexpr size_t kTerrainRedisplaySlot = 0x110 / sizeof(void *);
		constexpr uint32_t kShadowDrawFlags = 9;
		constexpr uint32_t kWrapFlag = 2;

		// CreateOccupantShadow's body stack pointer B. At the AddShadow call the
		// six arguments and the return address sit below it. At the stash site
		// the six arguments of the position extraction do.
		constexpr ptrdiff_t kReturnSlotToFrame = 0x1C;
		constexpr ptrdiff_t kStashToFrame = 0x18;
		constexpr ptrdiff_t kFramePositions = 0x24;
		constexpr ptrdiff_t kFrameTransform = 0x58;
		constexpr ptrdiff_t kFrameUVs = 0xA8;

		// Relaxed props - those without Is Ground Model (0x8A5E5DB8), which SC4
		// never shadows - are sorted by what their mesh shows. One whose lowest
		// point floats this far above the terrain is not a ground model at all
		// (the building zots are such props) and casts nothing, as in vanilla.
		constexpr float kFloatingPropHeight = 2.0f;
		// UVs the projector reproduces this closely are a prerendered view (a BAT
		// prop without the flag), which casts through the projector like any BAT
		// caster. Anything else is a real unwrap and casts through its own UVs.
		constexpr double kPrerenderedResidual = 0.01;

		constexpr uint32_t kMaxVertices = 0xFFFF;
		constexpr uint32_t kMaxPrimitiveRuns = 4096;
		constexpr uint32_t kMaxIndices = 0x40000;
		constexpr size_t kMaxIdsPerPass = 1u << 20;
		constexpr size_t kMaxPatchLength = 11;

		struct PatchSite {
			uintptr_t virtualAddress = 0;
			size_t length = 0;
			std::array<uint8_t, kMaxPatchLength> expected{};
			std::array<uint8_t, kMaxPatchLength> replacement{};
			std::array<uint8_t, kMaxPatchLength> saved{};
			uint8_t *address = nullptr;
			bool installed = false;
		};

		// cS3DTransform as built at 0x004915FE-0x0049166A. Flag byte 1 bit 4
		// means the rotation applies, bit 2 the uniform scale. The matrix is
		// row-major (world = R * local), which the Phase 1 capture confirms.
		struct Transform {
			uint8_t flags[2];
			uint8_t padding[2];
			float rotation[9];
			float translation[3];
			float scale;
		};
		static_assert(sizeof(Transform) == 0x38, "cS3DTransform must stay 56 bytes");

		struct MeshHeader {
			void const *vertices;
			void const *indexHolder;
			void const *primitives;
		};

		struct PrimitiveRun {
			uint32_t primitive;
			uint32_t start;
			uint32_t count;
		};

		struct IdList {
			int32_t *begin;
			int32_t *end;
			int32_t *capacity;
		};

		using AddShadowFunction = int32_t(__thiscall *)(
			void *, void const *, int32_t, void const *, void const *, void const *, void const *);
		using RemoveOverlayFunction = void(__thiscall *)(void *, int32_t);
		using DrawShadowsFunction = void(__thiscall *)(void *, float const *, void *, IdList *);

		PatchSite gAddShadowSlot;
		PatchSite gRemoveOverlaySlot;
		PatchSite gDrawShadowsEntry;
		PatchSite gDrawShadowsRoughEntry;
		PatchSite gMeshStash;

		bool gInstalled = false;
		AddShadowFunction gOriginalAddShadow = nullptr;
		RemoveOverlayFunction gOriginalRemoveOverlay = nullptr;
		uintptr_t gDrawShadowsRejoin = 0;
		uintptr_t gDrawShadowsRoughRejoin = 0;
		uintptr_t gMeshStashRejoin = 0;
		uintptr_t gRendererPointer = 0;
		uintptr_t gShadowStrength = 0;
		uintptr_t gShadowAlphaScale = 0;
		uintptr_t gShadowColour = 0;

		// Written by the stash site and read by the AddShadow hook further down
		// the same CreateOccupantShadow iteration. Plain globals: the frame
		// pointer is compared on use, so a stale or foreign value only costs the
		// index recovery, never correctness.
		void const *gStashedMesh = nullptr;
		uint8_t const *gStashedFrame = nullptr;
		// Set by LiveShadowHooks' site D stub for a relaxed prop, consumed by
		// the AddShadow call that follows on the same thread.
		thread_local bool tExpectMeshCaster = false;

		std::mutex gMutex;
		// A registered caster, with copies of what the gather reads for every id
		// so that culling one does not touch the Caster itself.
		struct Entry {
			std::shared_ptr<Caster const> caster; // null: no caster at this id
			void const *binding = nullptr;
			float low[3]{};
			float high[3]{};
			float baseHeight = 0.0f;
			// The gPassSerial of the pass this caster was last gathered into.
			uint32_t passSerial = 0;
			bool suppressOnly = false;
		};
		// Indexed by overlay id: AddShadow's ids are slots in the manager's
		// record array, so they are small and dense, and a gather that looks up
		// thousands of them per pass reads an array rather than chasing hash
		// nodes. Ids at or above kMaxCasterIds are left to the native draw.
		constexpr uint32_t kMaxCasterIds = 1u << 20;
		std::vector<Entry> gCasters;
		size_t gCasterCount = 0;
		uint32_t gHighestId = 0;

		// gMutex held. True when there was a caster at id.
		bool EraseCaster(int32_t id) {
			if (id < 0 || static_cast<uint32_t>(id) >= gCasters.size() || !gCasters[id].caster) return false;
			gCasters[id] = Entry{};
			--gCasterCount;
			return true;
		}

		void ClearCasters(bool release) {
			if (release) std::vector<Entry>().swap(gCasters);
			else gCasters.clear();
			gCasterCount = 0;
		}
		void const *gManager = nullptr;
		Pass gPass;
		bool gPassValid = false;
		// Bumped whenever gPass starts over, so a caster joins each pass once.
		uint32_t gPassSerial = 1;

		unsigned gRegistered = 0;
		unsigned gMeshCasters = 0;
		unsigned gMeshFallbacks = 0;
		unsigned gFloatingProps = 0;
		unsigned gPrerenderedProps = 0;
		unsigned gWithIndices = 0;
		unsigned gQuadOnly = 0;
		unsigned gRemoved = 0;
		unsigned gStaleRecords = 0;
		unsigned gPasses = 0;
		unsigned gDrawCalls = 0;
		unsigned gGathered = 0;
		unsigned gNotGathered = 0;
		Stats gStats;
		DWORD gDrawThread = 0;

		// The driver's current projection (NoteProjection), render thread only.
		float gProjection[16]{};
		bool gProjectionKnown = false;

		// A ground quad sits this far above the occupant's placement height, so
		// the terrain under it is behind it by more than the composite bias
		// along the 45-degree sun.
		constexpr float kGroundLiftWorld = 0.2f;

		bool GroundQuad(float const low[3], float const high[3], float baseHeight, float const sun[3],
		                float quad[4][3]) {
			if (sun[1] > -1.0e-3f) return false;
			float const height = baseHeight + kGroundLiftWorld;
			// The box's top and bottom corners land this far along the sun. The
			// footprint spans the box and both landings.
			float const alongLow = (height - low[1]) / sun[1];
			float const alongHigh = (height - high[1]) / sun[1];
			float footLow[2]{}, footHigh[2]{};
			for (unsigned axis = 0; axis < 2; ++axis) {
				unsigned const world = axis * 2;
				float const a = sun[world] * alongLow, b = sun[world] * alongHigh;
				footLow[axis] = low[world] + (std::min)({0.0f, a, b});
				footHigh[axis] = high[world] + (std::max)({0.0f, a, b});
			}
			float const corners[4][2]{
				{footLow[0], footLow[1]}, {footHigh[0], footLow[1]}, {footHigh[0], footHigh[1]}, {footLow[0], footHigh[1]}
			};
			for (unsigned index = 0; index < 4; ++index) {
				quad[index][0] = corners[index][0];
				quad[index][1] = height;
				quad[index][2] = corners[index][1];
			}
			return true;
		}

		bool ReachOutside(float const low[3], float const high[3], bool hasQuad, float const quad[4][3],
		                  float const *worldToClip) {
			float reachLow[3]{low[0], low[1], low[2]};
			float reachHigh[3]{high[0], high[1], high[2]};
			if (hasQuad) {
				for (unsigned axis = 0; axis < 3; ++axis) {
					reachLow[axis] = (std::min)(reachLow[axis], quad[0][axis]);
					reachHigh[axis] = (std::max)(reachHigh[axis], quad[2][axis]);
				}
			}
			// The matrix is affine, so the box's NDC extent is its centre and
			// half-size mapped through it.
			for (unsigned axis = 0; axis < 2; ++axis) {
				float centre = worldToClip[12 + axis], extent = 0.0f;
				for (unsigned k = 0; k < 3; ++k) {
					centre += worldToClip[k * 4 + axis] * (reachLow[k] + reachHigh[k]) * 0.5f;
					extent += std::fabs(worldToClip[k * 4 + axis]) * (reachHigh[k] - reachLow[k]) * 0.5f;
				}
				if (centre + extent < -1.0f || centre - extent > 1.0f) return true;
			}
			return false;
		}

		// Inverse of an affine column-major matrix. False for any other.
		bool InvertAffine(float const *m, float out[16]) {
			if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) return false;
			float const c0[3]{m[0], m[1], m[2]}, c1[3]{m[4], m[5], m[6]}, c2[3]{m[8], m[9], m[10]};
			auto cross = [](float const *a, float const *b, float *result) {
				result[0] = a[1] * b[2] - a[2] * b[1];
				result[1] = a[2] * b[0] - a[0] * b[2];
				result[2] = a[0] * b[1] - a[1] * b[0];
			};
			float rows[3][3];
			cross(c1, c2, rows[0]);
			cross(c2, c0, rows[1]);
			cross(c0, c1, rows[2]);
			float const determinant = c0[0] * rows[0][0] + c0[1] * rows[0][1] + c0[2] * rows[0][2];
			if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-12f) return false;
			for (unsigned row = 0; row < 3; ++row) {
				for (unsigned column = 0; column < 3; ++column) out[column * 4 + row] = rows[row][column] / determinant;
				out[12 + row] = -(out[row] * m[12] + out[4 + row] * m[13] + out[8 + row] * m[14]);
				out[row * 4 + 3] = 0.0f;
			}
			out[15] = 1.0f;
			return true;
		}

		double Milliseconds(LARGE_INTEGER const &start) {
			static LARGE_INTEGER const frequency = [] {
				LARGE_INTEGER value{};
				QueryPerformanceFrequency(&value);
				return value;
			}();
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			return frequency.QuadPart != 0
				       ? static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)
				       : 0.0;
		}

		// Why a DrawShadows call was left entirely to the native draw; each
		// reason is logged once.
		void NoteNotGathered(char const *reason, void const *manager) {
			++gNotGathered;
			static std::unordered_set<char const *> logged;
			if (logged.insert(reason).second) {
				Log(LogCategory::Initialization, "shadow registry: DrawShadows call left native (%s, manager %p, registry %p)",
				    reason, manager, gManager);
			}
		}

		// ------------------------------------------------------------------
		// Guarded access to game memory
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

		bool SafeShadowDirection(uintptr_t rendererPointer, float direction[3]) {
			__try {
				void *const renderer = *reinterpret_cast<void **>(rendererPointer);
				if (renderer == nullptr) return false;
				using GetLighting = void *(__thiscall *)(void *);
				void *const lighting =
					reinterpret_cast<GetLighting>((*reinterpret_cast<void ***>(renderer))[kRendererLightingSlot])(
						renderer);
				if (lighting == nullptr) return false;
				using GetDirection = float *(__thiscall *)(void *, float *);
				reinterpret_cast<GetDirection>(
					(*reinterpret_cast<void ***>(lighting))[kLightingShadowDirectionSlot])(lighting, direction);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		struct TerrainGrid {
			float cellWidth;
			uint32_t cellsX;
			uint32_t cellsZ;
		};

		bool SafeTerrainGrid(void *terrain, TerrainGrid &grid) {
			__try {
				void **const vtable = *reinterpret_cast<void ***>(terrain);
				grid.cellWidth = reinterpret_cast<float(__thiscall *)(void *)>(vtable[kTerrainCellWidthSlot])(terrain);
				grid.cellsX = reinterpret_cast<uint32_t(__thiscall *)(void *)>(vtable[kTerrainCellCountXSlot])(terrain);
				grid.cellsZ = reinterpret_cast<uint32_t(__thiscall *)(void *)>(vtable[kTerrainCellCountZSlot])(terrain);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeRedisplayTerrain(void *terrain, bool updateView, int32_t const rect[4]) {
			__try {
				using Redisplay = void(__thiscall *)(void *, bool, bool, int32_t const *, uint32_t);
				reinterpret_cast<Redisplay>((*reinterpret_cast<void ***>(terrain))[kTerrainRedisplaySlot])(
					terrain, false, updateView, rect, 0);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeTerrainAltitude(void *terrain, float x, float z, float &altitude) {
			__try {
				using GetAltitude = float(__thiscall *)(void *, float, float);
				altitude = reinterpret_cast<GetAltitude>((*reinterpret_cast<void ***>(terrain))[kTerrainAltitudeSlot])(
					terrain, x, z);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeCopy(void const *, void *, size_t) { return false; }
		bool SafeShadowDirection(uintptr_t, float *) { return false; }
		struct TerrainGrid {
			float cellWidth;
			uint32_t cellsX;
			uint32_t cellsZ;
		};
		bool SafeTerrainGrid(void *, TerrainGrid &) { return false; }
		bool SafeRedisplayTerrain(void *, bool, int32_t const *) { return false; }
		bool SafeTerrainAltitude(void *, float, float, float &) { return false; }
#endif

		template<typename T>
		bool Read(void const *source, T &value) {
			return SafeCopy(source, &value, sizeof(T));
		}

		// The terrain and view-update flag of the overlay manager the registry
		// belongs to, i.e. of the loaded city.
		bool CityTerrain(void *&terrain, bool &updateView) {
			void const *manager = nullptr;
			{
				std::lock_guard<std::mutex> lock(gMutex);
				manager = gManager;
			}
			uint8_t update = 0;
			if (manager == nullptr ||
			    !Read(static_cast<uint8_t const *>(manager) + kManagerTerrain, terrain) || terrain == nullptr ||
			    !Read(static_cast<uint8_t const *>(manager) + kManagerUpdateView, update))
				return false;
			updateView = update != 0;
			return true;
		}

		uint8_t const *RecordAddress(void const *manager, int32_t id) {
			uint8_t const *records = nullptr;
			if (!Read(static_cast<uint8_t const *>(manager) + kManagerRecords, records) || records == nullptr)
				return nullptr;
			return records + static_cast<size_t>(id) * kRecordSize;
		}

		// ------------------------------------------------------------------
		// Capture
		// ------------------------------------------------------------------

		void ToWorld(Transform const &transform, float const local[3], float world[3]) {
			float rotated[3]{local[0], local[1], local[2]};
			if ((transform.flags[1] & 4) != 0) {
				for (unsigned row = 0; row < 3; ++row) {
					rotated[row] = transform.rotation[row * 3 + 0] * local[0] +
					               transform.rotation[row * 3 + 1] * local[1] +
					               transform.rotation[row * 3 + 2] * local[2];
				}
			}
			float const scale = (transform.flags[1] & 2) != 0 ? transform.scale : 1.0f;
			for (unsigned axis = 0; axis < 3; ++axis)
				world[axis] = rotated[axis] * scale + transform.translation[axis];
		}

		// Reads the mesh the stash site recorded, provided it belongs to the
		// CreateOccupantShadow frame this AddShadow was called from. Every value
		// is checked against what AddShadow itself received.
		bool RecoverIndices(uint8_t const *returnSlot, uint32_t count, void const *positions, void const *uvs,
		                    void const *transform, std::vector<uint32_t> &triangles) {
			uint8_t const *const frame = returnSlot + kReturnSlotToFrame;
			if (gStashedFrame == nullptr || gStashedFrame + kStashToFrame != frame) return false;
			void const *framePositions = nullptr;
			void const *frameUVs = nullptr;
			if (!Read(frame + kFramePositions, framePositions) || !Read(frame + kFrameUVs, frameUVs) ||
			    framePositions != positions || frameUVs != uvs || frame + kFrameTransform != transform)
				return false;

			MeshHeader mesh{};
			uint16_t vertexCount = 0;
			if (!Read(gStashedMesh, mesh) || mesh.vertices == nullptr || mesh.primitives == nullptr ||
			    !Read(static_cast<uint8_t const *>(mesh.vertices) + 6, vertexCount) || vertexCount != count)
				return false;
			uint16_t const *indexBase = nullptr;
			if (mesh.indexHolder != nullptr && (!Read(mesh.indexHolder, indexBase) || indexBase == nullptr))
				return false;
			PrimitiveRun const *runs = nullptr;
			PrimitiveRun const *runsEnd = nullptr;
			if (!Read(static_cast<uint8_t const *>(mesh.primitives) + 4, runs) ||
			    !Read(static_cast<uint8_t const *>(mesh.primitives) + 8, runsEnd) || runsEnd < runs ||
			    static_cast<size_t>(runsEnd - runs) > kMaxPrimitiveRuns)
				return false;

			std::vector<uint32_t> source;
			std::vector<uint16_t> packed;
			for (PrimitiveRun const *cursor = runs; cursor != runsEnd; ++cursor) {
				PrimitiveRun run{};
				if (!Read(cursor, run) || run.count > kMaxIndices || run.start > kMaxIndices) return false;
				source.resize(run.count);
				if (indexBase != nullptr) {
					packed.resize(run.count);
					if (run.count != 0 &&
					    !SafeCopy(indexBase + run.start, packed.data(), run.count * sizeof(uint16_t)))
						return false;
					std::copy(packed.begin(), packed.end(), source.begin());
				} else {
					for (uint32_t index = 0; index < run.count; ++index) source[index] = run.start + index;
				}
				if (std::any_of(source.begin(), source.end(), [count](uint32_t index) { return index >= count; }))
					return false;
				AppendTriangles(run.primitive, source.data(), run.count, triangles);
				if (triangles.size() > kMaxIndices) return false;
			}
			return !triangles.empty();
		}

		// RMS distance between the mesh's UVs and what the projector gives for
		// the same world positions.
		double ProjectorResidual(Caster const &caster, float const *uvs, uint32_t count) {
			double squared = 0.0;
			for (uint32_t vertex = 0; vertex < count; ++vertex) {
				float const *const world = caster.vertices.data() + vertex * kVertexFloats;
				float const *const m = caster.projector;
				double const du = m[0] * world[0] + m[4] * world[1] + m[8] * world[2] + m[12] - uvs[vertex * 2 + 0];
				double const dv = m[1] * world[0] + m[5] * world[1] + m[9] * world[2] + m[13] - uvs[vertex * 2 + 1];
				squared += du * du + dv * dv;
			}
			return count == 0 ? 0.0 : std::sqrt(squared / count);
		}

		void Register(void *manager, int32_t id, void const *binding, int32_t count, void const *positions,
		              void const *uvs, void const *box, void const *transform, uint8_t const *returnSlot,
		              bool meshCaster) {
			auto erase = [&] {
				std::lock_guard<std::mutex> lock(gMutex);
				EraseCaster(id);
			};
			if (binding == nullptr || positions == nullptr || uvs == nullptr || transform == nullptr || count < 3 ||
			    static_cast<uint32_t>(count) > kMaxVertices || static_cast<uint32_t>(id) >= kMaxCasterIds) {
				erase();
				return;
			}
			uint32_t const vertexCount = static_cast<uint32_t>(count);
			bool const timing = DrawStatsEnabled();
			LARGE_INTEGER start{};
			if (timing) QueryPerformanceCounter(&start);
			auto caster = std::make_shared<Caster>();
			caster->id = static_cast<uint32_t>(id);
			caster->manager = manager;
			caster->binding = binding;

			uint8_t const *const record = RecordAddress(manager, id);
			void const *recordBinding = nullptr;
			Transform xf{};
			std::vector<float> local(static_cast<size_t>(vertexCount) * 3);
			std::vector<float> uvValues(static_cast<size_t>(vertexCount) * 2);
			if (record == nullptr || !SafeCopy(record + kRecordProjector, caster->projector, sizeof(caster->projector)) ||
			    !Read(record + kRecordBinding, recordBinding) || recordBinding != binding || !Read(transform, xf) ||
			    !SafeCopy(positions, local.data(), local.size() * sizeof(float)) ||
			    !SafeCopy(uvs, uvValues.data(), uvValues.size() * sizeof(float))) {
				erase();
				return;
			}

			caster->vertices.resize(static_cast<size_t>(vertexCount) * kVertexFloats);
			for (unsigned axis = 0; axis < 3; ++axis) {
				caster->low[axis] = FLT_MAX;
				caster->high[axis] = -FLT_MAX;
			}
			caster->uvBounds[0] = caster->uvBounds[1] = FLT_MAX;
			caster->uvBounds[2] = caster->uvBounds[3] = -FLT_MAX;
			for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
				float *const world = caster->vertices.data() + vertex * kVertexFloats;
				ToWorld(xf, local.data() + vertex * 3, world);
				for (unsigned axis = 0; axis < 3; ++axis) {
					caster->low[axis] = (std::min)(caster->low[axis], world[axis]);
					caster->high[axis] = (std::max)(caster->high[axis], world[axis]);
				}
				for (unsigned axis = 0; axis < 2; ++axis) {
					caster->uvBounds[axis] = (std::min)(caster->uvBounds[axis], uvValues[vertex * 2 + axis]);
					caster->uvBounds[axis + 2] = (std::max)(caster->uvBounds[axis + 2], uvValues[vertex * 2 + axis]);
				}
			}
			caster->baseHeight = xf.translation[1];
			if (!std::isfinite(caster->low[0]) || !std::isfinite(caster->high[0]) ||
			    !std::isfinite(caster->uvBounds[0]) || !std::isfinite(caster->uvBounds[2])) {
				erase();
				return;
			}

			bool const indexed = RecoverIndices(returnSlot, vertexCount, positions, uvs, transform, caster->indices);
			if (!indexed) caster->indices.clear();
			if (meshCaster) {
				void *terrain = nullptr;
				float ground = caster->low[1];
				if (Read(static_cast<uint8_t const *>(manager) + kManagerTerrain, terrain) && terrain != nullptr) {
					float altitude = 0.0f;
					if (SafeTerrainAltitude(terrain, (caster->low[0] + caster->high[0]) * 0.5f,
					                        (caster->low[2] + caster->high[2]) * 0.5f, altitude) &&
					    std::isfinite(altitude))
						ground = altitude;
				}
				double const residual = ProjectorResidual(*caster, uvValues.data(), vertexCount);
				bool const floating = caster->low[1] - ground > kFloatingPropHeight;
				if (floating) {
					caster->suppressOnly = true;
					++gFloatingProps;
				} else if (residual < kPrerenderedResidual) {
					++gPrerenderedProps;
				} else if (indexed) {
					caster->meshUVs = true;
					for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
						std::memcpy(caster->vertices.data() + vertex * kVertexFloats + 3, uvValues.data() + vertex * 2,
						            sizeof(float) * 2);
					++gMeshCasters;
				} else {
					// Without triangles the UVs cannot be drawn. The prop casts
					// through the per-draw path instead, and its record - whose
					// projector would stamp a slab - is only kept out of DrawShadows.
					caster->suppressOnly = true;
					LiveShadowHooks::RegisterPropMesh(vertexCount, positions, uvs);
					++gMeshFallbacks;
				}
			}

			std::lock_guard<std::mutex> lock(gMutex);
			if (gManager != manager) {
				// A new overlay manager is a new city: every id belongs to it now.
				if (gManager != nullptr) {
					Log(LogCategory::Initialization, "shadow registry: overlay manager changed, %u casters dropped",
					    static_cast<unsigned>(gCasterCount));
				}
				ClearCasters(false);
				++gPassSerial;
				gPass.casters.clear();
				gManager = manager;
			}
			Entry fresh;
			fresh.binding = caster->binding;
			std::memcpy(fresh.low, caster->low, sizeof(fresh.low));
			std::memcpy(fresh.high, caster->high, sizeof(fresh.high));
			fresh.baseHeight = caster->baseHeight;
			fresh.suppressOnly = caster->suppressOnly;
			fresh.caster = std::move(caster);
			uint32_t const slot = static_cast<uint32_t>(id);
			if (slot >= gCasters.size()) gCasters.resize((std::max)(slot + 1, static_cast<uint32_t>(gCasters.size()) * 3 / 2));
			if (!gCasters[slot].caster) ++gCasterCount;
			gCasters[slot] = std::move(fresh);
			gHighestId = (std::max)(gHighestId, slot);
			++gRegistered;
			if (timing) {
				++gStats.registrations;
				if (GetCurrentThreadId() != gDrawThread) ++gStats.offThreadRegistrations;
				gStats.registerMs += Milliseconds(start);
			}
			if (indexed) ++gWithIndices;
			else ++gQuadOnly;
			static bool loggedFirst = false;
			if (!loggedFirst) {
				loggedFirst = true;
				Log(LogCategory::Initialization, "shadow registry: first caster registered (id %d, %d vertices, %s)",
				    id, count, indexed ? "indexed" : "ground quad only");
			}
		}

		// Called by the DrawShadows call-site hooks with the pass's overlay ids.
		// Registry-owned records are moved into the pass. Every other id, and
		// any record that no longer matches what was registered, is kept for the
		// native draw. False leaves the caller to draw the original list.
		bool Gather(void *manager, float const *eyeToWorld, IdList const *ids, std::vector<int32_t> &kept) {
			kept.clear();
			IdList list{};
			if (!Read(ids, list) || list.begin == nullptr || list.end <= list.begin) {
				NoteNotGathered("empty or unreadable id list", manager);
				return false;
			}
			size_t const count = static_cast<size_t>(list.end - list.begin);
			if (count > kMaxIdsPerPass) {
				NoteNotGathered("id list too long", manager);
				return false;
			}
			kept.resize(count);
			if (!SafeCopy(list.begin, kept.data(), count * sizeof(int32_t))) {
				NoteNotGathered("id list unreadable", manager);
				return false;
			}

			float matrix[16]{};
			float sun[3]{};
			float colour[3]{};
			float strength = 0.0f;
			float alphaScale = 0.0f;
			bool const haveMatrix = eyeToWorld != nullptr && SafeCopy(eyeToWorld, matrix, sizeof(matrix));
			bool const haveSun = SafeShadowDirection(gRendererPointer, sun);
			Read(reinterpret_cast<void const *>(gShadowStrength), strength);
			Read(reinterpret_cast<void const *>(gShadowAlphaScale), alphaScale);
			SafeCopy(reinterpret_cast<void const *>(gShadowColour), colour, sizeof(colour));
			if (!haveMatrix) {
				NoteNotGathered("eye-to-world matrix unreadable", manager);
				return false;
			}

			// Rows x and y of projection * world-to-eye, for culling. Only an
			// orthographic projection with a known sun is used. Anything else
			// hands every caster over, as before.
			float worldToClip[16]{};
			float sunWorld[3]{sun[0], sun[1], sun[2]};
			float worldToEye[16]{};
			float const sunLength = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
			float const *const p = gProjection;
			bool const cull = haveSun && sunLength > 1.0e-6f && gProjectionKnown && p[3] == 0.0f && p[7] == 0.0f &&
			                  p[11] == 0.0f && p[15] == 1.0f && InvertAffine(matrix, worldToEye);
			if (cull) {
				for (float &value: sunWorld) value /= sunLength;
				for (unsigned column = 0; column < 4; ++column)
					for (unsigned row = 0; row < 2; ++row)
						worldToClip[column * 4 + row] = p[row] * worldToEye[column * 4] + p[4 + row] * worldToEye[column * 4 + 1] +
						                                p[8 + row] * worldToEye[column * 4 + 2] +
						                                p[12 + row] * worldToEye[column * 4 + 3];
			}

			std::lock_guard<std::mutex> lock(gMutex);
			if (manager != gManager) {
				NoteNotGathered("not the registry's overlay manager", manager);
				return false;
			}
			if (gCasterCount == 0) {
				NoteNotGathered("registry empty", manager);
				return false;
			}
			uint8_t const *records = nullptr;
			if (!Read(static_cast<uint8_t const *>(manager) + kManagerRecords, records) || records == nullptr) {
				NoteNotGathered("record table unreadable", manager);
				return false;
			}
			++gGathered;
			size_t write = 0;
			for (size_t read = 0; read < kept.size(); ++read) {
				int32_t const id = kept[read];
				if (id < 0 || static_cast<uint32_t>(id) >= gCasters.size() || !gCasters[id].caster) {
					kept[write++] = id;
					continue;
				}
				Entry &entry = gCasters[id];
				// Culled before the record is read, from the entry alone. A record
				// SC4 reused without RemoveOverlay is then only noticed once it is
				// in view. None has been seen in practice.
				if (cull) {
					float quad[4][3];
					bool const hasQuad = GroundQuad(entry.low, entry.high, entry.baseHeight, sunWorld, quad);
					if (ReachOutside(entry.low, entry.high, hasQuad, quad, worldToClip)) continue;
				}
				// The in-use word, the flags and the binding, in one guarded read.
				uint8_t header[kRecordBinding + sizeof(void *)];
				int32_t inUse = 0;
				uint32_t flags = 0;
				void const *binding = nullptr;
				void const *textureObject = nullptr;
				uint32_t texture = 0;
				bool const readable = SafeCopy(records + static_cast<size_t>(id) * kRecordSize, header, sizeof(header));
				if (readable) {
					std::memcpy(&inUse, header, sizeof(inUse));
					std::memcpy(&flags, header + kRecordFlags, sizeof(flags));
					std::memcpy(&binding, header + kRecordBinding, sizeof(binding));
				}
				if (!readable || inUse != -1 || binding != entry.binding) {
					// The slot was freed or reused by something that never went
					// through AddShadow, e.g. a decal. It is not ours any more.
					++gStaleRecords;
					EraseCaster(id);
					kept[write++] = id;
					continue;
				}
				if ((flags & kShadowDrawFlags) != kShadowDrawFlags) continue; // disabled: native skips it too
				if (entry.suppressOnly) continue;
				if (!Read(binding, textureObject) || textureObject == nullptr || !Read(textureObject, texture)) {
					kept[write++] = id;
					continue;
				}
				if (entry.passSerial != gPassSerial) {
					entry.passSerial = gPassSerial;
					gPass.casters.push_back(PassCaster{entry.caster, texture, (flags & kWrapFlag) != 0});
				}
			}
			kept.resize(write);
			std::memcpy(gPass.eyeToWorld, matrix, sizeof(matrix));
			if (haveSun) std::memcpy(gPass.sunDirection, sun, sizeof(sun));
			gPass.sunValid = haveSun;
			std::memcpy(gPass.colour, colour, sizeof(colour));
			gPass.strength = strength;
			gPass.alphaScale = alphaScale;
			gPass.culled = cull;
			if (cull) std::memcpy(gPass.projection, gProjection, sizeof(gPass.projection));
			gPassValid = true;
			return true;
		}

	} // namespace

	bool GroundQuad(Caster const &caster, float const sun[3], float quad[4][3]) {
		return GroundQuad(caster.low, caster.high, caster.baseHeight, sun, quad);
	}

	bool ReachOutside(Caster const &caster, bool hasQuad, float const quad[4][3], float const *worldToClip) {
		return ReachOutside(caster.low, caster.high, hasQuad, quad, worldToClip);
	}

	void NoteProjection(float const projection[16]) {
		std::memcpy(gProjection, projection, sizeof(gProjection));
		gProjectionKnown = true;
	}

	Stats TakeStats() {
		std::lock_guard<std::mutex> lock(gMutex);
		Stats const taken = gStats;
		gStats = Stats{};
		return taken;
	}

	bool AppendTriangles(uint32_t primitive, uint32_t const *indices, uint32_t count,
	                     std::vector<uint32_t> &triangles) {
		switch (primitive) {
			case 0: // triangles
				for (uint32_t i = 0; i + 2 < count; i += 3)
					triangles.insert(triangles.end(), {indices[i], indices[i + 1], indices[i + 2]});
				return count >= 3;
			case 1: // triangle strip. The shadow pass does not cull, so winding is irrelevant
				for (uint32_t i = 0; i + 2 < count; ++i)
					triangles.insert(triangles.end(), {indices[i], indices[i + 1], indices[i + 2]});
				return count >= 3;
			case 2: // triangle fan
				for (uint32_t i = 1; i + 1 < count; ++i)
					triangles.insert(triangles.end(), {indices[0], indices[i], indices[i + 1]});
				return count >= 3;
			case 6: // quads
				for (uint32_t i = 0; i + 3 < count; i += 4)
					triangles.insert(triangles.end(), {
						                 indices[i], indices[i + 1], indices[i + 2],
						                 indices[i], indices[i + 2], indices[i + 3]
					                 });
				return count >= 4;
			case 7: // quad strip
				for (uint32_t i = 0; i + 3 < count; i += 2)
					triangles.insert(triangles.end(), {
						                 indices[i], indices[i + 1], indices[i + 2],
						                 indices[i + 2], indices[i + 1], indices[i + 3]
					                 });
				return count >= 4;
			default:
				return false;
		}
	}

	// ----------------------------------------------------------------------
	// Hook bodies
	// ----------------------------------------------------------------------

	namespace {
		int32_t __fastcall AddShadowHook(void *manager, void *, void const *binding, int32_t count,
		                                 void const *positions, void const *uvs, void const *box,
		                                 void const *transform) {
			bool const meshCaster = tExpectMeshCaster;
			tExpectMeshCaster = false;
			int32_t const id = gOriginalAddShadow(manager, binding, count, positions, uvs, box, transform);
			if (id >= 0) {
				try {
					Register(manager, id, binding, count, positions, uvs, box, transform,
					         static_cast<uint8_t const *>(_AddressOfReturnAddress()), meshCaster);
				} catch (std::bad_alloc const &) {
					std::lock_guard<std::mutex> lock(gMutex);
					EraseCaster(id);
				}
			} else if (meshCaster && count > 0) {
				// CalcShadowProjection rejected the fit, so there is no record to
				// keep: the prop casts through the per-draw path as before.
				LiveShadowHooks::RegisterPropMesh(static_cast<uint32_t>(count), positions, uvs);
				++gMeshFallbacks;
			}
			return id;
		}

		void __fastcall RemoveOverlayHook(void *manager, void *, int32_t id) {
			gOriginalRemoveOverlay(manager, id);
			// Negative ids are handles into the manager's +0xAC table. AddShadow
			// never returns one, and a group's members come back through here.
			if (id < 0) return;
			std::lock_guard<std::mutex> lock(gMutex);
			if (manager == gManager && EraseCaster(id)) ++gRemoved;
		}

		void DrawFiltered(DrawShadowsFunction original, void *manager, float const *eyeToWorld, void *drawContext,
		                  IdList *ids) {
			// Only the render thread draws overlays, so one scratch list is enough.
			static std::vector<int32_t> kept;
			++gDrawCalls;
			bool const timing = DrawStatsEnabled();
			LARGE_INTEGER start{};
			if (timing) QueryPerformanceCounter(&start);
			bool gathered = false;
			try {
				gathered = Gather(manager, eyeToWorld, ids, kept);
			} catch (std::bad_alloc const &) {
			}
			double const gatherMs = timing ? Milliseconds(start) : 0.0;
			if (timing) QueryPerformanceCounter(&start);
			if (!gathered) {
				original(manager, eyeToWorld, drawContext, ids);
			} else {
				// DrawShadows reads only begin and end, and returns at once when they
				// are equal.
				IdList filtered{};
				if (!kept.empty()) filtered = IdList{kept.data(), kept.data() + kept.size(), kept.data() + kept.size()};
				original(manager, eyeToWorld, drawContext, &filtered);
			}
			if (timing) {
				double const nativeMs = Milliseconds(start);
				std::lock_guard<std::mutex> lock(gMutex);
				gDrawThread = GetCurrentThreadId();
				gStats.gatherMs += gatherMs;
				gStats.nativeDrawMs += nativeMs;
			}
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		// Entered by a call carrying the original arguments, so the replayed
		// SUB ESP builds the original frame and the body's RET 0xC returns here.
		__declspec(naked) void DrawShadowsOriginal() {
			__asm {
				sub  esp, 0x1c4
				jmp  dword ptr [gDrawShadowsRejoin]
			}
		}

		__declspec(naked) void DrawShadowsRoughOriginal() {
			__asm {
				sub  esp, 0x9c
				jmp  dword ptr [gDrawShadowsRoughRejoin]
			}
		}

		void __fastcall DrawShadowsHook(void *manager, void *, float const *eyeToWorld, void *drawContext,
		                                IdList *ids) {
			DrawFiltered(reinterpret_cast<DrawShadowsFunction>(&DrawShadowsOriginal), manager, eyeToWorld,
			             drawContext, ids);
		}

		void __fastcall DrawShadowsRoughHook(void *manager, void *, float const *eyeToWorld, void *drawContext,
		                                     IdList *ids) {
			DrawFiltered(reinterpret_cast<DrawShadowsFunction>(&DrawShadowsRoughOriginal), manager, eyeToWorld,
			             drawContext, ids);
		}

		// Replays the three displaced instructions. ESP is the original's, so the
		// ESP-relative load is unchanged.
		__declspec(naked) void MeshStashStub() {
			__asm {
				mov  dword ptr [gStashedMesh], edi
				mov  dword ptr [gStashedFrame], esp
				mov  eax, dword ptr [edi]
				mov  ecx, dword ptr [eax]
				mov  ebx, dword ptr [esp + 0xc0]
				jmp  dword ptr [gMeshStashRejoin]
			}
		}
#endif

		// ------------------------------------------------------------------
		// Patch plumbing
		// ------------------------------------------------------------------

		void ConfigureSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = expected.size();
			std::copy(expected.begin(), expected.end(), site.expected.begin());
		}

		void ConfigureBranchSite(PatchSite &site, uintptr_t virtualAddress, std::initializer_list<uint8_t> expected,
		                         uint8_t opcode, uint8_t *target, void const *destination) {
			ConfigureSite(site, virtualAddress, expected);
			site.replacement.fill(0x90);
			site.replacement[0] = opcode;
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(destination) -
			                                                 (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(site.replacement.data() + 1, &displacement, sizeof(displacement));
		}

		void ConfigureSlotSite(PatchSite &site, uintptr_t virtualAddress, uintptr_t expected, void const *hook) {
			site = PatchSite{};
			site.virtualAddress = virtualAddress;
			site.length = sizeof(uint32_t);
			uint32_t const value = static_cast<uint32_t>(expected);
			uint32_t const replacement = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hook));
			std::memcpy(site.expected.data(), &value, sizeof(value));
			std::memcpy(site.replacement.data(), &replacement, sizeof(replacement));
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
				Log(LogCategory::Initialization, "shadow registry: not restoring modified site 0x%08lX",
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
		if (!IsGameVersion641()) {
			Log(LogCategory::Initialization, "shadow registry: requires SimCity 4 1.1.641, skipping");
			return false;
		}
		// IsGameVersion641 only accepts an executable at its preferred base, so
		// the game's addresses are used as they are.
		auto const resolve = [](uintptr_t virtualAddress) { return reinterpret_cast<uint8_t *>(virtualAddress); };

		gOriginalAddShadow = reinterpret_cast<AddShadowFunction>(resolve(kAddShadowVA));
		gOriginalRemoveOverlay = reinterpret_cast<RemoveOverlayFunction>(resolve(kRemoveOverlayVA));
		gDrawShadowsRejoin = reinterpret_cast<uintptr_t>(resolve(kDrawShadowsRejoinVA));
		gDrawShadowsRoughRejoin = reinterpret_cast<uintptr_t>(resolve(kDrawShadowsRoughRejoinVA));
		gMeshStashRejoin = reinterpret_cast<uintptr_t>(resolve(kMeshStashRejoinVA));
		gRendererPointer = reinterpret_cast<uintptr_t>(resolve(kRendererPointerVA));
		gShadowStrength = reinterpret_cast<uintptr_t>(resolve(kShadowStrengthVA));
		gShadowAlphaScale = reinterpret_cast<uintptr_t>(resolve(kShadowAlphaScaleVA));
		gShadowColour = reinterpret_cast<uintptr_t>(resolve(kShadowColourVA));

		ConfigureSlotSite(gAddShadowSlot, kAddShadowSlotVA, reinterpret_cast<uintptr_t>(resolve(kAddShadowVA)),
		                  reinterpret_cast<void const *>(&AddShadowHook));
		ConfigureSlotSite(gRemoveOverlaySlot, kRemoveOverlaySlotVA,
		                  reinterpret_cast<uintptr_t>(resolve(kRemoveOverlayVA)),
		                  reinterpret_cast<void const *>(&RemoveOverlayHook));
		ConfigureBranchSite(gDrawShadowsEntry, kDrawShadowsVA, {0x81, 0xEC, 0xC4, 0x01, 0x00, 0x00}, 0xE9,
		                    resolve(kDrawShadowsVA), reinterpret_cast<void const *>(&DrawShadowsHook));
		ConfigureBranchSite(gDrawShadowsRoughEntry, kDrawShadowsRoughVA, {0x81, 0xEC, 0x9C, 0x00, 0x00, 0x00}, 0xE9,
		                    resolve(kDrawShadowsRoughVA), reinterpret_cast<void const *>(&DrawShadowsRoughHook));
		ConfigureBranchSite(gMeshStash, kMeshStashVA,
		                    {0x8B, 0x07, 0x8B, 0x08, 0x8B, 0x9C, 0x24, 0xC0, 0x00, 0x00, 0x00}, 0xE9,
		                    resolve(kMeshStashVA), reinterpret_cast<void const *>(&MeshStashStub));
		PatchSite *const sites[]{
			&gMeshStash, &gRemoveOverlaySlot, &gDrawShadowsEntry, &gDrawShadowsRoughEntry, &gAddShadowSlot
		};
		for (PatchSite *site: sites) site->address = resolve(site->virtualAddress);
		for (PatchSite const *site: sites) {
			if (!BytesMatch(*site)) {
				Log(LogCategory::Initialization, "shadow registry: guard failed at 0x%08lX, skipping",
				    static_cast<unsigned long>(site->virtualAddress));
				return false;
			}
		}
		{
			std::lock_guard<std::mutex> lock(gMutex);
			ClearCasters(true);
			gManager = nullptr;
			gPass = Pass{};
			gPassValid = false;
			++gPassSerial;
		}
		// AddShadow goes last: nothing is registered before the draw filter and
		// the removal hook that keep the registry honest are in place.
		for (PatchSite *site: sites) {
			if (!WriteSite(*site)) {
				Log(LogCategory::Initialization, "shadow registry: failed to write 0x%08lX, restoring",
				    static_cast<unsigned long>(site->virtualAddress));
				for (PatchSite *written: sites) RestoreSite(*written);
				return false;
			}
		}
		gInstalled = true;
		Log(LogCategory::Initialization,
		    "shadow registry installed: AddShadow records render through the live shadow map");
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		if (!gInstalled) return;
		RestoreSite(gAddShadowSlot);
		RestoreSite(gDrawShadowsRoughEntry);
		RestoreSite(gDrawShadowsEntry);
		RestoreSite(gRemoveOverlaySlot);
		RestoreSite(gMeshStash);
		gInstalled = false;
		size_t remaining = 0;
		{
			std::lock_guard<std::mutex> lock(gMutex);
			remaining = gCasterCount;
			ClearCasters(true);
			gManager = nullptr;
			gPass = Pass{};
			gPassValid = false;
			++gPassSerial;
		}
		Log(LogCategory::Initialization,
		    "shadow registry uninstalled: relaxed props: %u True3D kept with their UVs, %u prerendered through the "
		    "projector, %u floating left unshadowed, %u left to per-draw capture",
		    gMeshCasters, gPrerenderedProps, gFloatingProps, gMeshFallbacks);
		Log(LogCategory::Initialization,
		    "shadow registry uninstalled: %u registered (%u indexed, %u ground quad only), %u removed, "
		    "%u stale, %u passes, %u live at exit, highest id %u; DrawShadows calls %u (%u gathered, %u left native)",
		    gRegistered, gWithIndices, gQuadOnly, gRemoved, gStaleRecords, gPasses,
		    static_cast<unsigned>(remaining), gHighestId, gDrawCalls, gGathered, gNotGathered);
	}

	bool Enabled() {
		return gInstalled;
	}

	void ExpectMeshCaster() {
		tExpectMeshCaster = gInstalled;
	}

	bool RedisplayWorldRect(float minX, float minZ, float maxX, float maxZ) {
		void *terrain = nullptr;
		bool updateView = false;
		return gInstalled && CityTerrain(terrain, updateView) &&
		       RedisplayTerrainRect(terrain, updateView, minX, minZ, maxX, maxZ);
	}

	bool RedisplayTerrainRect(void *terrain, bool updateView, float minX, float minZ, float maxX, float maxZ) {
		TerrainGrid grid{};
		if (!gInstalled || terrain == nullptr || !(minX <= maxX) || !(minZ <= maxZ) ||
		    !SafeTerrainGrid(terrain, grid) || !(grid.cellWidth > 0.0f) || grid.cellsX == 0 || grid.cellsZ == 0)
			return false;
		// A cell of margin on every side, clamped to the city: RedisplayTerrain
		// reads the altitudes under the rectangle.
		auto cell = [&](float value, uint32_t count, float round) {
			double const index = std::floor(value / grid.cellWidth + round);
			return static_cast<int32_t>((std::min)((std::max)(index, 0.0), static_cast<double>(count)));
		};
		int32_t const rect[4]{
			cell(minX, grid.cellsX, -1.0f), cell(minZ, grid.cellsZ, -1.0f),
			cell(maxX, grid.cellsX, 2.0f), cell(maxZ, grid.cellsZ, 2.0f)
		};
		if (rect[2] <= rect[0] || rect[3] <= rect[1]) return false;
		return SafeRedisplayTerrain(terrain, updateView, rect);
	}

	bool TakePass(Pass &pass) {
		std::lock_guard<std::mutex> lock(gMutex);
		if (!gPassValid) return false;
		pass = std::move(gPass);
		gPass = Pass{};
		gPassValid = false;
		++gPassSerial;
		++gPasses;
		return true;
	}
} // namespace nSCD3D11::ShadowRegistry
