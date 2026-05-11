/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *
 *  Direct3D 9 fixed-function state manager.
 */

#include <cassert>
#include <cstring>
#include "GLStateManager.h"
#include "VertexFormatUtils.h"

static D3DPRIMITIVETYPE d3dPrimitiveMap[8] = {
	D3DPT_TRIANGLELIST,
	D3DPT_TRIANGLESTRIP,
	D3DPT_TRIANGLEFAN,
	D3DPT_POINTLIST,
	D3DPT_LINELIST,
	D3DPT_LINESTRIP,
	D3DPT_TRIANGLELIST, // SimGL quads are expanded before drawing.
	D3DPT_TRIANGLELIST, // SimGL quad strips are expanded before drawing.
};

static D3DCMPFUNC d3dFuncMap[8] = {
	D3DCMP_NEVER,
	D3DCMP_LESS,
	D3DCMP_EQUAL,
	D3DCMP_LESSEQUAL,
	D3DCMP_GREATER,
	D3DCMP_NOTEQUAL,
	D3DCMP_GREATEREQUAL,
	D3DCMP_ALWAYS,
};

static D3DBLEND d3dBlendMap[11] = {
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_SRCCOLOR,
	D3DBLEND_INVSRCCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
	D3DBLEND_DESTCOLOR,
	D3DBLEND_INVDESTCOLOR,
	D3DBLEND_SRCALPHASAT,
};

static D3DTEXTUREOP d3dTextureOpMap[] = {
	D3DTOP_SELECTARG1,
	D3DTOP_MODULATE,
	D3DTOP_ADD,
	D3DTOP_ADDSIGNED,
	D3DTOP_BLENDCURRENTALPHA,
	D3DTOP_DOTPRODUCT3,
};

static DWORD d3dTextureArgMap[] = {
	D3DTA_TEXTURE,
	D3DTA_CURRENT,
	D3DTA_TFACTOR,
	D3DTA_DIFFUSE,
};

static uint32_t PrimitiveCount(uint32_t gdMode, int32_t vertexCount)
{
	switch (gdMode) {
	case 0: return vertexCount / 3;
	case 1: return vertexCount > 2 ? vertexCount - 2 : 0;
	case 2: return vertexCount > 2 ? vertexCount - 2 : 0;
	case 3: return vertexCount;
	case 4: return vertexCount / 2;
	case 5: return vertexCount > 1 ? vertexCount - 1 : 0;
	case 6: return (vertexCount / 4) * 2;
	case 7: return vertexCount >= 4 ? ((vertexCount - 2) / 2) * 2 : 0;
	default: return 0;
	}
}

static D3DMATRIX ToD3DMatrix(float const* m)
{
	D3DMATRIX matrix{};
	std::memcpy(&matrix, m, sizeof(matrix));
	return matrix;
}

