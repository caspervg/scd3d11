/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "SunPath.h"

#include "Diagnostics.h"
#include "Settings.h"

#include "GZServPtrs.h"
#include "cISC424HourClock.h"
#include "cISC4App.h"
#include "cISC4City.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace nSCD3D11::SunPath {
	namespace {
		constexpr float kDegrees = 3.14159265f / 180.0f;
		// The sun moves one step per this much visual time. SC4's clock runs
		// 135 times faster than real time, so 10 minutes is about 4.4 seconds,
		// and each step costs a full redraw of the city view.
		constexpr uint32_t kStepSeconds = 10 * 60;
		// Shadows fade out over the last degrees before sunset.
		constexpr float kFadeElevation = 6.0f;

		// cISC424HourClock::GetVisualTimePercentage (0x00445450 in 1.1.641) takes
		// a pointer to its result and returns it, not a float as gzcom-dll
		// declares, so it is called through its vtable slot.
		constexpr size_t kVisualTimeSlot = 9;

		float VisualTimeFraction(cISC424HourClock *clock) {
			using Get = float *(__thiscall *)(cISC424HourClock *, float *);
			float fraction = 0.0f;
			reinterpret_cast<Get>((*reinterpret_cast<void ***>(clock))[kVisualTimeSlot])(clock, &fraction);
			return std::isfinite(fraction) ? (std::clamp)(fraction, 0.0f, 1.0f) : 0.0f;
		}

		bool gValid = false;
		uint32_t gStep = 0;
		float gDirection[3]{};
		float gElevation = 0.0f;
	}

	void Direction(float latitude, float hour, float direction[3], float &elevation) {
		// Midsummer: the sun stands over the tropic of the latitude's hemisphere.
		float const declination = (latitude < 0.0f ? -23.44f : 23.44f) * kDegrees;
		float const hourAngle = (hour - 12.0f) * 15.0f * kDegrees;
		float const phi = latitude * kDegrees;
		// The sun's position in east, north and up.
		float const east = -std::cos(declination) * std::sin(hourAngle);
		float const north = std::cos(phi) * std::sin(declination) -
		                    std::sin(phi) * std::cos(declination) * std::cos(hourAngle);
		float const up = std::sin(phi) * std::sin(declination) +
		                 std::cos(phi) * std::cos(declination) * std::cos(hourAngle);
		elevation = std::asin((std::clamp)(up, -1.0f, 1.0f)) / kDegrees;

		// Keep the azimuth and clamp the height. A sun straight overhead
		// has no azimuth, so it leans south.
		float horizontal = std::sqrt(east * east + north * north);
		float x = horizontal > 1.0e-6f ? east / horizontal : 0.0f;
		float z = horizontal > 1.0e-6f ? -north / horizontal : 1.0f;
		float const clamped = (std::clamp)(elevation, kMinElevation, kMaxElevation) * kDegrees;
		horizontal = std::cos(clamped);
		// Light travels away from the sun.
		direction[0] = -x * horizontal;
		direction[1] = -std::sin(clamped);
		direction[2] = -z * horizontal;
	}

	bool Update() {
		std::optional<float> const &latitude = Settings::Get().sunLatitude;
		cISC4AppPtr app;
		cISC4City *const city = latitude && app ? app->GetCity() : nullptr;
		cISC424HourClock *const clock = city != nullptr ? city->Get24HourClock() : nullptr;
		if (clock == nullptr) {
			bool const changed = gValid;
			gValid = false;
			return changed;
		}
		float const fraction = VisualTimeFraction(clock);
		uint32_t const seconds = static_cast<uint32_t>(fraction * 86400.0f);
		uint32_t const step = seconds / kStepSeconds;
		if (gValid && step == gStep) return false;

		// The middle of the step, so the sun is never more than half a step off.
		float const hour = static_cast<float>(seconds / kStepSeconds * kStepSeconds + kStepSeconds / 2) / 3600.0f;
		Direction(*latitude, hour, gDirection, gElevation);
		if (!gValid) {
			Log(LogCategory::Initialization,
			    "sun path: latitude %.1f, %02u:%02u visual time, elevation %.1f, direction %.3f/%.3f/%.3f",
			    *latitude, seconds / 3600, seconds / 60 % 60, gElevation, gDirection[0], gDirection[1],
			    gDirection[2]);
		}
		gStep = step;
		gValid = true;
		return true;
	}

	bool Current(float direction[3]) {
		if (!gValid) return false;
		std::memcpy(direction, gDirection, sizeof(gDirection));
		return true;
	}

	float Strength() {
		return gValid ? (std::clamp)(gElevation / kFadeElevation, 0.0f, 1.0f) : 1.0f;
	}
} // namespace nSCD3D11::SunPath
