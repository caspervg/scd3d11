/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *
 *  This class name is kept for ABI/source stability, but the implementation
 *  now targets Direct3D 9 fixed function instead of OpenGL.
 */

#pragma once

#include <vector>
#include "D3DSupport.h"
#include "cIGZGDriver.h"

class GLStateManager
{
public:
	GLStateManager();

public:
	void SetDevice(IDirect3DDevice9* device);
	void ResetStateCache();

public:
	void DrawArrays(uint32_t gdMode, int32_t first, int32_t count);
	void DrawElements(uint32_t gdMode, int32_t count, uint32_t gdType, void const* indices);
	void InterleavedArrays(uint32_t format, int32_t stride, void const* pointer);

public:
	void ColorMask(bool flag);
	void DepthFunc(uint32_t gdFunc);
	void DepthMask(bool flag);
	void StencilFunc(uint32_t gdFunc, int32_t ref, uint32_t mask);
	void StencilMask(uint32_t mask);
	void StencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass);
	void BlendFunc(uint32_t sfactor, uint32_t dfactor);
	void AlphaFunc(uint32_t func, float ref);
	void ShadeModel(uint32_t mode);
	void ColorMultiplier(float r, float g, float b);
	void AlphaMultiplier(float a);
	void EnableVertexColors(bool ambient, bool diffuse);

public:
	void MatrixMode(uint32_t mode);
	void LoadMatrix(float const* m);
	void LoadIdentity(void);

public:
	void Enable(uint32_t gdCap);
	void Disable(uint32_t gdCap);
	bool IsEnabled(uint32_t gdCap);

public:
	void TexEnv(uint32_t target, uint32_t pname, int32_t gdParam);
	void TexEnv(uint32_t target, uint32_t pname, float const* params);
	void TexParameter(uint32_t target, uint32_t pname, int32_t param);
	void TexStage(uint32_t texUnit);
	void TexStageCoord(uint32_t gdTexCoordSource);
	void TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTexMatFlags);

public:
	void BindTexture(uint32_t textureId);
	void SetTexture(uint32_t textureId, uint32_t texUnit);
	void SetTextureImmediately(uint32_t textureId);
	intptr_t GetTexture(uint32_t texUnit);
	uint32_t GetActiveTextureUnit() const;

private:
	void ApplyVertexFormat();
	void ApplyTextureStages();
	void DrawIndexedConvertedQuads(uint32_t gdType, void const* indices, int32_t count);
	DWORD CurrentFVF() const;

private:
	IDirect3DDevice9* device;
	uint32_t interleavedFormat;
	uint32_t interleavedStride;
	void const* interleavedPointer;

	uint32_t activeMatrixMode;
	uint32_t activeTextureUnit;
	uint32_t textureHandles[MAX_TEXTURE_UNITS];
	bool textureEnabled[MAX_TEXTURE_UNITS];
	uint32_t textureCoordSource[MAX_TEXTURE_UNITS];

	bool enabledCapabilities[kGDNumCapabilities];
	bool ambientLightEnabled;
	bool diffuseLightEnabled;
	float ambientLightParams[4];
	float diffuseLightParams[4];
	float textureEnvColor[4];
	bool isIdentityMatrix[2];

	std::vector<uint32_t> convertedIndices;
};
