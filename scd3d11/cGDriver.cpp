/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
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

cIGZGBufferRegionExtension::~cIGZGBufferRegionExtension() {
}

cIGZGDriverVertexBufferExtension::~cIGZGDriverVertexBufferExtension() {
}

/*static_assert(sizeof(sGDMode) == 56U);
static_assert(offsetof(sGDMode, fullscreen) == 0x20);
static_assert(offsetof(sGDMode, is3DAccelerated) == 0x22);
static_assert(offsetof(sGDMode, _unknownFuncPtr) == 0x34);*/

namespace nSCD3D11 {
	cGDriver::cGDriver() : refCount(0),
	                       lastError(DriverError::OK),
#ifndef NDEBUG
	                       dbgLastError(GL_NO_ERROR),
#endif
	                       initialized(false),
	                       videoModeCount(0),
	                       currentVideoMode(-1),
	                       driverInfo("Maxis 3D GDriver\nDirect3D 11\n11.0\n"),
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
	                       windowProcedure(nullptr),
	                       showDriverWindow(false),
	                       recoveringDevice(false),
	                       deviceLost(false),
	                       deviceRecoveryFailures(0),
	                       nextDeviceRecovery(0),
	                       presentationMode(PresentationMode::Windowed),
	                       swapChainFlags(0),
	                       depthStencilFormat(DXGI_FORMAT_D24_UNORM_S8_UINT),
	                       depthRegionScratchValid(false),
	                       activeTransformBuffer(0),
	                       appliedTransformBuffer(nullptr),
	                       vertexBufferSegments{},
	                       indexBufferSegments{},
	                       activeVertexBufferSegment(0),
	                       activeIndexBufferSegment(0),
	                       dynamicVertexBufferOffset(0),
	                       dynamicIndexBufferOffset(0),
	                       appliedVertexBuffer(nullptr),
	                       appliedVertexBufferOffset(UINT32_MAX),
	                       vertexBufferCacheHits(0),
	                       vertexBufferCacheMisses(0),
	                       indexBufferCacheHits(0),
	                       indexBufferCacheMisses(0),
	                       geometryPipelineBound(false),
	                       appliedPixelShader(nullptr),
	                       textureBindingsValid(false),
	                       appliedTopology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),
	                       appliedTextureViews{},
	                       appliedSamplers{},
	                       interleavedFormat(UINT_MAX),
	                       interleavedStride(0),
	                       interleavedPointer(nullptr),
	                       activeMatrixMode(0),
	                       matrices{},
	                       normalMatrix{},
	                       normalMatrixDirty(true),
	                       constantsDirty(true),
	                       constantsFlagInputs(0),
	                       extensionVertexCursor(0),
	                       extensionVertexStart(0),
	                       extensionVertexGeneration(0),
	                       extensionVerticesLocked(false),
	                       nextTextureId(1),
	                       boundTextures{},
	                       activeTextureStage(0),
	                       textureStageEnabled{},
	                       pixelStoreRowLength(0),
	                       blitUsesSourceAlpha(false),
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
	                       colorMultipliers{1.0f, 1.0f, 1.0f, 1.0f},
	                       fogMode(0),
	                       fogSource(4),
	                       fogColor{0.0f, 0.0f, 0.0f, 0.0f},
	                       fogDensity(1.0f),
	                       fogStart(0.0f),
	                       fogEnd(1.0f),
	                       ambientVertexColors(false),
	                       diffuseVertexColors(false),
	                       polygonOffset(0),
	                       scissorEnabled(false),
	                       lightingEnabled(true),
	                       lightsEnabled{true},
	                       globalAmbient{0.0f, 0.0f, 0.0f, 1.0f},
	                       lightAmbient{},
	                       lightDiffuse{},
	                       lightSpecular{},
	                       lightPosition{},
	                       materialAmbient{0.0f, 0.0f, 0.0f, 1.0f},
	                       materialDiffuse{1.0f, 1.0f, 1.0f, 1.0f},
	                       materialSpecular{0.0f, 0.0f, 0.0f, 1.0f},
	                       materialEmission{0.0f, 0.0f, 0.0f, 1.0f},
	                       materialShininess(0.0f),
	                       appliedDepthStateKey(UINT64_MAX),
	                       appliedBlendStateKey(UINT64_MAX),
	                       appliedRasterizerStateKey(UINT64_MAX),
	                       appliedStencilReference(INT32_MIN),
	                       featureLevel(D3D_FEATURE_LEVEL_10_0),
	                       deviceGeneration(0),
	                       clearColor{0.0f, 0.0f, 0.0f, 0.0f},
	                       clearDepth(1.0f),
	                       clearStencil(0) {
		for (float *matrix: matrices) {
			matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
		}
		textureStageEnabled[0] = true;
		lightAmbient[0][3] = 1.0f;
		lightDiffuse[0][0] = lightDiffuse[0][1] = lightDiffuse[0][2] = lightDiffuse[0][3] = 1.0f;
		lightSpecular[0][0] = lightSpecular[0][1] = lightSpecular[0][2] = lightSpecular[0][3] = 1.0f;
		lightPosition[0][0] = lightPosition[0][1] = 1.0f;
		for (TextureStageState &stage: textureStages) {
			stage.matrix[0] = stage.matrix[5] = stage.matrix[10] = stage.matrix[15] = 1.0f;
		}
	}

	cGDriver::~cGDriver() {
		Shutdown();
		UninstallReShadeAddon();
	}

	uint32_t cGDriver::MakeVertexFormat(uint32_t, intptr_t gdElementTypePtr) {
		Log(LogCategory::Unsupported, "custom vertex-format construction requested (element pointer %p)",
		    reinterpret_cast<void *>(gdElementTypePtr));
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
			reshadeEffectsInBackBuffer = false;
		}

		UINT const depthStencilFlags = D3D11DepthStencilClearFlags(mask);
		if (depthStencilFlags != 0 && depthStencilView) {
			d3dContext->ClearDepthStencilView(depthStencilView.Get(), depthStencilFlags, clearDepth, clearStencil);
			depthRegionScratchValid = false;
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
		if (!IsValidBlendFunction(sfactor, dfactor)) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		sourceBlend = static_cast<uint8_t>(sfactor);
		destinationBlend = static_cast<uint8_t>(dfactor);
	}

	void cGDriver::AlphaFunc(GLenum func, GLclampf ref) {
		constantsDirty = true;
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
		constantsDirty = true;
		if (gdFogParamType == 0 && gdFogParam <= 2) {
			fogMode = static_cast<uint8_t>(gdFogParam);
			return;
		}
		if (gdFogParamType == 5 && (gdFogParam == 3 || gdFogParam == 4)) {
			fogSource = static_cast<uint8_t>(gdFogParam);
			if (gdFogParam == 3)
				Log(LogCategory::Unsupported,
				    "explicit vertex fog coordinates are not implemented; using eye-space depth");
			return;
		}
		SetLastError(DriverError::INVALID_ENUM);
	}

	void cGDriver::Fog(uint32_t gdFogParamType, GLfloat const *params) {
		constantsDirty = true;
		if (params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		switch (gdFogParamType) {
			case 1:
				memcpy(fogColor, params, sizeof(fogColor));
				break;
			case 2:
				if (*params < 0.0f) {
					SetLastError(DriverError::INVALID_VALUE);
					return;
				}
				fogDensity = *params;
				break;
			case 3:
				fogStart = *params;
				break;
			case 4:
				fogEnd = *params;
				break;
			default:
				SetLastError(DriverError::INVALID_ENUM);
				break;
		}
	}

	void cGDriver::ColorMultiplier(float r, float g, float b) {
		constantsDirty = true;
		colorMultipliers[0] = r;
		colorMultipliers[1] = g;
		colorMultipliers[2] = b;
	}

	void cGDriver::AlphaMultiplier(float a) {
		constantsDirty = true;
		colorMultipliers[3] = a;
	}

	void cGDriver::EnableVertexColors(bool ambient, bool diffuse) {
		constantsDirty = true;
		ambientVertexColors = ambient;
		diffuseVertexColors = diffuse;
	}

	void cGDriver::MatrixMode(GLenum mode) {
		if (mode < 2) {
			activeMatrixMode = static_cast<uint8_t>(mode);
		} else {
			SetLastError(DriverError::INVALID_ENUM);
		}
	}

	void cGDriver::LoadMatrix(GLfloat const *m) {
		constantsDirty = true;
		if (m != nullptr) {
			memcpy(matrices[activeMatrixMode], m, sizeof(matrices[activeMatrixMode]));
			if (activeMatrixMode == MODEL_VIEW) normalMatrixDirty = true;
		}
	}

	void cGDriver::LoadIdentity(void) {
		constantsDirty = true;
		if (activeMatrixMode == MODEL_VIEW) normalMatrixDirty = true;
		memset(matrices[activeMatrixMode], 0, sizeof(matrices[activeMatrixMode]));
		matrices[activeMatrixMode][0] = matrices[activeMatrixMode][5] =
		                                matrices[activeMatrixMode][10] = matrices[activeMatrixMode][15] = 1.0f;
	}

	void cGDriver::Enable(GLenum gdCap) {
		constantsDirty = true;
		if (gdCap >= kGDNumCapabilities || gdCap == kGDCapability_Unused0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		if (gdCap == kGDCapability_Texture2D) {
			textureStageEnabled[activeTextureStage] = true;
		} else {
			enabledCapabilities[gdCap] = true;
		}
	}

	void cGDriver::Disable(GLenum gdCap) {
		constantsDirty = true;
		if (gdCap >= kGDNumCapabilities || gdCap == kGDCapability_Unused0) {
			SetLastError(DriverError::INVALID_ENUM);
			return;
		}
		if (gdCap == kGDCapability_Texture2D) {
			textureStageEnabled[activeTextureStage] = false;
		} else {
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

	void cGDriver::GetBoolean(GLenum pname, bool *params) {
		if (pname != 0 || params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		*params = pixelStoreRowLength != 0;
	}

	void cGDriver::GetInteger(GLenum pname, GLint *params) {
		if (pname != 0 || params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		*params = static_cast<int32_t>(pixelStoreRowLength);
	}

	void cGDriver::GetFloat(GLenum pname, GLfloat *params) {
		if (pname != 0 || params == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		*params = static_cast<float>(pixelStoreRowLength);
	}

	void cGDriver::PolygonOffset(int32_t offset) {
		polygonOffset = offset;
	}
}
