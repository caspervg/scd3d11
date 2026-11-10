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

#include <cstdint>

namespace nSCD3D11 {
	// The frame rate to hold for a MaxFPS setting, or 0 for no limit. With MaxFPS=screen, VSync already
	// holds the screen's rate, so only VSync=off needs the limiter: twice the screen's rate.
	uint32_t FrameLimit(bool screen, uint32_t maxFps, bool vsync, uint32_t screenRefreshHz);

	// Holds SC4's frame loop to a frame rate. Without a limit and without VSync, a cheap view such as the
	// region runs at over 1000 frames a second, which only makes heat and coil whine.
	class FrameLimiter {
	public:
		~FrameLimiter();
		// Waits until the next frame is due. Called once a frame, before Present. 0 does not wait.
		void Wait(uint32_t framesPerSecond);

	private:
		// When the next frame is due, in QueryPerformanceCounter ticks. 0 before the first frame.
		int64_t nextFrame = 0;
		void *timer = nullptr;
		bool timerTried = false;
	};
}
