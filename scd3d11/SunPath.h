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

// A sun that moves with SC4's clock, for live shadows (SunLatitude).
//
// SC4 lights its city from a fixed direction. With SunLatitude set, live
// shadows take the sun's real position instead, at that latitude on midsummer
// day, by the 24-hour clock's visual time, which is what SC4's day and night
// follow. The date is left out because SC4's calendar runs far faster than its
// clock: a whole year can pass in one visual day. World x points east and world
// z points south.
namespace nSCD3D11::SunPath {
	// The direction sunlight travels at a latitude (degrees) and local solar
	// hour (0 to 24) on that hemisphere's midsummer day, as a unit vector in
	// SC4's world axes. elevation is the sun's real height above the horizon in
	// degrees. The direction itself is held between kMinElevation and
	// kMaxElevation.
	void Direction(float latitude, float hour, float direction[3], float &elevation);

	// The same for a full moon that night: it stands opposite the sun, so in
	// midsummer it crosses low in the sky.
	void MoonDirection(float latitude, float hour, float direction[3], float &elevation);

	// Low suns make long shadows, which cost fill and redraw area. A high one
	// leaves the terrain shadows without a direction.
	constexpr float kMinElevation = 15.0f;
	constexpr float kMaxElevation = 80.0f;

	// Reads the clock. True when the sun moved far enough that the
	// city view has to be redrawn. Render thread only, once per frame.
	bool Update();

	// The current direction, or false when SunLatitude is off, no city runs or
	// SC4's day and night cycle is turned off.
	bool Current(float direction[3]);

	// How strong shadows are for the real elevation of the sun, or of the moon
	// when it casts: 1 when well up, falling to 0 at the horizon.
	float Strength();
} // namespace nSCD3D11::SunPath
