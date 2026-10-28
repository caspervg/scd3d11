/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// The city side of terrain shadows.
//
// SimCity 4 Deluxe 1.1.641. cSTETerrain (vtable 0x00AB3D18) keeps a 0x24-byte
// record per vertex at +0x6C: the altitude at +0x00, the lighting normal at
// +0x04 and flags at +0x22. Flag 0x3000 means the cell with that lowest corner
// is split along its other diagonal (IsTriangulationFlipped, 0x00741C20).
// +0x28/+0x2C are the cell counts and +0x38/+0x3C the vertex counts.
// GetAltitudes (+0x3C, 0x00742A00) copies an inclusive rectangle without bounds
// checks, so the rectangle is always built from the terrain's own counts. The
// normals and flags are read from the records, and only used when every
// altitude matches GetAltitudes. CellWidth (+0x18, 0x0074C5C0) returns a float
// although cISTETerrainMap declares uint32_t, so it is called through a
// float-typed pointer.
//
// SC4 has no terrain revision counter, so the height field is read again when
// the sun changes or every few seconds, and the map is only rebuilt when
// either differs.
//
// SC4 also darkens terrain and models below its own shadow-height grid.
// Models and most lighting calls ask ShadowDepth_ (0x007D6950), but the
// terrain's vertex colours (GetTerrainVertexColors, 0x007D7F00) read the grid
// themselves. Under these shadows that would darken the same slopes twice, so
// Install makes ShadowDepth_ return 0 and makes the vertex colours always take
// their unshadowed branch. The grids themselves must stay, because BAT shadow
// placement (CalcShadowTerrainRect, 0x007D6CC0) uses them.

#include "TerrainShadows.h"

#include "Diagnostics.h"
#include "LiveShadowHooks.h"

#include "GZServPtrs.h"
#include "SC4Rect.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4RenderProperties.h"
#include "cISTETerrain.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <windows.h>

namespace nSCD3D11::TerrainShadows {
	namespace {
		// Half a terrain cell: shadow edges follow the terrain's 16 m facets
		// closely, and a large city stays under 700 texels a side.
		constexpr float kSpacingCells = 0.5f;
		// The sun's apparent radius for the penumbra, in radians. The real sun
		// is 0.27 degrees. 3 degrees stands in for the sky light that softens
		// real hill shadows, and is the knob if edges look too hard or too soft.
		constexpr float kSunRadius = 3.0f * 0.0174532925f;
		// The sun is view-locked and only moves with a rotation.
		constexpr float kSunTolerance = 1.0e-5f;
		// How often an unchanged sun re-reads the height field to catch terraforming.
		constexpr ULONGLONG kTerrainPollMilliseconds = 2000;
		constexpr size_t kCellWidthSlot = 0x18 / sizeof(void *);
		constexpr uint32_t kMaxCells = 4096;
		constexpr ptrdiff_t kTerrainRecords = 0x6C;
		constexpr size_t kRecordSize = 0x24;
		constexpr ptrdiff_t kRecordNormal = 0x04;
		constexpr ptrdiff_t kRecordFlags = 0x22;
		constexpr uint16_t kFlippedFlags = 0x3000;

		constexpr uintptr_t kImageBase = 0x00400000;

		struct CodePatch {
			uintptr_t address;
			uint8_t guard[9];
			size_t guardSize;
			// Where in the guarded bytes the new bytes go.
			size_t offset;
			uint8_t bytes[5];
			size_t size;
		};

		CodePatch const kPatches[]{
			// ShadowDepth_: PUSH EBX / PUSH ESI / PUSH EDI / MOV EDI,[ESP+0x10] / FLD [EDI],
			// __thiscall, RET 4, float result on the x87 stack. FLDZ / RET 4: never below the shadow.
			{0x007D6950, {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0xD9, 0x07}, 9, 0, {0xD9, 0xEE, 0xC2, 0x04, 0x00}, 5},
			// GetTerrainVertexColors: FNSTSW AX / TEST AH,0x41 / JNZ +0x30 skips the bent normal when
			// the vertex is not below the grid. JMP +0x30 always skips it.
			{0x007D7FFB, {0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x30}, 7, 5, {0xEB}, 1},
		};

		bool gShadowDepthPatched = false;