static void SetDefaultTextureStage(IDirect3DDevice9* device, uint32_t stage)
{
	device->SetTextureStageState(stage, D3DTSS_COLOROP, D3DTOP_MODULATE);
	device->SetTextureStageState(stage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(stage, D3DTSS_COLORARG2, stage == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
	device->SetTextureStageState(stage, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	device->SetTextureStageState(stage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(stage, D3DTSS_ALPHAARG2, stage == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
}

static D3DTextureHandle* TextureFromId(uint32_t textureId)
{
	return reinterpret_cast<D3DTextureHandle*>(static_cast<uintptr_t>(textureId));
}

GLStateManager::GLStateManager() :
	device(nullptr),
	interleavedFormat(0),
	interleavedStride(0),
	interleavedPointer(nullptr),
	activeMatrixMode(0),
	activeTextureUnit(0),
	textureHandles{},
	textureEnabled{ true, false },
	textureCoordSource{},
	enabledCapabilities{},
	ambientLightEnabled(false),
	diffuseLightEnabled(false),
	ambientLightParams{ 0.2f, 0.2f, 0.2f, 1.0f },
	diffuseLightParams{ 1.0f, 1.0f, 1.0f, 1.0f },
	textureEnvColor{ 0.0f, 0.0f, 0.0f, 0.0f },
	isIdentityMatrix{ true, true },
	convertedIndices()
{
}

void GLStateManager::SetDevice(IDirect3DDevice9* newDevice)
{
	device = newDevice;
	ResetStateCache();
}

void GLStateManager::ResetStateCache()
{
	interleavedFormat = 0;
	interleavedStride = 0;
	interleavedPointer = nullptr;
	activeMatrixMode = 0;
	activeTextureUnit = 0;
	textureHandles[0] = 0;
	textureHandles[1] = 0;
	textureEnabled[0] = true;
	textureEnabled[1] = false;
	textureCoordSource[0] = 0;
	textureCoordSource[1] = 0;
	std::memset(enabledCapabilities, 0, sizeof(enabledCapabilities));
	isIdentityMatrix[0] = true;
	isIdentityMatrix[1] = true;

	if (device != nullptr) {
		SetDefaultTextureStage(device, 0);
		device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	}
}

DWORD GLStateManager::CurrentFVF() const
{
	DWORD fvf = D3DFVF_XYZ;

	if (RZVertexFormatNumElements(interleavedFormat, kGDElementType_Normal) != 0) {
		fvf |= D3DFVF_NORMAL;
	}

	if (RZVertexFormatNumElements(interleavedFormat, kGDElementType_Color) != 0) {
		fvf |= D3DFVF_DIFFUSE;
	}

	uint32_t texCoordCount = RZVertexFormatNumElements(interleavedFormat, kGDElementType_TexCoord);
	if (texCoordCount > 0) {
		fvf |= texCoordCount << D3DFVF_TEXCOUNT_SHIFT;
	}

	return fvf;
}

void GLStateManager::ApplyVertexFormat()
{
	if (device == nullptr) {
		return;
	}

	device->SetFVF(CurrentFVF());
}

void GLStateManager::ApplyTextureStages()
{
	if (device == nullptr) {
		return;
	}

	for (uint32_t i = 0; i < MAX_TEXTURE_UNITS; i++) {
		D3DTextureHandle* handle = TextureFromId(textureHandles[i]);
		device->SetTexture(i, (textureEnabled[i] && handle != nullptr) ? handle->texture : nullptr);

		if (textureEnabled[i]) {
			device->SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, textureCoordSource[i]);
		}
		else {
			device->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
			device->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}
	}
}

void GLStateManager::DrawArrays(uint32_t gdMode, int32_t first, int32_t count)
{
	SIZE_CHECK(gdMode, d3dPrimitiveMap);
	if (device == nullptr || interleavedPointer == nullptr || count <= 0) {
		return;
	}

	ApplyVertexFormat();
	ApplyTextureStages();

	uint8_t const* vertices = reinterpret_cast<uint8_t const*>(interleavedPointer) + (first * interleavedStride);
	uint32_t primitiveCount = PrimitiveCount(gdMode, count);
	if (primitiveCount == 0) {
		return;
	}

	if (gdMode == 6 || gdMode == 7) {
		convertedIndices.clear();
		if (gdMode == 6) {
			for (uint32_t i = 0; i + 3 < static_cast<uint32_t>(count); i += 4) {
				convertedIndices.push_back(i);
				convertedIndices.push_back(i + 1);
				convertedIndices.push_back(i + 2);
				convertedIndices.push_back(i);
				convertedIndices.push_back(i + 2);
				convertedIndices.push_back(i + 3);
			}
		}
		else {
			for (uint32_t i = 0; i + 3 < static_cast<uint32_t>(count); i += 2) {
				convertedIndices.push_back(i);
				convertedIndices.push_back(i + 1);
				convertedIndices.push_back(i + 2);
				convertedIndices.push_back(i + 1);
				convertedIndices.push_back(i + 3);
				convertedIndices.push_back(i + 2);
			}
		}

		device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, count, primitiveCount, convertedIndices.data(), D3DFMT_INDEX32, vertices, interleavedStride);
		return;
	}

	device->DrawPrimitiveUP(d3dPrimitiveMap[gdMode], primitiveCount, vertices, interleavedStride);
}

void GLStateManager::DrawIndexedConvertedQuads(uint32_t gdType, void const* indices, int32_t count)
{
	convertedIndices.clear();
	uint32_t maxIndex = 0;

	if (gdType == 3) {
		uint16_t const* src = reinterpret_cast<uint16_t const*>(indices);
		for (int32_t i = 0; i + 3 < count; i += 4) {
			maxIndex = max(maxIndex, static_cast<uint32_t>(src[i]));
			maxIndex = max(maxIndex, static_cast<uint32_t>(src[i + 1]));
			maxIndex = max(maxIndex, static_cast<uint32_t>(src[i + 2]));
			maxIndex = max(maxIndex, static_cast<uint32_t>(src[i + 3]));
			convertedIndices.push_back(src[i]);
			convertedIndices.push_back(src[i + 1]);
			convertedIndices.push_back(src[i + 2]);
			convertedIndices.push_back(src[i]);
			convertedIndices.push_back(src[i + 2]);
			convertedIndices.push_back(src[i + 3]);
		}
	}
	else if (gdType == 5) {
		uint32_t const* src = reinterpret_cast<uint32_t const*>(indices);
		for (int32_t i = 0; i + 3 < count; i += 4) {
			maxIndex = max(maxIndex, src[i]);
			maxIndex = max(maxIndex, src[i + 1]);
			maxIndex = max(maxIndex, src[i + 2]);
			maxIndex = max(maxIndex, src[i + 3]);
			convertedIndices.push_back(src[i]);
			convertedIndices.push_back(src[i + 1]);
			convertedIndices.push_back(src[i + 2]);
			convertedIndices.push_back(src[i]);
			convertedIndices.push_back(src[i + 2]);
			convertedIndices.push_back(src[i + 3]);
		}
	}
	else {
		return;
	}

	device->DrawIndexedPrimitiveUP(
		D3DPT_TRIANGLELIST,
		0,
		maxIndex + 1,
		(count / 4) * 2,
		convertedIndices.data(),
		D3DFMT_INDEX32,
		interleavedPointer,
		interleavedStride);
}

void GLStateManager::DrawElements(uint32_t gdMode, int32_t count, uint32_t gdType, void const* indices)
{
	SIZE_CHECK(gdMode, d3dPrimitiveMap);
	if (device == nullptr || interleavedPointer == nullptr || indices == nullptr || count <= 0) {
		return;
	}

	ApplyVertexFormat();
	ApplyTextureStages();

	uint32_t primitiveCount = PrimitiveCount(gdMode, count);
	if (primitiveCount == 0) {
		return;
	}

	if (gdMode == 6) {
		DrawIndexedConvertedQuads(gdType, indices, count);
		return;
	}

	D3DFORMAT indexFormat;
	uint32_t maxIndex = 0;
	if (gdType == 3) {
		indexFormat = D3DFMT_INDEX16;
		uint16_t const* src = reinterpret_cast<uint16_t const*>(indices);
		for (int32_t i = 0; i < count; i++) {
			maxIndex = max(maxIndex, static_cast<uint32_t>(src[i]));
		}
	}
	else if (gdType == 5) {
		indexFormat = D3DFMT_INDEX32;
		uint32_t const* src = reinterpret_cast<uint32_t const*>(indices);
		for (int32_t i = 0; i < count; i++) {
			maxIndex = max(maxIndex, src[i]);
		}
	}
	else {
		UNEXPECTED();
		return;
	}

	device->DrawIndexedPrimitiveUP(d3dPrimitiveMap[gdMode], 0, maxIndex + 1, primitiveCount, indices, indexFormat, interleavedPointer, interleavedStride);
}

void GLStateManager::InterleavedArrays(uint32_t format, int32_t stride, void const* pointer)
{
	interleavedFormat = format;
	interleavedStride = stride;
	interleavedPointer = pointer;
}

void GLStateManager::ColorMask(bool flag)
{
	if (device != nullptr) {
		device->SetRenderState(D3DRS_COLORWRITEENABLE, flag ? (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA) : 0);
	}
}

void GLStateManager::DepthFunc(uint32_t gdFunc)
{
	SIZE_CHECK(gdFunc, d3dFuncMap);
	if (device != nullptr) {
		device->SetRenderState(D3DRS_ZFUNC, d3dFuncMap[gdFunc]);
	}
}

void GLStateManager::DepthMask(bool flag)
{
	if (device != nullptr) {
		device->SetRenderState(D3DRS_ZWRITEENABLE, flag);
	}
}

void GLStateManager::StencilFunc(uint32_t gdFunc, int32_t ref, uint32_t mask)
{
	SIZE_CHECK(gdFunc, d3dFuncMap);
	if (device != nullptr) {
		device->SetRenderState(D3DRS_STENCILFUNC, d3dFuncMap[gdFunc]);
		device->SetRenderState(D3DRS_STENCILREF, ref);
		device->SetRenderState(D3DRS_STENCILMASK, mask);
	}
}

void GLStateManager::StencilMask(uint32_t mask)
{
	if (device != nullptr) {
		device->SetRenderState(D3DRS_STENCILWRITEMASK, mask);
	}
}

void GLStateManager::StencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass)
{
	static D3DSTENCILOP d3dStencilMap[] = { D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_INCRSAT, D3DSTENCILOP_DECRSAT, D3DSTENCILOP_INVERT };
	SIZE_CHECK(fail, d3dStencilMap);
	SIZE_CHECK(zfail, d3dStencilMap);
	SIZE_CHECK(zpass, d3dStencilMap);

	if (device != nullptr) {
		device->SetRenderState(D3DRS_STENCILFAIL, d3dStencilMap[fail]);
		device->SetRenderState(D3DRS_STENCILZFAIL, d3dStencilMap[zfail]);
		device->SetRenderState(D3DRS_STENCILPASS, d3dStencilMap[zpass]);
	}
}

void GLStateManager::BlendFunc(uint32_t sfactor, uint32_t dfactor)
{
	SIZE_CHECK(sfactor, d3dBlendMap);
	SIZE_CHECK(dfactor, d3dBlendMap);

	if (device != nullptr) {
		device->SetRenderState(D3DRS_SRCBLEND, d3dBlendMap[sfactor]);
		device->SetRenderState(D3DRS_DESTBLEND, d3dBlendMap[dfactor]);
	}
}

void GLStateManager::AlphaFunc(uint32_t func, float ref)
{
	SIZE_CHECK(func, d3dFuncMap);
	if (device != nullptr) {
		device->SetRenderState(D3DRS_ALPHAFUNC, d3dFuncMap[func]);
		device->SetRenderState(D3DRS_ALPHAREF, static_cast<DWORD>(ref * 255.0f));
	}
}

void GLStateManager::ShadeModel(uint32_t mode)
{
	static D3DSHADEMODE shadeModelMap[] = { D3DSHADE_FLAT, D3DSHADE_GOURAUD };
	SIZE_CHECK(mode, shadeModelMap);
	if (device != nullptr) {
		device->SetRenderState(D3DRS_SHADEMODE, shadeModelMap[mode]);
	}
}

void GLStateManager::ColorMultiplier(float r, float g, float b)
{
	ambientLightParams[0] = r;
	ambientLightParams[1] = g;
	ambientLightParams[2] = b;

	if (device != nullptr) {
		device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(r, g, b, ambientLightParams[3]));
	}
}

void GLStateManager::AlphaMultiplier(float a)
{
	diffuseLightParams[3] = a;
}

void GLStateManager::EnableVertexColors(bool ambient, bool diffuse)
{
	ambientLightEnabled = ambient;
	diffuseLightEnabled = diffuse;

	if (device != nullptr) {
		device->SetRenderState(D3DRS_COLORVERTEX, ambient || diffuse);
		device->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, ambient ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
		device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, diffuse ? D3DMCS_COLOR1 : D3DMCS_MATERIAL);
	}
}

