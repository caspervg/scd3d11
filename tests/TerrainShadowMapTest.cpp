#include "TerrainShadowMap.h"

#include <cassert>
#include <cmath>
#include <vector>

using namespace nSCD3D11::TerrainShadowMap;

namespace {
	constexpr float kCell = 16.0f;
	constexpr float kGround = 100.0f;
	// A point counts as shadowed once it is this far below the ceiling, as in
	// the composite's bias.
	constexpr float kBias = 0.5f;

	HeightField Flat(uint32_t vertices) {
		HeightField field;
		field.verticesX = vertices;
		field.verticesZ = vertices;
		field.cellWidth = kCell;
		field.altitudes.assign(static_cast<size_t>(vertices) * vertices, kGround);
		return field;
	}

	float &At(HeightField &field, uint32_t x, uint32_t z) {
		return field.altitudes[static_cast<size_t>(z) * field.verticesX + x];
	}

	bool Shadowed(Map const &map, HeightField const &field, float x, float z) {
		return Ceiling(map, x, z) > Altitude(field, x, z) + kBias;
	}

	bool Covered(std::vector<Region> const &regions, float x, float z) {
		for (Region const &region: regions)
			if (x >= region.minX && x <= region.maxX && z >= region.minZ && z <= region.maxZ) return true;
		return false;
	}
} // namespace