		uint8_t *PatchSite(CodePatch const &patch) {
			return reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr)) + (patch.address - kImageBase);
		}

		void WriteCode(uint8_t *site, uint8_t const *bytes, size_t size) {
			DWORD protection = 0;
			if (!VirtualProtect(site, size, PAGE_EXECUTE_READWRITE, &protection)) return;
			std::memcpy(site, bytes, size);
			DWORD ignored = 0;
			VirtualProtect(site, size, protection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), site, size);
		}

		TerrainShadowMap::HeightField gField;
		TerrainShadowMap::HeightField gScratch;
		TerrainShadowMap::Map gMap;
		bool gMapValid = false;
		void const *gTerrain = nullptr;
		float gSun[3]{};
		uint64_t gGeneration = 0;
		ULONGLONG gLastRead = 0;
		unsigned gBuilds = 0;
		// -2 before the first lookup, -1 when RenderShadows cannot be read.
		int32_t gRenderShadowsId = -2;

		cISTETerrain *CityTerrain() {
			cISC4AppPtr app;
			if (!app) return nullptr;
			cISC4City *const city = app->GetCity();
			return city != nullptr ? city->GetTerrain() : nullptr;
		}

#if defined(_MSC_VER)
		bool SafeTerrainGeometry(cISTETerrain *terrain, uint32_t &cellsX, uint32_t &cellsZ, int32_t &rowStride,
		                         float &cellWidth) {
			__try {
				cellsX = terrain->CellCountX();
				cellsZ = terrain->CellCountZ();
				rowStride = terrain->GetVertIndex(0, 1);
				using CellWidth = float(__thiscall *)(void *);
				cellWidth = reinterpret_cast<CellWidth>((*reinterpret_cast<void ***>(terrain))[kCellWidthSlot])(terrain);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeGetAltitudes(cISTETerrain *terrain, uint32_t verticesX, uint32_t verticesZ, float *out) {
			__try {
				SC4Rect<int32_t> const rect{
					0, 0, static_cast<int32_t>(verticesX - 1), static_cast<int32_t>(verticesZ - 1)
				};
				terrain->GetAltitudes(rect, out, static_cast<int32_t>(verticesX), 1);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Normals and flip flags, from records whose altitudes must be exactly
		// the ones GetAltitudes returned.
		bool SafeReadRecords(cISTETerrain *terrain, size_t vertices, float const *altitudes, uint8_t *flipped,
		                     float *normals) {
			__try {
				uint8_t const *const records =
					*reinterpret_cast<uint8_t const *const *>(reinterpret_cast<uint8_t const *>(terrain) +
					                                          kTerrainRecords);
				if (records == nullptr) return false;
				for (size_t index = 0; index < vertices; ++index) {
					uint8_t const *const record = records + index * kRecordSize;
					if (std::memcmp(record, altitudes + index, sizeof(float)) != 0) return false;
					std::memcpy(normals + index * 3, record + kRecordNormal, sizeof(float) * 3);
					uint16_t flags = 0;
					std::memcpy(&flags, record + kRecordFlags, sizeof(flags));
					flipped[index] = (flags & kFlippedFlags) != 0 ? 1 : 0;
				}
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The render property interface has not been called from a DLL before,
		// so the id is only used once its name reads back.
		bool SafeFindBoolProperty(cISC4RenderProperties *properties, char const *name, int32_t &id) {
			__try {
				int32_t const found = properties->BoolPropertyIDFromName(name);
				char const *const readBack = properties->NameFromBoolPropertyID(found);
				if (readBack == nullptr || _stricmp(readBack, name) != 0) return false;
				id = found;
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool SafeBoolValue(cISC4RenderProperties *properties, int32_t id, bool &value) {
			__try {
				value = properties->BoolValue(id);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeTerrainGeometry(cISTETerrain *, uint32_t &, uint32_t &, int32_t &, float &) { return false; }
		bool SafeGetAltitudes(cISTETerrain *, uint32_t, uint32_t, float *) { return false; }
		bool SafeReadRecords(cISTETerrain *, size_t, float const *, uint8_t *, float *) { return false; }
		bool SafeFindBoolProperty(cISC4RenderProperties *, char const *, int32_t &) { return false; }
		bool SafeBoolValue(cISC4RenderProperties *, int32_t, bool &) { return false; }
#endif

		bool ReadHeightField(cISTETerrain *terrain, TerrainShadowMap::HeightField &field) {
			uint32_t cellsX = 0, cellsZ = 0;
			int32_t rowStride = 0;
			float cellWidth = 0.0f;
			if (!SafeTerrainGeometry(terrain, cellsX, cellsZ, rowStride, cellWidth)) return false;
			if (cellsX == 0 || cellsZ == 0 || cellsX > kMaxCells || cellsZ > kMaxCells ||
			    rowStride != static_cast<int32_t>(cellsX + 1) || !(cellWidth > 0.0f && cellWidth < 1024.0f)) {
				static bool logged = false;
				if (!logged) {
					logged = true;
					Log(LogCategory::Initialization,
					    "terrain shadows: unexpected terrain geometry (cells %ux%u, row stride %d, cell width %.3f)",
					    cellsX, cellsZ, rowStride, cellWidth);
				}
				return false;
			}
			field.verticesX = cellsX + 1;
			field.verticesZ = cellsZ + 1;
			field.cellWidth = cellWidth;
			field.altitudes.resize(static_cast<size_t>(field.verticesX) * field.verticesZ);
			if (!SafeGetAltitudes(terrain, field.verticesX, field.verticesZ, field.altitudes.data())) return false;
			for (float const altitude: field.altitudes)
				if (!std::isfinite(altitude)) return false;
			size_t const vertices = field.altitudes.size();
			field.flipped.resize(vertices);
			field.normals.resize(vertices * 3);
			bool const records =
				SafeReadRecords(terrain, vertices, field.altitudes.data(), field.flipped.data(), field.normals.data());
			if (records) {
				for (float const component: field.normals)
					if (!std::isfinite(component)) {
						field.normals.clear();
						break;
					}
			} else {
				field.flipped.clear();
				field.normals.clear();
			}
			static bool loggedRecords = false;
			if (!loggedRecords) {
				loggedRecords = true;
				size_t flippedCells = 0;
				for (uint8_t const flip: field.flipped) flippedCells += flip;
				if (records) {
					Log(LogCategory::Initialization,
					    "terrain shadows: vertex records match GetAltitudes; %u flipped cells, %s normals",
					    static_cast<unsigned>(flippedCells), field.normals.empty() ? "derived" : "SC4's own");
				} else {
					Log(LogCategory::Initialization,
					    "terrain shadows: vertex records do not match GetAltitudes; cells taken as unflipped, "
					    "normals derived from the height field");
				}
			}
			return true;
		}

		bool SameField(TerrainShadowMap::HeightField const &a, TerrainShadowMap::HeightField const &b) {
			return a.verticesX == b.verticesX && a.verticesZ == b.verticesZ && a.cellWidth == b.cellWidth &&
			       a.flipped.size() == b.flipped.size() && a.normals.size() == b.normals.size() &&
			       std::memcmp(a.altitudes.data(), b.altitudes.data(), a.altitudes.size() * sizeof(float)) == 0 &&
			       std::memcmp(a.flipped.data(), b.flipped.data(), a.flipped.size()) == 0 &&
			       std::memcmp(a.normals.data(), b.normals.data(), a.normals.size() * sizeof(float)) == 0;
		}

		double Milliseconds(LARGE_INTEGER const &start) {
			LARGE_INTEGER end{}, frequency{};
			QueryPerformanceCounter(&end);
			QueryPerformanceFrequency(&frequency);
			return frequency.QuadPart != 0
				       ? static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)
				       : 0.0;
		}
	} // namespace

	void Install() {
		if (gShadowDepthPatched || !LiveShadowHooks::Enabled()) return;
		// Both sites or neither, so terrain and models never disagree about SC4's shadow.
		for (CodePatch const &patch: kPatches) {
			if (std::memcmp(PatchSite(patch), patch.guard, patch.guardSize) != 0) {
				Log(LogCategory::Initialization,
				    "terrain shadows: guard failed at 0x%08lX, SC4's terrain self-shading stays on",
				    static_cast<unsigned long>(patch.address));
				return;
			}
		}
		for (CodePatch const &patch: kPatches) WriteCode(PatchSite(patch) + patch.offset, patch.bytes, patch.size);
		gShadowDepthPatched = true;
		Log(LogCategory::Initialization, "terrain shadows installed: SC4's own terrain self-shading replaced");
	}

	void Uninstall() {
		if (!gShadowDepthPatched) return;
		for (CodePatch const &patch: kPatches) {
			uint8_t *const site = PatchSite(patch) + patch.offset;
			if (std::memcmp(site, patch.bytes, patch.size) == 0) WriteCode(site, patch.guard + patch.offset, patch.size);
		}
		gShadowDepthPatched = false;
		gMapValid = false;
		gTerrain = nullptr;
	}

	bool ShadowsRendered() {
		cISC4AppPtr app;
		cISC4RenderProperties *const properties = app ? app->GetRenderProperties() : nullptr;
		if (properties == nullptr) return true;
		if (gRenderShadowsId == -2) {
			int32_t id = -1;
			gRenderShadowsId = SafeFindBoolProperty(properties, "RenderShadows", id) ? id : -1;
			bool value = true;
			bool const read = gRenderShadowsId >= 0 && SafeBoolValue(properties, gRenderShadowsId, value);
			Log(LogCategory::Initialization, "terrain shadows: RenderShadows render property %s%s",
			    read ? "is " : "could not be read, shadows assumed on",
			    read ? (value ? "on" : "off") : "");
		}
		bool value = true;
		return gRenderShadowsId < 0 || !SafeBoolValue(properties, gRenderShadowsId, value) || value;
	}

	TerrainShadowMap::Map const *Update(float const sun[3], uint64_t &generation) {
		cISTETerrain *const terrain = CityTerrain();
		bool const sameSun = gMapValid && std::fabs(sun[0] - gSun[0]) <= kSunTolerance &&
		                     std::fabs(sun[1] - gSun[1]) <= kSunTolerance &&
		                     std::fabs(sun[2] - gSun[2]) <= kSunTolerance;
		// Reading and comparing the height field is the expensive part, so an
		// unchanged sun over the same terrain re-reads it at most every
		// kTerrainPollMilliseconds. A terraform shows up on the first pass after
		// that, as a new generation.
		ULONGLONG const now = GetTickCount64();
		if (sameSun && terrain != nullptr && terrain == gTerrain && now - gLastRead < kTerrainPollMilliseconds) {
			generation = gGeneration;
			return &gMap;
		}
		gLastRead = now;
		if (terrain == nullptr || !ReadHeightField(terrain, gScratch)) {
			gMapValid = false;
			gTerrain = nullptr;
			return nullptr;
		}
		bool const sameTerrain = gMapValid && terrain == gTerrain && SameField(gScratch, gField);
		if (sameTerrain && sameSun) {
			generation = gGeneration;
			return &gMap;
		}

		LARGE_INTEGER start{};
		QueryPerformanceCounter(&start);
		TerrainShadowMap::Map next;
		if (!TerrainShadowMap::Build(gScratch, sun, gScratch.cellWidth * kSpacingCells, next, kSunRadius)) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				Log(LogCategory::Initialization, "terrain shadows: no map for sun %.4f/%.4f/%.4f over %ux%u vertices",
				    sun[0], sun[1], sun[2], gScratch.verticesX, gScratch.verticesZ);
			}
			gMapValid = false;
			return nullptr;
		}
		double const buildTime = Milliseconds(start);
		bool const newCity = terrain != gTerrain;
		std::swap(gField, gScratch);
		gMap = std::move(next);
		gMapValid = true;
		gTerrain = terrain;
		std::memcpy(gSun, sun, sizeof(gSun));
		++gGeneration;
		++gBuilds;

		if (newCity || gBuilds <= 2) {
			Log(LogCategory::Initialization,
			    "terrain shadows: %s %ux%u vertices (cell %.1f), map %ux%u at %.1f, sun %.4f/%.4f/%.4f, %.2f ms",
			    newCity ? "city" : (sameSun ? "terrain changed," : "sun changed,"), gField.verticesX,
			    gField.verticesZ, gField.cellWidth, gMap.width, gMap.height, gMap.spacing, sun[0], sun[1], sun[2],
			    buildTime);
		}
		generation = gGeneration;
		return &gMap;
	}

	TerrainShadowMap::HeightField const &Terrain() {
		return gField;
	}

	bool Altitude(float x, float z, float &altitude) {
		if (!gMapValid) return false;
		float const extentX = static_cast<float>(gField.verticesX - 1) * gField.cellWidth;
		float const extentZ = static_cast<float>(gField.verticesZ - 1) * gField.cellWidth;
		if (!(x >= 0.0f && x <= extentX && z >= 0.0f && z <= extentZ)) return false;
		altitude = TerrainShadowMap::Altitude(gField, x, z);
		return true;
	}
} // namespace nSCD3D11::TerrainShadows