void GLStateManager::MatrixMode(uint32_t mode)
{
	SIZE_CHECK(mode, isIdentityMatrix);
	activeMatrixMode = mode;
}

void GLStateManager::LoadMatrix(float const* m)
{
	if (device == nullptr || m == nullptr) {
		return;
	}

	D3DMATRIX matrix = ToD3DMatrix(m);
	device->SetTransform(activeMatrixMode == 0 ? D3DTS_VIEW : D3DTS_PROJECTION, &matrix);
	isIdentityMatrix[activeMatrixMode] = false;
}

void GLStateManager::LoadIdentity(void)
{
	if (device == nullptr || isIdentityMatrix[activeMatrixMode]) {
		return;
	}

	D3DMATRIX identity{};
	identity._11 = 1.0f;
	identity._22 = 1.0f;
	identity._33 = 1.0f;
	identity._44 = 1.0f;
	device->SetTransform(activeMatrixMode == 0 ? D3DTS_VIEW : D3DTS_PROJECTION, &identity);
	isIdentityMatrix[activeMatrixMode] = true;
}

void GLStateManager::Enable(uint32_t gdCap)
{
	SIZE_CHECK(gdCap, enabledCapabilities);
	enabledCapabilities[gdCap] = true;

	if (device == nullptr) {
		return;
	}

	switch (gdCap) {
	case kGDCapability_AlphaTest: device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); break;
	case kGDCapability_DepthTest: device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE); break;
	case kGDCapability_StencilTest: device->SetRenderState(D3DRS_STENCILENABLE, TRUE); break;
	case kGDCapability_CullFace: device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW); break;
	case kGDCapability_Blend: device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); break;
	case kGDCapability_Texture2D:
		textureEnabled[activeTextureUnit] = true;
		SetDefaultTextureStage(device, activeTextureUnit);
		break;
	case kGDCapability_Fog: device->SetRenderState(D3DRS_FOGENABLE, TRUE); break;
	default: break;
	}
}

