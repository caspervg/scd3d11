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

#include <algorithm>
#include <cstring>

namespace nSCD3D11
{
	void cGDriver::EnableLighting(bool enabled) {
		SetConstant(lightingEnabled, enabled);
	}

	void cGDriver::EnableLight(uint32_t light, bool enabled) {
		if (light >= sizeof(lightsEnabled) / sizeof(lightsEnabled[0])) {
			SetLastError(DriverError::OUT_OF_RANGE);
			return;
		}
		SetConstant(lightsEnabled[light], enabled);
	}

	// D3DRS_AMBIENT is a D3DCOLOR: clamped to [0, 1], alpha ignored.
	void cGDriver::LightModelAmbient(float red, float green, float blue, float) {
		float const ambient[3]{
			(std::clamp)(red, 0.0f, 1.0f), (std::clamp)(green, 0.0f, 1.0f), (std::clamp)(blue, 0.0f, 1.0f)};
		SetConstantBytes(globalAmbient, ambient, sizeof(ambient));
	}

	void cGDriver::LightColor(uint32_t light, uint32_t parameter, float const* color) {
		if (light >= 8 || parameter > 2 || color == nullptr) {
			SetLastError(light >= 8 ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}
		float* destination = parameter == 0 ? lightAmbient[light] :
		                     (parameter == 1 ? lightDiffuse[light] : lightSpecular[light]);
		SetConstantBytes(destination, color, sizeof(lightAmbient[light]));
	}

	void cGDriver::LightColor(
		uint32_t light, float const* ambient, float const* diffuse, float const* specular)
	{
		if (ambient) LightColor(light, 0, ambient);
		if (diffuse) LightColor(light, 1, diffuse);
		if (specular) LightColor(light, 2, specular);
	}

	void cGDriver::LightPosition(uint32_t light, float const* position) {
		if (light >= 8 || position == nullptr) {
			SetLastError(light >= 8 ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}
		SetConstantBytes(lightPosition[light], position, sizeof(lightPosition[light]));
	}

	void cGDriver::LightDirection(uint32_t light, float const* direction) {
		if (light >= 8 || direction == nullptr) {
			SetLastError(light >= 8 ? DriverError::OUT_OF_RANGE : DriverError::INVALID_VALUE);
			return;
		}
		float const position[4]{direction[0], direction[1], direction[2], 0.0f};
		SetConstantBytes(lightPosition[light], position, sizeof(position));
	}

	void cGDriver::MaterialColor(uint32_t parameter, float const* color) {
		if (parameter > 4 || color == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		if (parameter == 4) {
			SetConstant(materialShininess, color[0]);
			return;
		}
		float* destination = parameter == 0 ? materialAmbient :
			(parameter == 1 ? materialDiffuse : (parameter == 2 ? materialSpecular : materialEmission));
		SetConstantBytes(destination, color, sizeof(materialAmbient));
	}

	void cGDriver::MaterialColor(
		float const* ambient,
		float const* diffuse,
		float const* specular,
		float const* emission,
		float shininess)
	{
		if (ambient) SetConstantBytes(materialAmbient, ambient, sizeof(materialAmbient));
		if (diffuse) SetConstantBytes(materialDiffuse, diffuse, sizeof(materialDiffuse));
		if (specular) SetConstantBytes(materialSpecular, specular, sizeof(materialSpecular));
		if (emission) SetConstantBytes(materialEmission, emission, sizeof(materialEmission));
		if (shininess >= 0.0f) SetConstant(materialShininess, shininess);
	}
}
