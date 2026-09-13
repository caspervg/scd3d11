/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
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

namespace nSCD3D11
{
	void cGDriver::EnableLighting(bool enabled) {
		constantsDirty = true;
		lightingEnabled = enabled;
	}

	void cGDriver::EnableLight(uint32_t light, bool enabled) {
		constantsDirty = true;
		if (light >= sizeof(lightsEnabled) / sizeof(lightsEnabled[0])) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}
		lightsEnabled[light] = enabled;
	}

	void cGDriver::LightModelAmbient(float red, float green, float blue, float alpha) {
		constantsDirty = true;
		globalAmbient[0] = red;
		globalAmbient[1] = green;
		globalAmbient[2] = blue;
		globalAmbient[3] = alpha;
	}

	void cGDriver::LightColor(uint32_t light, uint32_t parameter, float const* color) {
		constantsDirty = true;
		if (light >= 8 || parameter > 2 || color == nullptr) {
			SetLastError(light >= 8 ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}
		float* destination = parameter == 0 ? lightAmbient[light] :
		                     (parameter == 1 ? lightDiffuse[light] : lightSpecular[light]);
		memcpy(destination, color, sizeof(lightAmbient[light]));
	}

	void cGDriver::LightColor(
		uint32_t light, float const* ambient, float const* diffuse, float const* specular)
	{
		constantsDirty = true;
		if (ambient) LightColor(light, 0, ambient);
		if (diffuse) LightColor(light, 1, diffuse);
		if (specular) LightColor(light, 2, specular);
	}

	void cGDriver::LightPosition(uint32_t light, float const* position) {
		constantsDirty = true;
		if (light >= 8 || position == nullptr) {
			SetLastError(light >= 8 ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}
		memcpy(lightPosition[light], position, sizeof(lightPosition[light]));
	}

	void cGDriver::LightDirection(uint32_t light, float const* direction) {
		constantsDirty = true;
		if (light >= 8 || direction == nullptr) {
			SetLastError(light >= 8 ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}
		memcpy(lightPosition[light], direction, sizeof(float) * 3);
		lightPosition[light][3] = 0.0f;
	}

	void cGDriver::MaterialColor(uint32_t parameter, float const* color) {
		constantsDirty = true;
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
		constantsDirty = true;
		if (ambient) memcpy(materialAmbient, ambient, sizeof(materialAmbient));
		if (diffuse) memcpy(materialDiffuse, diffuse, sizeof(materialDiffuse));
		if (specular) memcpy(materialSpecular, specular, sizeof(materialSpecular));
		if (emission) memcpy(materialEmission, emission, sizeof(materialEmission));
		if (shininess >= 0.0f) materialShininess = shininess;
	}
}