void GLStateManager::Disable(uint32_t gdCap)
{
	SIZE_CHECK(gdCap, enabledCapabilities);
	enabledCapabilities[gdCap] = false;

	if (device == nullptr) {
		return;
	}

	switch (gdCap) {
	case kGDCapability_AlphaTest: device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); break;
	case kGDCapability_DepthTest: device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE); break;
	case kGDCapability_StencilTest: device->SetRenderState(D3DRS_STENCILENABLE, FALSE); break;
	case kGDCapability_CullFace: device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); break;
	case kGDCapability_Blend: device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); break;
	case kGDCapability_Texture2D: textureEnabled[activeTextureUnit] = false; break;
	case kGDCapability_Fog: device->SetRenderState(D3DRS_FOGENABLE, FALSE); break;
	default: break;
	}
}

bool GLStateManager::IsEnabled(uint32_t gdCap)
{
	SIZE_CHECK_RETVAL(gdCap, enabledCapabilities, false);
	return enabledCapabilities[gdCap];
}

void GLStateManager::TexEnv(uint32_t, uint32_t pname, int32_t gdParam)
{
	if (device == nullptr) {
		return;
	}

	if (pname == kGDTextureEnvParamType_Mode) {
		static D3DTEXTUREOP envOpMap[] = { D3DTOP_SELECTARG1, D3DTOP_MODULATE, D3DTOP_SELECTARG1, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_MODULATE, D3DTOP_MODULATE };
		SIZE_CHECK(gdParam, envOpMap);

		device->SetTextureStageState(activeTextureUnit, D3DTSS_COLOROP, envOpMap[gdParam]);
		device->SetTextureStageState(activeTextureUnit, D3DTSS_ALPHAOP, envOpMap[gdParam]);
	}
}

