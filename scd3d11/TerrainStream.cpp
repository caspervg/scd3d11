/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SimCity 4 Deluxe 1.1.641. cSTETerrainView3D keeps the whole city's terrain as two vertex arrays of 0x20 bytes
// per vertex (position, BGRA colour, texture coordinates), row by row along x: sPoints (0x00B4C758) for the
// ground and sWaterPoints (0x00B4C75C) for the water surface, sVertexCountX (0x00B4C74C) to a row and
// sNumVertices (0x00B4C754) in all. The ground's colour is SC4's lighting, its alpha set per texture as each is
// drawn (DrawTerrainSurfaceRows, 0x0075B460). Which textures a vertex blends comes from cSTETerrain's 0x24 byte
// vertex records (at +0x6C): up to three of a 16-bit texture id at +0x16 and a weight byte at +0x18, four bytes
// apart, a zero id ending the list. A texture id's low 12 bits and bits 16 up name the FSH (0x7AB50E44,
// 0x891B0E1A, zoom x 0x100 + id, else id), bits 12 to 15 the image in it
// (SetUpTexturesForCurrentTerrainTypeAndZoom, 0x007571D0). Flags 0x3000 at +0x22 split a cell along its other
// diagonal. The textures are loaded for sCurZoom (0x00B4C70C).

#include "TerrainStream.h"

#include "cGDriverWire.h"
#include "Diagnostics.h"
#include "GameVersion.h"

#include "GZServPtrs.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"

#include <cstring>
#include <vector>

#include <windows.h>

namespace nSCD3D11::TerrainStream {
	namespace {
		constexpr uintptr_t kPoints = 0x00B4C758, kWaterPoints = 0x00B4C75C, kVertexCountX = 0x00B4C74C,
		                    kNumVertices = 0x00B4C754, kCurZoom = 0x00B4C70C;
		constexpr size_t kPointBytes = 0x20, kRecordBytes = 0x24;
		constexpr ptrdiff_t kTerrainRecords = 0x6C;
		constexpr ULONGLONG kPollMilliseconds = 1000;
		constexpr uint32_t kMaxVertices = 4097 * 4097;

		ULONGLONG gLastRead = 0;
		void const *gTerrain = nullptr;
		bool gChecked = false, gUsable = false;
		nSCDWire::TerrainField gField{};
		std::vector<nSCDWire::TerrainVertex> gSent, gScratch;

		cISTETerrain *CityTerrain() {
			cISC4AppPtr app;
			if (!app) return nullptr;
			cISC4City *const city = app->GetCity();
			return city != nullptr ? city->GetTerrain() : nullptr;
		}

		template<typename T>
		T Global(uintptr_t address) { return *reinterpret_cast<T const *>(address); }

		bool ReadInto(cISTETerrain *terrain, nSCDWire::TerrainField &field, std::vector<nSCDWire::TerrainVertex> &out) {
			uint32_t const columns = Global<uint32_t>(kVertexCountX), count = Global<uint32_t>(kNumVertices);
			uint8_t const *const points = Global<uint8_t const *>(kPoints);
			uint8_t const *const water = Global<uint8_t const *>(kWaterPoints);
			uint8_t const *const records =
				*reinterpret_cast<uint8_t const *const *>(reinterpret_cast<uint8_t const *>(terrain) + kTerrainRecords);
			if (points == nullptr || water == nullptr || records == nullptr || columns < 2 || count > kMaxVertices ||
			    count % columns != 0 || count / columns < 2 || terrain->CellCountX() + 1 != columns) return false;
			auto const point = [&](uint8_t const *base, uint32_t index, uint32_t offset) {
				float value;
				std::memcpy(&value, base + index * kPointBytes + offset, sizeof(value));
				return value;
			};
			field.verticesX = columns;
			field.verticesZ = count / columns;
			field.cellWidth = point(points, 1, 0) - point(points, 0, 0);
			field.zoom = Global<uint32_t>(kCurZoom);
			if (!(field.cellWidth > 0.0f) || field.zoom > 4) return false;
			// The coordinates are a plane over x and z, so three vertices give them.
			for (uint32_t axis = 0; axis < 2; ++axis) {
				float const at0 = point(points, 0, 0x10 + axis * 4);
				field.uv[axis * 3] = (point(points, 1, 0x10 + axis * 4) - at0) / field.cellWidth;
				field.uv[axis * 3 + 1] = (point(points, columns, 0x10 + axis * 4) - at0) / field.cellWidth;
				field.uv[axis * 3 + 2] = at0;
			}
			out.resize(count);
			for (uint32_t i = 0; i < count; ++i) {
				uint8_t const *const p = points + i * kPointBytes;
				uint8_t const *const w = water + i * kPointBytes;
				uint8_t const *const r = records + i * kRecordBytes;
				nSCDWire::TerrainVertex &v = out[i];
				std::memcpy(&v.height, p + 4, 4);
				std::memcpy(&v.water, w + 4, 4);
				std::memcpy(&v.color, p + 12, 4);
				v.color &= 0x00FFFFFFu;
				std::memcpy(&v.waterColor, w + 12, 4);
				for (int k = 0; k < 3; ++k) {
					std::memcpy(&v.texture[k], r + 0x16 + k * 4, 2);
					v.weight[k] = v.texture[k] != 0 ? r[0x18 + k * 4] : 0;
					if (v.texture[k] == 0) {
						for (int rest = k + 1; rest < 3; ++rest) v.texture[rest] = 0, v.weight[rest] = 0;
						break;
					}
				}
				uint16_t flags;
				std::memcpy(&flags, r + 0x22, 2);
				v.flags = (flags & 0x3000) != 0 ? 1 : 0;
				v.reserved = 0;
			}
			return true;
		}

#if defined(_MSC_VER)
		bool SafeRead(cISTETerrain *terrain, nSCDWire::TerrainField &field, std::vector<nSCDWire::TerrainVertex> &out) {
			__try {
				return ReadInto(terrain, field, out);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
#else
		bool SafeRead(cISTETerrain *, nSCDWire::TerrainField &, std::vector<nSCDWire::TerrainVertex> &) { return false; }
#endif
	}

	void Poll(cGDriverWire &wire) {
		ULONGLONG const now = GetTickCount64();
		if (now - gLastRead < kPollMilliseconds) return;
		gLastRead = now;
		if (!gChecked) {
			gChecked = true;
			gUsable = IsGameVersion641();
		}
		if (!gUsable) return;
		cISTETerrain *const terrain = CityTerrain();
		if (terrain == nullptr) {
			gTerrain = nullptr;
			return;
		}
		nSCDWire::TerrainField field{};
		if (!SafeRead(terrain, field, gScratch)) return;
		bool const same = terrain == gTerrain && std::memcmp(&field, &gField, sizeof(field)) == 0 &&
		                  gScratch.size() == gSent.size() &&
		                  std::memcmp(gScratch.data(), gSent.data(), gSent.size() * sizeof(nSCDWire::TerrainVertex)) == 0;
		if (same) return;
		if (terrain != gTerrain) {
			Log(LogCategory::Initialization, "terrain stream: %ux%u vertices, cell %.1f m, zoom %u", field.verticesX,
			    field.verticesZ, field.cellWidth, field.zoom);
		}
		gTerrain = terrain;
		gField = field;
		std::swap(gSent, gScratch);
		wire.RecordTerrainField(gField, gSent.data());
	}
}
