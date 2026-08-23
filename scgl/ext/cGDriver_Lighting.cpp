/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "../cGDriver.h"
#include "../Diagnostics.h"

#include <cstring>

namespace nSCGL
{
	void cGDriver::EnableLighting(bool enabled) {
		lightingEnabled = enabled;
	}

	void cGDriver::EnableLight(uint32_t light, bool enabled) {
		if (light >= sizeof(lightsEnabled) / sizeof(lightsEnabled[0])) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}
		lightsEnabled[light] = enabled;
		if (light != 0 && enabled) Log(LogCategory::Unsupported, "light %u enabled; generic shader currently evaluates light 0", light);
	}

	void cGDriver::LightModelAmbient(float red, float green, float blue, float alpha) {
		globalAmbient[0] = red;
		globalAmbient[1] = green;
		globalAmbient[2] = blue;
		globalAmbient[3] = alpha;
	}

	void cGDriver::LightColor(uint32_t light, uint32_t parameter, float const* color) {
		if (light != 0 || parameter > 2 || color == nullptr) {
			if (light != 0) Log(LogCategory::Unsupported, "color for light %u is not translated", light);
			else SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		float* destination = parameter == 0 ? lightAmbient : (parameter == 1 ? lightDiffuse : lightSpecular);
		memcpy(destination, color, sizeof(lightAmbient));
	}

	void cGDriver::LightColor(
		uint32_t light, float const* ambient, float const* diffuse, float const* specular)
	{
		if (ambient) LightColor(light, 0, ambient);
		if (diffuse) LightColor(light, 1, diffuse);
		if (specular) LightColor(light, 2, specular);
	}

	void cGDriver::LightPosition(uint32_t light, float const* position) {
		if (light != 0 || position == nullptr) {
			if (light != 0) Log(LogCategory::Unsupported, "position for light %u is not translated", light);
			else SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		if (position[3] != 0.0f) {
			Log(LogCategory::Unsupported, "positional light requested; generic shader currently supports directional light 0");
		}
		memcpy(lightDirection, position, sizeof(lightDirection));
	}

	void cGDriver::LightDirection(uint32_t light, float const* direction) {
		if (light != 0 || direction == nullptr) {
			if (light != 0) Log(LogCategory::Unsupported, "direction for light %u is not translated", light);
			else SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		memcpy(lightDirection, direction, sizeof(float) * 3);
		lightDirection[3] = 0.0f;
	}

	void cGDriver::MaterialColor(uint32_t parameter, float const* color) {
		if (parameter > 4 || color == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		if (parameter == 4) {
			materialShininess = color[0];
			return;
		}
		float* destination = parameter == 0 ? materialAmbient :
			(parameter == 1 ? materialDiffuse : (parameter == 2 ? materialSpecular : materialEmission));
		memcpy(destination, color, sizeof(materialAmbient));
	}

	void cGDriver::MaterialColor(
		float const* ambient,
		float const* diffuse,
		float const* specular,
		float const* emission,
		float shininess)
	{
		if (ambient) memcpy(materialAmbient, ambient, sizeof(materialAmbient));
		if (diffuse) memcpy(materialDiffuse, diffuse, sizeof(materialDiffuse));
		if (specular) memcpy(materialSpecular, specular, sizeof(materialSpecular));
		if (emission) memcpy(materialEmission, emission, sizeof(materialEmission));
		if (shininess >= 0.0f) materialShininess = shininess;
	}
}
