/*
 *  SCGL - a free OpenGL driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#include "cGDriver.h"
#include "D3D11Conversions.h"
#include "Diagnostics.h"
#include "GLSupport.h"
#include "VertexFormatUtils.h"

#include <cstring>

cIGZGBufferRegionExtension::~cIGZGBufferRegionExtension() { }
cIGZGDriverVertexBufferExtension::~cIGZGDriverVertexBufferExtension() { }

/*static_assert(sizeof(sGDMode) == 56U);
static_assert(offsetof(sGDMode, fullscreen) == 0x20);
static_assert(offsetof(sGDMode, is3DAccelerated) == 0x22);
static_assert(offsetof(sGDMode, _unknownFuncPtr) == 0x34);*/

namespace nSCGL
{
	static GLenum fogParamTypeMap[] = { GL_FOG_MODE, GL_FOG_COLOR, GL_FOG_DENSITY, GL_FOG_START, GL_FOG_END, GL_FOG_COORD_SRC };

	cGDriver::cGDriver() :
		lastError(DriverError::OK),
#ifndef NDEBUG
		dbgLastError(GL_NO_ERROR),
#endif
		currentVideoMode(-1),
		driverInfo("Maxis 3D GDriver\nOpenGL\n3.0\n"),
		videoModeCount(0),
		refCount(0),
		windowWidth(0),
		windowHeight(0),
		viewportX(0),
		viewportY(0),
		viewportWidth(0),
		viewportHeight(0),
		bufferRegionFlags(0),
		bufferRegions(),
		supportedExtensions(),
		windowHandle(nullptr),
		dynamicVertexBufferCapacity(0),
		dynamicIndexBufferCapacity(0),
		interleavedFormat(UINT_MAX),
		interleavedStride(0),
		interleavedPointer(nullptr),
		activeMatrixMode(0),
		matrices{},
		nextTextureId(1),
		boundTextures{},
		activeTextureStage(0),
		textureStageEnabled{},
		pixelStoreRowLength(0),
		enabledCapabilities{},
		colorWriteEnabled(true),
		depthFunction(1),
		depthWriteEnabled(true),
		stencilFunction(7),
		stencilReference(0),
		stencilReadMask(0xff),
		stencilWriteMask(0xff),
		stencilFailOperation(0),
		stencilDepthFailOperation(0),
		stencilPassOperation(0),
		sourceBlend(1),
		destinationBlend(0),
		alphaFunction(7),
		alphaReference(0.0f),
		shadeModel(1),
		colorMultipliers{ 1.0f, 1.0f, 1.0f, 1.0f },
		ambientVertexColors(false),
		diffuseVertexColors(false),
		polygonOffset(0),
		scissorEnabled(false),
		lightingEnabled(true),
		lightsEnabled{ true },
		globalAmbient{ 0.0f, 0.0f, 0.0f, 1.0f },
		lightAmbient{ 0.0f, 0.0f, 0.0f, 1.0f },
		lightDiffuse{ 1.0f, 1.0f, 1.0f, 1.0f },
		lightSpecular{ 1.0f, 1.0f, 1.0f, 1.0f },
		lightDirection{ 1.0f, 1.0f, 0.0f, 0.0f },
		materialAmbient{ 0.0f, 0.0f, 0.0f, 1.0f },
		materialDiffuse{ 1.0f, 1.0f, 1.0f, 1.0f },
		materialSpecular{ 0.0f, 0.0f, 0.0f, 1.0f },
		materialEmission{ 0.0f, 0.0f, 0.0f, 1.0f },
		materialShininess(0.0f),
		featureLevel(D3D_FEATURE_LEVEL_10_0),
		clearColor{ 0.0f, 0.0f, 0.0f, 0.0f },
		clearDepth(1.0f),
		clearStencil(0)
	{
		for (float* matrix : matrices) {
			matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
		}
		textureStageEnabled[0] = true;
		for (TextureStageState& stage : textureStages) {
			stage.matrix[0] = stage.matrix[5] = stage.matrix[10] = stage.matrix[15] = 1.0f;
		}
	}

