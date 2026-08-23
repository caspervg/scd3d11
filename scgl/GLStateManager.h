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
private:
	static constexpr uint32_t StreamingVertexBufferSize = 4U * 1024U * 1024U;
	static constexpr uint32_t StreamingIndexBufferSize = 1U * 1024U * 1024U;
	static constexpr uint32_t RenderStateCount = 256;
	static constexpr uint32_t TextureStageStateCount = 64;
	static constexpr uint32_t SamplerStateCount = 16;

public:
	GLStateManager();
	~GLStateManager();

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
	void InvalidateDeviceState();

public:
	void SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
	void SetTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE state, DWORD value);
	DWORD GetTextureStageState(uint32_t stage, D3DTEXTURESTAGESTATETYPE state);
	void SetSamplerState(uint32_t stage, D3DSAMPLERSTATETYPE state, DWORD value);
	void SetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX const& matrix);
	void SetTextureNow(uint32_t stage, uint32_t textureId);

private:
	void ApplyVertexFormat();
	void ApplyTextureStages();
	void DrawIndexedConvertedQuads(uint32_t gdType, void const* indices, int32_t count);
	bool EnsureStreamingBuffers();
	bool StreamVertices(void const* source, uint32_t vertexCount, uint32_t stride, uint32_t& startVertex);
	bool StreamIndices(void const* source, uint32_t indexCount, D3DFORMAT format, uint32_t& startIndex);
	bool StreamConvertedArrayIndices(uint32_t gdMode, int32_t count, uint32_t& indexCount, uint32_t& startIndex);
	DWORD CurrentFVF() const;
	void ReleaseStreamingBuffers();

private:
	IDirect3DDevice9* device;
	IDirect3DVertexBuffer9* streamingVertexBuffer;
	IDirect3DIndexBuffer9* streamingIndexBuffer;
	uint32_t streamingVertexOffset;
	uint32_t streamingIndexOffset;
	DWORD currentFVF;
	IDirect3DVertexBuffer9* currentStream;
	uint32_t currentStreamOffset;
	uint32_t currentStreamStride;
	IDirect3DIndexBuffer9* currentIndices;

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
	DWORD renderStateCache[RenderStateCount];
	bool renderStateKnown[RenderStateCount];
	DWORD textureStageStateCache[MAX_TEXTURE_UNITS][TextureStageStateCount];
	bool textureStageStateKnown[MAX_TEXTURE_UNITS][TextureStageStateCount];
	DWORD samplerStateCache[MAX_TEXTURE_UNITS][SamplerStateCount];
	bool samplerStateKnown[MAX_TEXTURE_UNITS][SamplerStateCount];
	uint32_t boundTextureIds[MAX_TEXTURE_UNITS];

public:
	struct PerfCounters
	{
		uint64_t drawCalls;
		uint64_t primitives;
		uint64_t verticesStreamed;
		uint64_t indicesStreamed;
		uint64_t renderStateChanges;
		uint64_t textureStageStateChanges;
		uint64_t samplerStateChanges;
		uint64_t textureBinds;
		uint64_t fvfChanges;
		uint64_t streamChanges;
		uint64_t indexBufferChanges;
		uint64_t fallbackUPDraws;
	};

	PerfCounters counters;
};
