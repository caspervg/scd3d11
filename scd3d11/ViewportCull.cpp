/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "ViewportCull.h"

#include <cstring>
#include <limits>

#if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64)
#define SCD3D11_CULL_SSE2 1
#include <emmintrin.h>
#endif

namespace nSCD3D11 {
#if SCD3D11_CULL_SSE2
	namespace {
		// Below this the setup and horizontal reductions cost more than the general path's scalar loop.
		constexpr uint32_t AFFINE_MINIMUM_VERTICES = 8;

		// Exactly the 12 position bytes of a vertex, so nothing past the last one is read.
		__m128 LoadPosition(uint8_t const *position) {
			__m128 const xy = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<__m128i const *>(position)));
			float z;
			std::memcpy(&z, position + 8, sizeof(z));
			return _mm_movelh_ps(xy, _mm_set_ss(z));
		}

		// By reference: MSVC cannot pass an aligned __m128 by value on x86.
		float ReduceMin(__m128 const &lanes) {
			__m128 const v = _mm_min_ps(lanes, _mm_movehl_ps(lanes, lanes));
			return _mm_cvtss_f32(_mm_min_ss(v, _mm_shuffle_ps(v, v, 1)));
		}

		float ReduceMax(__m128 const &lanes) {
			__m128 const v = _mm_max_ps(lanes, _mm_movehl_ps(lanes, lanes));
			return _mm_cvtss_f32(_mm_max_ss(v, _mm_shuffle_ps(v, v, 1)));
		}

		// Projection x model-view with no perspective row, which is SC4's orthographic camera. Every vertex
		// then has w == rows.w[3], so "all beyond one edge" reduces to the x/y extent lying beyond that edge.
		// The sums keep the general path's evaluation order, so each comparison sees the same rounded value.
		// Any non-finite input makes the general path's w NaN, which keeps the draw, so it keeps it here too.
		bool AllVerticesOutsideAffine(
			ClipRows const &rows, ClipBounds const &ndc, uint8_t const *positions, uint32_t stride, uint32_t count) {
			float const w = rows.w[3];
			if (!(w > 0.0f)) return false;
			float const left = ndc.left * w, right = ndc.right * w, bottom = ndc.bottom * w, top = ndc.top * w;
			__m128 const ax0 = _mm_set1_ps(rows.x[0]), ax1 = _mm_set1_ps(rows.x[1]);
			__m128 const ax2 = _mm_set1_ps(rows.x[2]), ax3 = _mm_set1_ps(rows.x[3]);
			__m128 const ay0 = _mm_set1_ps(rows.y[0]), ay1 = _mm_set1_ps(rows.y[1]);
			__m128 const ay2 = _mm_set1_ps(rows.y[2]), ay3 = _mm_set1_ps(rows.y[3]);
			float const infinity = std::numeric_limits<float>::infinity();
			__m128 minX = _mm_set1_ps(infinity), maxX = _mm_set1_ps(-infinity);
			__m128 minY = minX, maxY = maxX;
			__m128 nonFinite = _mm_setzero_ps();
			uint32_t const last = count - 1;
			for (uint32_t index = 0, group = 0; index < count; index += 4, ++group) {
				// A short final group repeats the last vertex, which changes neither the extent nor finiteness.
				auto const at = [&](uint32_t offset) {
					uint32_t const vertex = index + offset < last ? index + offset : last;
					return positions + static_cast<size_t>(vertex) * stride;
				};
				__m128 px = LoadPosition(at(0)), py = LoadPosition(at(1)), pz = LoadPosition(at(2));
				__m128 unused = LoadPosition(at(3));
				_MM_TRANSPOSE4_PS(px, py, pz, unused);
				__m128 const x = _mm_add_ps(
					_mm_add_ps(_mm_add_ps(_mm_mul_ps(ax0, px), _mm_mul_ps(ax1, py)), _mm_mul_ps(ax2, pz)), ax3);
				__m128 const y = _mm_add_ps(
					_mm_add_ps(_mm_add_ps(_mm_mul_ps(ay0, px), _mm_mul_ps(ay1, py)), _mm_mul_ps(ay2, pz)), ay3);
				// v - v is NaN exactly for infinities and NaNs.
				__m128 const dx = _mm_sub_ps(px, px), dy = _mm_sub_ps(py, py), dz = _mm_sub_ps(pz, pz);
				__m128 const rx = _mm_sub_ps(x, x), ry = _mm_sub_ps(y, y);
				nonFinite = _mm_or_ps(nonFinite, _mm_or_ps(
					_mm_or_ps(_mm_cmpunord_ps(dx, dy), _mm_cmpunord_ps(dz, rx)), _mm_cmpunord_ps(ry, ry)));
				minX = _mm_min_ps(minX, x);
				maxX = _mm_max_ps(maxX, x);
				minY = _mm_min_ps(minY, y);
				maxY = _mm_max_ps(maxY, y);
				// Checking every 16 vertices keeps the horizontal reductions off the per-vertex path. Once the
				// extent reaches past every edge's inside, no further vertex can move it back.
				if ((group & 3) == 3 || index + 4 >= count) {
					if (_mm_movemask_ps(nonFinite) != 0) return false;
					if (ReduceMax(maxX) >= left && ReduceMin(minX) <= right &&
					    ReduceMax(maxY) >= bottom && ReduceMin(minY) <= top) {
						return false;
					}
				}
			}
			return true;
		}
	}
