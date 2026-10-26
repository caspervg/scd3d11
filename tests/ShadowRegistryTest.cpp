#include "ShadowRegistry.h"

#include <cassert>
#include <vector>

using nSCD3D11::ShadowRegistry::AppendTriangles;

class cRZCOMDllDirector;
// SunPath reaches SC4 through the COM director, which the tests never start.
cRZCOMDllDirector *RZGetCOMDllDirector() { return nullptr; }

int main() {
	uint32_t const indices[]{10, 11, 12, 13, 14, 15};
	std::vector<uint32_t> triangles;

	// Lists keep whole triangles only.
	assert(AppendTriangles(0, indices, 5, triangles));
	assert((triangles == std::vector<uint32_t>{10, 11, 12}));

	// Strips and fans become one triangle per vertex after the second.
	triangles.clear();
	assert(AppendTriangles(1, indices, 5, triangles));
	assert((triangles == std::vector<uint32_t>{10, 11, 12, 11, 12, 13, 12, 13, 14}));
	triangles.clear();
	assert(AppendTriangles(2, indices, 5, triangles));
	assert((triangles == std::vector<uint32_t>{10, 11, 12, 10, 12, 13, 10, 13, 14}));

	// Quads and quad strips split into two triangles each.
	triangles.clear();
	assert(AppendTriangles(6, indices, 4, triangles));
	assert((triangles == std::vector<uint32_t>{10, 11, 12, 10, 12, 13}));
	triangles.clear();
	assert(AppendTriangles(7, indices, 6, triangles));
	assert((triangles == std::vector<uint32_t>{10, 11, 12, 12, 11, 13, 12, 13, 14, 14, 13, 15}));

	// Runs append to what is already there, as a mesh's runs do.
	assert(AppendTriangles(0, indices + 3, 3, triangles));
	assert(triangles.size() == 15 && triangles[12] == 13 && triangles[14] == 15);

	// Points, lines and too-short runs cast nothing.
	triangles.clear();
	assert(!AppendTriangles(3, indices, 6, triangles));
	assert(!AppendTriangles(4, indices, 6, triangles));
	assert(!AppendTriangles(5, indices, 6, triangles));
	assert(!AppendTriangles(0, indices, 2, triangles));
	assert(!AppendTriangles(6, indices, 3, triangles));
	assert(triangles.empty());

	// A unit box standing on y = 0 under a 45-degree sun travelling +x: its
	// quad runs from just behind its foot to where its top lands.
	nSCD3D11::ShadowRegistry::Caster caster;
	caster.high[0] = caster.high[1] = caster.high[2] = 1.0f;
	float const sun[3]{0.70710678f, -0.70710678f, 0.0f};
	float quad[4][3]{};
	assert(nSCD3D11::ShadowRegistry::GroundQuad(caster, sun, quad));
	assert(quad[0][0] < -0.19f && quad[0][0] > -0.21f); // the foot sits below the lifted plane
	assert(quad[2][2] == 1.0f && quad[0][1] > 0.0f);
	assert(quad[2][0] > 1.79f && quad[2][0] < 1.81f); // 1 + (1 - 0.2 lift)
	float const sunUp[3]{0.0f, 1.0f, 0.0f};
	assert(!nSCD3D11::ShadowRegistry::GroundQuad(caster, sunUp, quad));

	// A projector along that same sun: u = x + y, v = z. Under it the quad
	// reads the texture where it lies.
	caster.projector[0] = 1.0f;
	caster.projector[4] = 1.0f;
	caster.projector[9] = 1.0f;
	caster.projector[15] = 1.0f;
	float offsets[4][2]{};
	nSCD3D11::ShadowRegistry::QuadProjectorOffsets(caster, sun, quad, offsets);
	for (auto const &offset : offsets) assert(offset[0] == 0.0f && offset[1] == 0.0f);
	// A sun travelling +z at about 63 degrees: half as long a shadow, turned a
	// quarter. A point 0.5 down its shadow from the centre reads SC4's
	// shadow 1 along +x from it.
	float const turned[3]{0.0f, -0.89442719f, 0.44721360f};
	float warpedQuad[4][3]{};
	assert(nSCD3D11::ShadowRegistry::GroundQuad(caster, turned, warpedQuad));
	warpedQuad[0][0] = 0.5f;
	warpedQuad[0][2] = 1.0f;
	nSCD3D11::ShadowRegistry::QuadProjectorOffsets(caster, turned, warpedQuad, offsets);
	float const readX = warpedQuad[0][0] + offsets[0][0], readZ = warpedQuad[0][2] + offsets[0][1];
	assert(readX > 1.49f && readX < 1.51f && readZ > 0.49f && readZ < 0.51f);

	// Reach is culled only once box and quad both miss the NDC square. With
	// world x -> NDC x - 2.5, the box [0, 1] misses but the quad to 1.8 does not.
	float worldToClip[16]{};
	worldToClip[0] = worldToClip[5] = worldToClip[10] = worldToClip[15] = 1.0f;
	worldToClip[12] = -2.5f;
	assert(nSCD3D11::ShadowRegistry::ReachOutside(caster, false, quad, worldToClip));
	assert(nSCD3D11::ShadowRegistry::GroundQuad(caster, sun, quad));
	assert(!nSCD3D11::ShadowRegistry::ReachOutside(caster, true, quad, worldToClip));
	worldToClip[12] = 0.0f;
	worldToClip[13] = 5.0f;
	assert(nSCD3D11::ShadowRegistry::ReachOutside(caster, true, quad, worldToClip));

	// Nothing is installed without LiveShadows, and an idle
	// registry reports no pass.
	assert(!nSCD3D11::ShadowRegistry::Install());
	assert(!nSCD3D11::ShadowRegistry::Enabled());
	nSCD3D11::ShadowRegistry::Pass pass;
	assert(!nSCD3D11::ShadowRegistry::TakePass(pass));
	return 0;
}
