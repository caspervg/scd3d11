/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "FrameLimiter.h"

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace nSCD3D11 {
	namespace {
		int64_t Now() {
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		int64_t TicksPerSecond() {
			static int64_t const frequency = [] {
				LARGE_INTEGER value{};
				QueryPerformanceFrequency(&value);
				return value.QuadPart;
			}();
			return frequency;
		}
	}

	uint32_t FrameLimit(bool screen, uint32_t maxFps, bool vsync, uint32_t screenRefreshHz) {
		if (!screen) return maxFps;
		// A limit at the refresh rate itself drifts against the screen, which then repeats a frame now and
		// then: scrolling judders. At twice the rate, each refresh has a new frame.
		return vsync ? 0 : screenRefreshHz * 2;
	}

	FrameLimiter::~FrameLimiter() {
		if (timer != nullptr) CloseHandle(timer);
	}

	void FrameLimiter::Wait(uint32_t framesPerSecond) {
		if (framesPerSecond == 0) {
			nextFrame = 0;
			return;
		}
		int64_t const interval = TicksPerSecond() / framesPerSecond;
		int64_t now = Now();
		// A late frame starts a new schedule. Catching up would show a burst of fast frames.
		if (nextFrame == 0 || now >= nextFrame || nextFrame - now > interval) {
			nextFrame = now + interval;
			return;
		}
		if (!timerTried) {
			// Windows 10 1803 and later. Sleep is only good to a millisecond or worse.
			timerTried = true;
			timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		}
		// The timer and Sleep can be late, so the last part of the wait spins.
		int64_t const spin = TicksPerSecond() / (timer != nullptr ? 2000 : 500);
		while ((now = Now()) < nextFrame) {
			int64_t const remaining = nextFrame - now;
			if (remaining <= spin) {
				YieldProcessor();
			} else if (timer != nullptr) {
				// In 100 ns units, negative for a time from now.
				LARGE_INTEGER due{};
				due.QuadPart = -((remaining - spin) * 10000000 / TicksPerSecond());
				if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
				    WaitForSingleObject(timer, 100) == WAIT_FAILED) {
					CloseHandle(timer);
					timer = nullptr;
				}
			} else {
				Sleep(remaining > spin * 2 ? 1 : 0);
			}
		}
		nextFrame += interval;
	}
}