void GLStateManager::TexEnv(uint32_t, uint32_t pname, float const* params)
{
	if (params == nullptr || pname != kGDTextureEnvParamType_Color) {
		return;
	}

	std::memcpy(textureEnvColor, params, sizeof(textureEnvColor));
	if (device != nullptr) {
		device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(params[0], params[1], params[2], params[3]));
	}
}

void GLStateManager::TexParameter(uint32_t, uint32_t pname, int32_t param)
{
	static D3DSAMPLERSTATETYPE samplerNameMap[] = { D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV };
	static DWORD samplerParamMap[] = { D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_LINEAR };
	SIZE_CHECK(pname, samplerNameMap);
	SIZE_CHECK(param, samplerParamMap);

	if (device != nullptr) {
		for (uint32_t i = 0; i < MAX_TEXTURE_UNITS; i++) {
			device->SetSamplerState(i, samplerNameMap[pname], samplerParamMap[param]);
			if (pname == 1 && param >= 4) {
				device->SetSamplerState(i, D3DSAMP_MIPFILTER, param == 4 || param == 6 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
			}
		}
	}
}

void GLStateManager::TexStage(uint32_t texUnit)
{
	if (texUnit < MAX_TEXTURE_UNITS) {
		activeTextureUnit = texUnit;
	}
}

void GLStateManager::TexStageCoord(uint32_t gdTexCoordSource)
{
	textureCoordSource[activeTextureUnit] = gdTexCoordSource;
}

void GLStateManager::TexStageMatrix(float const* matrix, uint32_t, uint32_t, uint32_t gdTexMatFlags)
{
	if (device == nullptr) {
		return;
	}

	if (matrix == nullptr) {
		D3DMATRIX identity{};
		identity._11 = 1.0f;
		identity._22 = 1.0f;
		identity._33 = 1.0f;
		identity._44 = 1.0f;
		device->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + activeTextureUnit), &identity);
		device->SetTextureStageState(activeTextureUnit, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		return;
	}

	D3DMATRIX d3dMatrix = ToD3DMatrix(matrix);
	device->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + activeTextureUnit), &d3dMatrix);
	device->SetTextureStageState(activeTextureUnit, D3DTSS_TEXTURETRANSFORMFLAGS, gdTexMatFlags & 3);
}

void GLStateManager::BindTexture(uint32_t textureId)
{
	SetTexture(textureId, activeTextureUnit);
}

void GLStateManager::SetTexture(uint32_t textureId, uint32_t texUnit)
{
	if (texUnit < MAX_TEXTURE_UNITS) {
		textureHandles[texUnit] = textureId;
	}
}

void GLStateManager::SetTextureImmediately(uint32_t textureId)
{
	SetTexture(textureId, activeTextureUnit);
}

intptr_t GLStateManager::GetTexture(uint32_t texUnit)
{
	if (texUnit >= MAX_TEXTURE_UNITS) {
		return 0;
	}

	return textureHandles[texUnit];
}

uint32_t GLStateManager::GetActiveTextureUnit() const
{
	return activeTextureUnit;
}
