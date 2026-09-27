#include "NativeShadowRegistry.h"

#include <cassert>
#include <vector>

using nSCD3D11::NativeShadowRegistry::AppendTriangles;

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

	// Nothing is installed without -NativeShadowMasks:replace, and an idle
	// registry reports no pass.
	assert(!nSCD3D11::NativeShadowRegistry::Install());
	assert(!nSCD3D11::NativeShadowRegistry::Enabled());
	nSCD3D11::NativeShadowRegistry::Pass pass;
	assert(!nSCD3D11::NativeShadowRegistry::TakePass(pass));
	return 0;
}
