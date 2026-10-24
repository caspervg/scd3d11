/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>

// While panning, SC4 shifts its backing store and redraws only the exposed strip through a small
// scissored viewport, but still submits nearly every draw of the view. Nothing outside the viewport
// is rasterized and nothing observes the draws otherwise, so a draw whose vertices all lie beyond one
// edge of that viewport can be dropped without changing a pixel.
namespace nSCD3D11 {
	// The x, y and w rows of projection x model-view, each as {x, y, z, constant} coefficients.
	struct ClipRows {
		float x[4];
		float y[4];
		float w[4];
	};

	// The rectangle that can be rasterized, in normalized device coordinates (GL convention, y up).
	struct ClipBounds {
		float left;
		float right;
		float bottom;
		float top;
	};

	// Both matrices are OpenGL column-major, m[column * 4 + row].
	void MakeClipRows(float const *projection, float const *modelView, ClipRows &rows);

	// Maps a window rectangle (GL convention, origin bottom-left), widened by marginPixels on every side,
	// into the NDC of a viewport. False for an empty viewport.
	bool MakeClipBounds(
		int32_t viewportX, int32_t viewportY, int32_t viewportWidth, int32_t viewportHeight,
		int32_t rectX, int32_t rectY, int32_t rectWidth, int32_t rectHeight, float marginPixels,
		ClipBounds &bounds);

	// True only when every vertex lies beyond the same edge of bounds. Positions are three floats at the
	// start of each stride. Stops at the first vertex that rules this out; a vertex with w <= 0 (or NaN)
	// always keeps the draw.
	bool AllVerticesOutside(
		ClipRows const &rows, ClipBounds const &bounds, uint8_t const *positions, uint32_t stride, uint32_t count);
}