int main() {
	// Altitude follows the drawn triangles and clamps outside the city.
	{
		HeightField field = Flat(3);
		At(field, 1, 1) = 200.0f;
		assert(std::fabs(Altitude(field, 16.0f, 16.0f) - 200.0f) < 1.0e-4f);
		assert(std::fabs(Altitude(field, 8.0f, 16.0f) - 150.0f) < 1.0e-4f);
		assert(std::fabs(Altitude(field, -50.0f, 16.0f) - kGround) < 1.0e-4f);
		assert(std::fabs(Altitude(field, 16.0f, 500.0f) - kGround) < 1.0e-4f);
		// Cell (0, 0) has its high corner at (1, 1). Split along (0, 0)-(1, 1),
		// its centre lies on that diagonal, halfway up; flipped, on the other
		// diagonal, whose ends are both low.
		assert(std::fabs(Altitude(field, 8.0f, 8.0f) - 150.0f) < 1.0e-4f);
		assert(std::fabs(Altitude(field, 12.0f, 4.0f) - 125.0f) < 1.0e-4f);
		assert(std::fabs(Altitude(field, 4.0f, 12.0f) - 125.0f) < 1.0e-4f);
		field.flipped.assign(9, 0);
		field.flipped[0] = 1;
		assert(std::fabs(Altitude(field, 8.0f, 8.0f) - kGround) < 1.0e-4f);
		assert(std::fabs(Altitude(field, 4.0f, 4.0f) - kGround) < 1.0e-4f);
		assert(std::fabs(Altitude(field, 12.0f, 12.0f) - 150.0f) < 1.0e-4f);
		// The other cells keep the usual split.
		assert(std::fabs(Altitude(field, 24.0f, 24.0f) - 150.0f) < 1.0e-4f);
	}

	float const east[3]{1.0f, -1.0f, 0.0f};
	// Flat ground shadows nothing, and the stored ceiling never drops more
	// than kFloorDepth under it.
	{
		HeightField const field = Flat(33);
		Map map;
		assert(Build(field, east, 8.0f, map));
		assert(map.width == 65 && map.height == 65);
		for (float x = 0.0f; x <= 512.0f; x += 13.0f)
			for (float z = 0.0f; z <= 512.0f; z += 13.0f) assert(!Shadowed(map, field, x, z));
		for (float value: map.ceiling) assert(value >= kGround - kFloorDepth - 1.0e-3f);
	}

	// A ridge 100 m high across a 45-degree sun travelling +x shadows the next
	// 100 m downwind, falling one metre per metre, and nothing up-sun of it.
	{
		HeightField field = Flat(33);
		for (uint32_t z = 0; z < 33; ++z) At(field, 10, z) = kGround + 100.0f;
		Map map;
		assert(Build(field, east, 8.0f, map));
		float const ridge = 10.0f * kCell;
		for (float distance: {24.0f, 40.0f, 80.0f}) {
			assert(std::fabs(Ceiling(map, ridge + distance, 256.0f) - (kGround + 100.0f - distance)) < 0.1f);
			assert(Shadowed(map, field, ridge + distance, 256.0f));
		}
		assert(!Shadowed(map, field, ridge + 110.0f, 256.0f));
		assert(!Shadowed(map, field, ridge - 40.0f, 256.0f));
		// A wall or roof above the ceiling is lit, one below it is not.
		assert(Ceiling(map, ridge + 40.0f, 256.0f) < kGround + 61.0f);
		assert(Ceiling(map, ridge + 40.0f, 256.0f) > kGround + 59.0f);
	}

	// SC4's own sun: 45 degrees up, 67.5 + 90k degrees around. A spike casts
	// along the light and not across it. The spike is a single vertex, so the
	// map row nearest its peak can pass up to half a texel beside it; 60 m
	// downwind is well inside its shadow either way.
	float const sc4Sun[3]{-0.65328145f, -0.707106769f, 0.270598054f};
	HeightField const flat = Flat(65);
	HeightField spike = Flat(65);
	At(spike, 32, 32) = kGround + 200.0f;
	float const peak = 32.0f * kCell;
	Map flatMap, spikeMap;
	assert(Build(flat, sc4Sun, 8.0f, flatMap));
	assert(Build(spike, sc4Sun, 8.0f, spikeMap));
	float const horizontal = std::sqrt(sc4Sun[0] * sc4Sun[0] + sc4Sun[2] * sc4Sun[2]);
	float const along[2]{sc4Sun[0] / horizontal, sc4Sun[2] / horizontal};
	float const across[2]{-along[1], along[0]};
	float const downwind[2]{peak + along[0] * 60.0f, peak + along[1] * 60.0f};
	float const beside[2]{peak + across[0] * 100.0f, peak + across[1] * 100.0f};
	float const upwind[2]{peak - along[0] * 100.0f, peak - along[1] * 100.0f};
	assert(Shadowed(spikeMap, spike, downwind[0], downwind[1]));
	assert(!Shadowed(spikeMap, spike, beside[0], beside[1]));
	assert(!Shadowed(spikeMap, spike, upwind[0], upwind[1]));
	assert(!Shadowed(flatMap, flat, downwind[0], downwind[1]));

	// Raising the spike changes the shading downwind of it, which is what a
	// partial pass must redisplay, and nothing up-sun.
	{
		assert(SameLayout(flatMap, spikeMap));
		std::vector<Region> regions;
		ChangedRegions(flatMap, spikeMap, spike, 32, regions);
		assert(!regions.empty());
		assert(Covered(regions, downwind[0], downwind[1]));
		// A changed tile spans 128 m of map, under 190 m of world however it is turned.
		assert(!Covered(regions, peak - along[0] * 300.0f, peak - along[1] * 300.0f));
		for (Region const &region: regions)
			assert(region.minX >= 0.0f && region.minZ >= 0.0f && region.maxX <= 1024.0f && region.maxZ <= 1024.0f);
		ChangedRegions(spikeMap, spikeMap, spike, 32, regions);
		assert(regions.empty());
		// Too many regions collapse into their union.
		ChangedRegions(flatMap, spikeMap, spike, 0, regions);
		assert(regions.size() == 1 && Covered(regions, downwind[0], downwind[1]));
	}

	// Another rotation lays the map out differently.
	{
		float const rotated[3]{-sc4Sun[2], sc4Sun[1], sc4Sun[0]};
		Map map;
		assert(Build(flat, rotated, 8.0f, map));
		assert(!SameLayout(map, flatMap));
	}

	// Terrain beyond the city edge is not drawn and casts nothing: the up-sun
	// edge of a flat city is lit.
	{
		Map map;
		assert(Build(flat, east, 8.0f, map));
		assert(!Shadowed(map, flat, 0.0f, 512.0f));
		assert(!Shadowed(map, flat, 4.0f, 512.0f));
	}

	// A sun at or above the horizon, straight down, or a malformed field has
	// no map.
	{
		Map map;
		float const up[3]{1.0f, 0.5f, 0.0f};
		float const level[3]{1.0f, 0.0f, 0.0f};
		float const down[3]{0.0f, -1.0f, 0.0f};
		assert(!Build(flat, up, 8.0f, map));
		assert(!Build(flat, level, 8.0f, map));
		assert(!Build(flat, down, 8.0f, map));
		assert(!Build(flat, east, 0.0f, map));
		HeightField broken = flat;
		broken.altitudes.pop_back();
		assert(!Build(broken, east, 8.0f, map));
	}
	return 0;
}