#endif

	void MakeClipRows(float const *projection, float const *modelView, ClipRows &rows) {
		float *const destinations[3] = {rows.x, rows.y, rows.w};
		unsigned const projectionRows[3] = {0, 1, 3};
		for (unsigned row = 0; row < 3; ++row) {
			unsigned const r = projectionRows[row];
			for (unsigned column = 0; column < 4; ++column) {
				float sum = 0.0f;
				for (unsigned k = 0; k < 4; ++k) sum += projection[k * 4 + r] * modelView[column * 4 + k];
				destinations[row][column] = sum;
			}
		}
	}

	bool MakeClipBounds(
		int32_t viewportX, int32_t viewportY, int32_t viewportWidth, int32_t viewportHeight,
		int32_t rectX, int32_t rectY, int32_t rectWidth, int32_t rectHeight, float marginPixels,
		ClipBounds &bounds) {
		if (viewportWidth <= 0 || viewportHeight <= 0) return false;
		float const scaleX = 2.0f / static_cast<float>(viewportWidth);
		float const scaleY = 2.0f / static_cast<float>(viewportHeight);
		bounds.left = (static_cast<float>(rectX - viewportX) - marginPixels) * scaleX - 1.0f;
		bounds.right = (static_cast<float>(rectX + rectWidth - viewportX) + marginPixels) * scaleX - 1.0f;
		bounds.bottom = (static_cast<float>(rectY - viewportY) - marginPixels) * scaleY - 1.0f;
		bounds.top = (static_cast<float>(rectY + rectHeight - viewportY) + marginPixels) * scaleY - 1.0f;
		return true;
	}

	bool AllVerticesOutside(
		ClipRows const &rows, ClipBounds const &bounds, uint8_t const *positions, uint32_t stride, uint32_t count) {
		if (count == 0) return false;
#if SCD3D11_CULL_SSE2
		if (count >= AFFINE_MINIMUM_VERTICES && rows.w[0] == 0.0f && rows.w[1] == 0.0f && rows.w[2] == 0.0f) {
			return AllVerticesOutsideAffine(rows, bounds, positions, stride, count);
		}
#endif
		return AllVerticesOutsideProjective(rows, bounds, positions, stride, count);
	}

	bool AllVerticesOutsideProjective(
		ClipRows const &rows, ClipBounds const &bounds, uint8_t const *positions, uint32_t stride, uint32_t count) {
		if (count == 0) return false;
		unsigned outside = 15;
		for (uint32_t index = 0; index < count; ++index, positions += stride) {
			float p[3];
			std::memcpy(p, positions, sizeof(p));
			float const w = rows.w[0] * p[0] + rows.w[1] * p[1] + rows.w[2] * p[2] + rows.w[3];
			// Also catches NaN: !(w > 0).
			if (!(w > 0.0f)) return false;
			float const x = rows.x[0] * p[0] + rows.x[1] * p[1] + rows.x[2] * p[2] + rows.x[3];
			float const y = rows.y[0] * p[0] + rows.y[1] * p[1] + rows.y[2] * p[2] + rows.y[3];
			unsigned const code = (x < bounds.left * w ? 1u : 0u) | (x > bounds.right * w ? 2u : 0u) |
			                      (y < bounds.bottom * w ? 4u : 0u) | (y > bounds.top * w ? 8u : 0u);
			outside &= code;
			if (outside == 0) return false;
		}
		return true;
	}
}
