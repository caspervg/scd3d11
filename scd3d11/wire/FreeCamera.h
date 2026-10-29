/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// The player's own camera (SCD3D12 M7), as the shim sends it in a FreeCamera record. It is told as a change to
// SC4's camera, so SC4's own scrolling still moves it: the view turned about world up through the ground at
// the middle of the screen, tilted about that point, and zoomed into it. The server draws the city from it,
// and hands the view and projection it drew with back to the shim for picking (RingControl).
namespace nSCDWire {
	struct FreeCamera {
		uint32_t on; // 0: SC4's own camera
		float yaw; // degrees added to SC4's turn about world up
		float pitch; // degrees above the horizon, 0 for SC4's own
		float zoom; // SC4's metres per pixel are divided by this
	};
	static_assert(sizeof(FreeCamera) == 16);

	// Where the middle of the screen looks: a point on the ray and its direction, both in the world, from a view
	// and orthographic projection (column major, m[column * 4 + row]).
	inline void ScreenMiddleRay(float const *view, float const *projection, float *origin, float *direction) {
		// The view space point at the screen's middle and depth 0.
		float const x = -projection[12] / projection[0], y = -projection[13] / projection[5];
		for (int a = 0; a < 3; ++a) {
			// The view's rotation is orthonormal, so its inverse is its transpose.
			float const r0 = view[a * 4], r1 = view[a * 4 + 1], r2 = view[a * 4 + 2];
			origin[a] = r0 * (x - view[12]) + r1 * (y - view[13]) + r2 * (0.0f - view[14]);
			direction[a] = -r2;
		}
	}

	// The free camera's view and projection from SC4's. pivot is the world point at the middle of the screen,
	// which keeps its place on screen and its depth. With yaw 0, pitch 0 and zoom 1 the result is SC4's camera.
	inline void FreeView(float const *view, float const *projection, float const *pivot, FreeCamera const &camera,
	                     float *outView, float *outProjection) {
		constexpr float kRadians = 3.14159265358979f / 180.0f;
		float const r0[3]{view[0], view[4], view[8]}, r2[3]{view[2], view[6], view[10]};
		float const yaw = std::atan2(r0[2], r0[0]) + camera.yaw * kRadians;
		float const pitch = camera.pitch != 0.0f ? std::clamp(camera.pitch, 1.0f, 90.0f) * kRadians
		                                         : std::asin(std::clamp(r2[1], -1.0f, 1.0f));
		float const cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
		// Which way the view's depth axis leans from the x axis, as in SC4's view.
		float const lean = -r2[0] * std::sin(std::atan2(r0[2], r0[0])) + r2[2] * std::cos(std::atan2(r0[2], r0[0])) < 0.0f
			                   ? -1.0f : 1.0f;
		float const x[3]{cy, 0.0f, sy};
		float const z[3]{-sy * cp * lean, sp, cy * cp * lean};
		// y = z cross x, or the other way round, as SC4's view has it.
		float y[3]{z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
		float const r1[3]{view[1], view[5], view[9]};
		float const cross[3]{r2[1] * r0[2] - r2[2] * r0[1], r2[2] * r0[0] - r2[0] * r0[2], r2[0] * r0[1] - r2[1] * r0[0]};
		if (r1[0] * cross[0] + r1[1] * cross[1] + r1[2] * cross[2] < 0.0f) {
			for (float &value: y) value = -value;
		}
		float const *const rows[3]{x, y, z};
		std::memset(outView, 0, 16 * sizeof(float));
		for (int row = 0; row < 3; ++row) {
			// The pivot stays where SC4's view has it.
			float const before = view[row] * pivot[0] + view[4 + row] * pivot[1] + view[8 + row] * pivot[2] + view[12 + row];
			float const turned = rows[row][0] * pivot[0] + rows[row][1] * pivot[1] + rows[row][2] * pivot[2];
			for (int c = 0; c < 3; ++c) outView[c * 4 + row] = rows[row][c];
			outView[12 + row] = before - turned;
		}
		outView[15] = 1.0f;
		std::memcpy(outProjection, projection, 16 * sizeof(float));
		float const zoom = camera.zoom > 0.0f ? camera.zoom : 1.0f;
		for (int i: {0, 5, 12, 13}) outProjection[i] *= zoom;
	}
}
