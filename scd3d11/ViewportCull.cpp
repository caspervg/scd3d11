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

namespace nSCD3D11 {
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
