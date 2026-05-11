/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 */

#include "../cGDriver.h"

namespace nSCGL
{
	void cGDriver::EnableLighting(bool flag) {
		if (d3dDevice != nullptr) {
			d3dDevice->SetRenderState(D3DRS_LIGHTING, flag);
		}
	}

	void cGDriver::EnableLight(uint32_t light, bool flag) {
		if (d3dDevice != nullptr) {
			d3dDevice->LightEnable(light, flag);
		}
	}

	void cGDriver::LightModelAmbient(float r, float g, float b, float a) {
		if (d3dDevice != nullptr) {
			d3dDevice->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(r, g, b, a));
		}
	}

	void cGDriver::LightColor(uint32_t lightIndex, uint32_t gdParam, float const* color) {
		if (d3dDevice == nullptr || color == nullptr) {
			return;
		}

		D3DLIGHT9 light{};
		d3dDevice->GetLight(lightIndex, &light);

		D3DCOLORVALUE value{ color[0], color[1], color[2], color[3] };
		switch (gdParam) {
		case 0: light.Ambient = value; break;
		case 1: light.Diffuse = value; break;
		case 2: light.Specular = value; break;
		default: return;
		}

		d3dDevice->SetLight(lightIndex, &light);
	}

	void cGDriver::LightColor(uint32_t lightIndex, float const* ambient, float const* diffuse, float const* specular) {
		if (d3dDevice == nullptr) {
			return;
		}

		D3DLIGHT9 light{};
		d3dDevice->GetLight(lightIndex, &light);

		if (ambient) {
			light.Ambient = D3DCOLORVALUE{ ambient[0], ambient[1], ambient[2], ambient[3] };
		}

		if (diffuse) {
			light.Diffuse = D3DCOLORVALUE{ diffuse[0], diffuse[1], diffuse[2], diffuse[3] };
		}

		if (specular) {
			light.Specular = D3DCOLORVALUE{ specular[0], specular[1], specular[2], specular[3] };
		}

		d3dDevice->SetLight(lightIndex, &light);
	}

	void cGDriver::LightPosition(uint32_t lightIndex, float const* position) {
		if (d3dDevice == nullptr || position == nullptr) {
			return;
		}

		D3DLIGHT9 light{};
		d3dDevice->GetLight(lightIndex, &light);
		light.Type = position[3] == 0.0f ? D3DLIGHT_DIRECTIONAL : D3DLIGHT_POINT;
		light.Position.x = position[0];
		light.Position.y = position[1];
		light.Position.z = position[2];
		light.Direction.x = -position[0];
		light.Direction.y = -position[1];
		light.Direction.z = -position[2];
		d3dDevice->SetLight(lightIndex, &light);
	}

	void cGDriver::LightDirection(uint32_t lightIndex, float const* direction) {
		if (d3dDevice == nullptr || direction == nullptr) {
			return;
		}

		D3DLIGHT9 light{};
		d3dDevice->GetLight(lightIndex, &light);
		light.Direction.x = direction[0];
		light.Direction.y = direction[1];
		light.Direction.z = direction[2];
		d3dDevice->SetLight(lightIndex, &light);
	}

	void cGDriver::MaterialColor(uint32_t gdParam, float const* color) {
		if (d3dDevice == nullptr || color == nullptr) {
			return;
		}

		D3DMATERIAL9 material{};
		d3dDevice->GetMaterial(&material);
		D3DCOLORVALUE value{ color[0], color[1], color[2], color[3] };

		switch (gdParam) {
		case 0: material.Ambient = value; break;
		case 1: material.Diffuse = value; break;
		case 2: material.Specular = value; break;
		case 3: material.Emissive = value; break;
		case 4: material.Power = color[0]; break;
		default: return;
		}

		d3dDevice->SetMaterial(&material);
	}

	void cGDriver::MaterialColor(
		float const* ambient,
		float const* diffuse,
		float const* specular,
		float const* emission,
		float shininess)
	{
		if (d3dDevice == nullptr) {
			return;
		}

		D3DMATERIAL9 material{};
		d3dDevice->GetMaterial(&material);

		if (ambient) {
			material.Ambient = D3DCOLORVALUE{ ambient[0], ambient[1], ambient[2], ambient[3] };
		}

		if (diffuse) {
			material.Diffuse = D3DCOLORVALUE{ diffuse[0], diffuse[1], diffuse[2], diffuse[3] };
		}

		if (specular) {
			material.Specular = D3DCOLORVALUE{ specular[0], specular[1], specular[2], specular[3] };
		}

		if (emission) {
			material.Emissive = D3DCOLORVALUE{ emission[0], emission[1], emission[2], emission[3] };
		}

		if (shininess >= 0.0f) {
			material.Power = shininess;
		}

		d3dDevice->SetMaterial(&material);
	}
}

