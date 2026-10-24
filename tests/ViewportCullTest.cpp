#include "ViewportCull.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

using namespace nSCD3D11;

namespace {
	void Identity(float *m) {
		std::memset(m, 0, sizeof(float) * 16);
		m[0] = m[5] = m[10] = m[15] = 1.0f;
	}

	bool Outside(ClipRows const &rows, ClipBounds const &bounds, float const (*positions)[3], uint32_t count) {
		return AllVerticesOutside(rows, bounds, reinterpret_cast<uint8_t const *>(positions), sizeof(float) * 3, count);
	}

	bool Near(float a, float b) {
		return std::fabs(a - b) < 1e-5f;
	}
}

int main() {
	ClipBounds bounds{};
	// A viewport mapped onto itself spans NDC -1..1, plus the margin in pixels.
	assert(MakeClipBounds(0, 0, 100, 50, 0, 0, 100, 50, 1.0f, bounds));
	assert(Near(bounds.left, -1.02f) && Near(bounds.right, 1.02f));
	assert(Near(bounds.bottom, -1.04f) && Near(bounds.top, 1.04f));
	// A sub-rectangle of a larger viewport.
	ClipBounds strip{};
	assert(MakeClipBounds(0, 0, 200, 100, 50, 0, 50, 100, 0.0f, strip));
	assert(Near(strip.left, -0.5f) && Near(strip.right, 0.0f) && Near(strip.bottom, -1.0f) && Near(strip.top, 1.0f));
	assert(!MakeClipBounds(0, 0, 0, 100, 0, 0, 0, 100, 1.0f, strip));
	assert(!MakeClipBounds(0, 0, 100, -1, 0, 0, 100, 1, 1.0f, strip));

	float projection[16];
	float modelView[16];
	Identity(projection);
	Identity(modelView);
	ClipRows rows{};
	MakeClipRows(projection, modelView, rows);
	assert(rows.x[0] == 1.0f && rows.y[1] == 1.0f && rows.w[3] == 1.0f && rows.w[0] == 0.0f && rows.x[3] == 0.0f);

	float const right[3][3] = {{2, 0, 0}, {3, 0.5f, 0}, {2, -0.5f, 0}};
	float const straddling[3][3] = {{0.5f, 0, 0}, {3, 0.5f, 0}, {2, -0.5f, 0}};
	float const bothSides[2][3] = {{-2, 0, 0}, {2, 0, 0}};
	float const inMargin[2][3] = {{1.01f, 0, 0}, {3, 0, 0}};
	float const aboveAndRight[2][3] = {{2, 2, 0}, {0, 2, 0}};
	assert(Outside(rows, bounds, right, 3));
	assert(!Outside(rows, bounds, straddling, 3));
	// Beyond different edges is not provably outside.
	assert(!Outside(rows, bounds, bothSides, 2));
	assert(!Outside(rows, bounds, inMargin, 2));
	// Both above the top edge, one also to the right.
	assert(Outside(rows, bounds, aboveAndRight, 2));
	assert(!Outside(rows, bounds, right, 0));

	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const notANumber[2][3] = {{2, 0, 0}, {nan, 0, 0}};
	assert(!Outside(rows, bounds, notANumber, 2));

	// Translation and scale: clip x = 0.5 * (x + 3), so x = 0 lands at 1.5.
	modelView[12] = 3.0f;
	projection[0] = 0.5f;
	MakeClipRows(projection, modelView, rows);
	float const origin[1][3] = {{0, 0, 0}};
	float const left[1][3] = {{-3, 0, 0}};
	assert(Outside(rows, bounds, origin, 1));
	assert(!Outside(rows, bounds, left, 1));

	// Perspective: w = -z. A vertex behind the eye keeps the draw.
	Identity(projection);
	Identity(modelView);
	projection[11] = -1.0f;
	projection[15] = 0.0f;
	MakeClipRows(projection, modelView, rows);
	float const farRight[2][3] = {{5, 0, -1}, {6, 0, -2}};
	float const behind[2][3] = {{5, 0, -1}, {6, 0, 1}};
	assert(Outside(rows, bounds, farRight, 2));
	assert(!Outside(rows, bounds, behind, 2));
	// Within the frustum once divided: x / w = 0.5.
	float const inside[1][3] = {{1, 0, -2}};
	assert(!Outside(rows, bounds, inside, 1));

	// Positions inside a wider interleaved vertex.
	Identity(projection);
	MakeClipRows(projection, modelView, rows);
	float interleaved[3][6] = {{0, 2, 0, 9, 9, 9}, {1, 3, 0, 9, 9, 9}, {-1, 2.5f, 0, 9, 9, 9}};
	assert(AllVerticesOutside(rows, bounds, reinterpret_cast<uint8_t const *>(interleaved), sizeof(interleaved[0]), 3));
	interleaved[2][1] = 0.0f;
	assert(!AllVerticesOutside(rows, bounds, reinterpret_cast<uint8_t const *>(interleaved), sizeof(interleaved[0]), 3));
	return 0;
}
