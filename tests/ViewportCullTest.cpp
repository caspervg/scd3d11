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

	// The orthographic fast path must agree with the general test. Random orthographic projections and
	// rotated, translated model-views. Vertex sets that are mostly outside one edge, with sizes around the
	// four-vertex groups and the sixteen-vertex checkpoints, and some vertices exactly on an edge.
	uint32_t seed = 12345u;
	auto const random = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
	};
	uint32_t culled = 0;
	for (uint32_t trial = 0; trial < 4000; ++trial) {
		Identity(projection);
		projection[0] = 0.5f + random() * 2.0f;
		projection[5] = 0.5f + random() * 2.0f;
		projection[10] = -0.01f;
		projection[12] = random() - 0.5f;
		projection[13] = random() - 0.5f;
		float const angle = random() * 6.2831853f;
		Identity(modelView);
		modelView[0] = std::cos(angle);
		modelView[1] = std::sin(angle) * 0.5f;
		modelView[2] = std::sin(angle) * 0.8f;
		modelView[4] = -std::sin(angle);
		modelView[5] = std::cos(angle) * 0.5f;
		modelView[6] = std::cos(angle) * 0.8f;
		modelView[9] = 0.8f;
		modelView[10] = -0.5f;
		modelView[12] = random() * 4.0f - 2.0f;
		modelView[13] = random() * 4.0f - 2.0f;
		MakeClipRows(projection, modelView, rows);
		assert(rows.w[0] == 0.0f && rows.w[1] == 0.0f && rows.w[2] == 0.0f && rows.w[3] == 1.0f);

		uint32_t const count = 1 + trial % 37;
		uint32_t const floats = trial & 1 ? 6 : 3;
		float vertices[37 * 6];
		float const centerX = random() * 2.0f - 1.0f;
		float const centerY = random() * 2.0f - 1.0f;
		float const spread = random() * (trial % 5 == 0 ? 3.0f : 0.4f);
		for (uint32_t index = 0; index < count; ++index) {
			float *vertex = vertices + index * floats;
			vertex[0] = centerX + (random() - 0.5f) * spread;
			vertex[1] = centerY + (random() - 0.5f) * spread;
			vertex[2] = random() * 2.0f - 1.0f;
			if (floats == 6) vertex[3] = vertex[4] = vertex[5] = 9.0f;
		}
		uint8_t const *data = reinterpret_cast<uint8_t const *>(vertices);
		uint32_t const stride = floats * sizeof(float);
		bool const fast = AllVerticesOutside(rows, strip, data, stride, count);
		assert(fast == AllVerticesOutsideProjective(rows, strip, data, stride, count));
		culled += fast ? 1 : 0;
	}
	// Both outcomes were exercised (about 86% culled with this seed).
	assert(culled > 400 && culled < 3800);

	// Exactly on an edge counts as inside, in both paths.
	Identity(projection);
	Identity(modelView);
	MakeClipRows(projection, modelView, rows);
	float onEdge[8][3];
	for (uint32_t index = 0; index < 8; ++index) {
		onEdge[index][0] = 3.0f + static_cast<float>(index);
		onEdge[index][1] = 0.0f;
		onEdge[index][2] = 0.0f;
	}
	assert(Outside(rows, strip, onEdge, 8));
	onEdge[6][0] = strip.right;
	assert(!Outside(rows, strip, onEdge, 8));
	assert(!AllVerticesOutsideProjective(rows, strip, reinterpret_cast<uint8_t const *>(onEdge), sizeof(onEdge[0]), 8));

	// A non-finite vertex anywhere keeps the draw, in a four-vertex group or in the scalar tail.
	float const infinity = std::numeric_limits<float>::infinity();
	for (uint32_t index = 0; index < 8; ++index) onEdge[index][0] = 3.0f + static_cast<float>(index);
	for (uint32_t bad = 0; bad < 8; ++bad) {
		for (uint32_t component = 0; component < 3; ++component) {
			float saved = onEdge[bad][component];
			onEdge[bad][component] = bad & 1 ? nan : infinity;
			assert(!Outside(rows, strip, onEdge, 8));
			if (bad < 7) assert(!Outside(rows, strip, onEdge, 7));
			onEdge[bad][component] = saved;
		}
	}
	assert(Outside(rows, strip, onEdge, 8));
	// A non-positive constant w keeps the draw, as the general path does.
	ClipRows flipped = rows;
	flipped.w[3] = 0.0f;
	assert(!AllVerticesOutside(flipped, strip, reinterpret_cast<uint8_t const *>(onEdge), sizeof(onEdge[0]), 8));
	return 0;
}