	cGDriver::~cGDriver() {
		DestroyD3D11Context();
	}

	uint32_t cGDriver::MakeVertexFormat(uint32_t, intptr_t gdElementTypePtr) {
		Log(LogCategory::Unsupported, "custom vertex-format construction requested (element pointer %p)", reinterpret_cast<void*>(gdElementTypePtr));
		return UINT_MAX;
	}

	uint32_t cGDriver::MakeVertexFormat(uint32_t gdVertexFormat) {
		return RZMakeVertexFormat(gdVertexFormat);
	}

	uint32_t cGDriver::VertexFormatStride(uint32_t gdVertexFormat) {
		return RZVertexFormatStride(gdVertexFormat);
	}

	uint32_t cGDriver::VertexFormatElementOffset(uint32_t gdVertexFormat, uint32_t gdElementType, uint32_t count) {
		return RZVertexFormatElementOffset(gdVertexFormat, gdElementType, count);
	}

	uint32_t cGDriver::VertexFormatNumElements(uint32_t gdVertexFormat, uint32_t gdElementType) {
		return RZVertexFormatNumElements(gdVertexFormat, gdElementType);
	}

	void cGDriver::Clear(GLbitfield mask) {
		if (!d3dContext) {
			return;
		}

		if (ClearsColor(mask) && renderTargetView) {
			d3dContext->ClearRenderTargetView(renderTargetView.Get(), clearColor);
		}

		UINT const depthStencilFlags = D3D11DepthStencilClearFlags(mask);
		if (depthStencilFlags != 0 && depthStencilView) {
			d3dContext->ClearDepthStencilView(depthStencilView.Get(), depthStencilFlags, clearDepth, clearStencil);
		}
	}

	void cGDriver::ClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
		clearColor[0] = red;
		clearColor[1] = green;
		clearColor[2] = blue;
		clearColor[3] = alpha;
	}

	void cGDriver::ClearDepth(GLclampd depth) {
		clearDepth = static_cast<float>(depth < 0.0 ? 0.0 : (depth > 1.0 ? 1.0 : depth));
	}

	void cGDriver::ClearStencil(GLint s) {
		clearStencil = static_cast<uint8_t>(s);
	}

	void cGDriver::ColorMask(bool flag) {
		colorWriteEnabled = flag;
	}

	void cGDriver::DepthFunc(GLenum gdFunc) {
		if (D3D11Comparison(gdFunc) == 0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		depthFunction = static_cast<uint8_t>(gdFunc);
	}

	void cGDriver::DepthMask(bool flag) {
		depthWriteEnabled = flag;
	}

	void cGDriver::StencilFunc(GLenum gdFunc, GLint ref, GLuint mask) {
		if (D3D11Comparison(gdFunc) == 0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		stencilFunction = static_cast<uint8_t>(gdFunc);
		stencilReference = ref;
		stencilReadMask = static_cast<uint8_t>(mask);
	}

	void cGDriver::StencilMask(GLuint mask) {
		stencilWriteMask = static_cast<uint8_t>(mask);
	}

	void cGDriver::StencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
		if (D3D11StencilOperation(fail) == 0 || D3D11StencilOperation(zfail) == 0 ||
			D3D11StencilOperation(zpass) == 0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		stencilFailOperation = static_cast<uint8_t>(fail);
		stencilDepthFailOperation = static_cast<uint8_t>(zfail);
		stencilPassOperation = static_cast<uint8_t>(zpass);
	}

	void cGDriver::BlendFunc(GLenum sfactor, GLenum dfactor) {
		if (D3D11Blend(sfactor) == 0 || D3D11Blend(dfactor) == 0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		sourceBlend = static_cast<uint8_t>(sfactor);
		destinationBlend = static_cast<uint8_t>(dfactor);
	}

	void cGDriver::AlphaFunc(GLenum func, GLclampf ref) {
		if (D3D11Comparison(func) == 0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		alphaFunction = static_cast<uint8_t>(func);
		alphaReference = ref;
	}

	void cGDriver::ShadeModel(GLenum mode) {
		if (mode > 1) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		shadeModel = static_cast<uint8_t>(mode);
	}

	void cGDriver::Fog(uint32_t gdFogParamType, uint32_t gdFogParam) {
		Log(LogCategory::Unsupported, "fog integer state %u=%u is not translated yet", gdFogParamType, gdFogParam);
	}

	void cGDriver::Fog(uint32_t gdFogParamType, GLfloat const* params) {
		Log(LogCategory::Unsupported, "fog vector state %u (%p) is not translated yet", gdFogParamType, params);
	}

	void cGDriver::ColorMultiplier(float r, float g, float b) {
		colorMultipliers[0] = r;
		colorMultipliers[1] = g;
		colorMultipliers[2] = b;
	}

	void cGDriver::AlphaMultiplier(float a) {
		colorMultipliers[3] = a;
	}

	void cGDriver::EnableVertexColors(bool ambient, bool diffuse) {
		ambientVertexColors = ambient;
		diffuseVertexColors = diffuse;
	}

	void cGDriver::MatrixMode(GLenum mode) {
		if (mode < 2) {
			activeMatrixMode = static_cast<uint8_t>(mode);
		}
		else {
			SetLastError(DriverError::INVALID_ENUM);
		}
	}

	void cGDriver::LoadMatrix(GLfloat const* m) {
		if (m != nullptr) {
			memcpy(matrices[activeMatrixMode], m, sizeof(matrices[activeMatrixMode]));
		}
	}

	void cGDriver::LoadIdentity(void) {
		memset(matrices[activeMatrixMode], 0, sizeof(matrices[activeMatrixMode]));
		matrices[activeMatrixMode][0] = matrices[activeMatrixMode][5] =
			matrices[activeMatrixMode][10] = matrices[activeMatrixMode][15] = 1.0f;
	}

	void cGDriver::Enable(GLenum gdCap) {
		if (gdCap >= kGDNumCapabilities || gdCap == kGDCapability_Unused0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		if (gdCap == kGDCapability_Texture2D) {
			textureStageEnabled[activeTextureStage] = true;
		}
		else {
			enabledCapabilities[gdCap] = true;
		}
	}

	void cGDriver::Disable(GLenum gdCap) {
		if (gdCap >= kGDNumCapabilities || gdCap == kGDCapability_Unused0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		if (gdCap == kGDCapability_Texture2D) {
			textureStageEnabled[activeTextureStage] = false;
		}
		else {
			enabledCapabilities[gdCap] = false;
		}
	}

	bool cGDriver::IsEnabled(GLenum gdCap) {
		if (gdCap >= kGDNumCapabilities || gdCap == kGDCapability_Unused0) {
			SetLastError(DriverError::INVALID_ENUM);
			return false;
		}
		return gdCap == kGDCapability_Texture2D
			? textureStageEnabled[activeTextureStage]
			: enabledCapabilities[gdCap];
	}

	void cGDriver::GetBoolean(GLenum pname, bool* params) {
		if (pname != 0 || params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		*params = pixelStoreRowLength != 0;
	}

	void cGDriver::GetInteger(GLenum pname, GLint* params) {
		if (pname != 0 || params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		*params = static_cast<int32_t>(pixelStoreRowLength);
	}

	void cGDriver::GetFloat(GLenum pname, GLfloat* params) {
		if (pname != 0 || params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		*params = static_cast<float>(pixelStoreRowLength);
	}

	void cGDriver::PolygonOffset(int32_t offset) {
		polygonOffset = offset;
	}

	void cGDriver::BitBlt(
		int32_t destLeft,
		int32_t destTop,
		int32_t unknownWidth1,
		int32_t unknownHeight1,
		uint32_t gdTexFormat,
		uint32_t gdType,
		void const* unknownBuffer1,
		bool unknown5,
		void const* unknownBuffer2)
	{
		uint8_t const* unknownUintBuffer1 = reinterpret_cast<uint8_t const*>(unknownBuffer1);
		uint8_t const* unknownUintBuffer2 = reinterpret_cast<uint8_t const*>(unknownBuffer2);

		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cGDriver::StretchBlt(
		int32_t destLeft,
		int32_t destTop,
		int32_t unknownWidth1,
		int32_t unknownHeight1,
		int32_t unknownWidth2,
		int32_t unknownHeight2,
		uint32_t gdTexFormat,
		uint32_t gdType,
		void const* unknownBuffer1,
		bool unknownBool,
		void const* unknownBuffer2)
	{
		uint8_t const* unknownUintBuffer1 = reinterpret_cast<uint8_t const*>(unknownBuffer1);
		uint8_t const* unknownUintBuffer2 = reinterpret_cast<uint8_t const*>(unknownBuffer2);

		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cGDriver::BitBltAlpha(
		int32_t unknown0,
		int32_t unknown1,
		int32_t unknown2,
		int32_t unknown3,
		uint32_t gdTexFormat,
		uint32_t gdType,
		void const* unknownBuffer1,
		bool unknown5,
		void const* unknownBuffer2,
		uint32_t unknown7)
	{
		uint8_t const* unknownUintBuffer1 = reinterpret_cast<uint8_t const*>(unknownBuffer1);
		uint8_t const* unknownUintBuffer2 = reinterpret_cast<uint8_t const*>(unknownBuffer2);

		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cGDriver::StretchBltAlpha(
		int32_t destLeft,
		int32_t destTop,
		int32_t unknownWidth1,
		int32_t unknownHeight1,
		int32_t unknownWidth2,
		int32_t unknownHeight2,
		uint32_t gdTexFormat,
		uint32_t gdType,
		void const* unknownBuffer1,
		bool unknown7,
		void const* unknownBuffer2,
		uint32_t unknown9)
	{
		uint8_t const* unknownUintBuffer1 = reinterpret_cast<uint8_t const*>(unknownBuffer1);
		uint8_t const* unknownUintBuffer2 = reinterpret_cast<uint8_t const*>(unknownBuffer2);

		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cGDriver::BitBltAlphaModulate(
		int32_t unknown0,
		int32_t unknown1,
		int32_t unknown2,
		uint32_t gdTexFormat,
		uint32_t gdType,
		void const* unknownBuffer1,
		bool unknown4,
		void const* unknownBuffer2,
		uint32_t unknown6)
	{
		uint8_t const* unknownUintBuffer1 = reinterpret_cast<uint8_t const*>(unknownBuffer1);
		uint8_t const* unknownUintBuffer2 = reinterpret_cast<uint8_t const*>(unknownBuffer2);

		SetLastError(DriverError::NOT_SUPPORTED);
	}

	void cGDriver::StretchBltAlphaModulate(
		int32_t destLeft,
		int32_t destTop,
		int32_t unknownWidth1,
		int32_t unknownHeight1,
		int32_t unknownWidth2,
		int32_t unknownHeight2,
		uint32_t gdTexFormat,
		uint32_t gdType,
		void const* unknownBuffer1,
		bool unknown7,
		void const* unknownBuffer2,
		uint32_t unknown9)
	{
		uint8_t const* unknownUintBuffer1 = reinterpret_cast<uint8_t const*>(unknownBuffer1);
		uint8_t const* unknownUintBuffer2 = reinterpret_cast<uint8_t const*>(unknownBuffer2);

		SetLastError(DriverError::NOT_SUPPORTED);
	}

	bool cGDriver::Punt(uint32_t, void*) {
		return false;
	}
}
