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
#include "VertexFormatUtils.h"
#include <cstring>

FILE* gLogFile = nullptr;
cIGZGBufferRegionExtension::~cIGZGBufferRegionExtension() { }
cIGZGDriverVertexBufferExtension::~cIGZGDriverVertexBufferExtension() { }

/*static_assert(sizeof(sGDMode) == 56U);
static_assert(offsetof(sGDMode, fullscreen) == 0x20);
static_assert(offsetof(sGDMode, is3DAccelerated) == 0x22);
static_assert(offsetof(sGDMode, _unknownFuncPtr) == 0x34);*/

namespace nSCGL
{
	static D3DRENDERSTATETYPE fogParamTypeMap[] = { D3DRS_FOGVERTEXMODE, D3DRS_FOGCOLOR, D3DRS_FOGDENSITY, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGTABLEMODE };

	cGDriver::cGDriver() :
		lastError(DriverError::OK),
#ifndef NDEBUG
		dbgLastError(D3D_OK),
#endif
		currentVideoMode(-1),
		driverInfo("Maxis 3D GDriver\nDirect3D\n9.0c\n"),
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
		supportedFeatures(),
		windowHandle(nullptr),
		d3d(nullptr),
		d3dDevice(nullptr),
		presentParams(),
		deviceCaps(),
		clearColor(D3DCOLOR_ARGB(0, 0, 0, 0)),
		clearDepth(1.0f),
		clearStencil(0)
	{
	}

	cGDriver::~cGDriver() {
	}

	void cGDriver::DrawArrays(uint32_t gdMode, int32_t first, int32_t count) {
		state.DrawArrays(gdMode, first, count);
	}

	void cGDriver::DrawElements(uint32_t gdMode, int32_t count, uint32_t gdType, void const* indices) {
		state.DrawElements(gdMode, count, gdType, indices);
	}

	void cGDriver::InterleavedArrays(uint32_t format, int32_t stride, void const* pointer) {
		if (stride == 0) {
			stride = VertexFormatStride(format);
		}

		state.InterleavedArrays(format, stride, pointer);
	}

	uint32_t cGDriver::MakeVertexFormat(uint32_t, intptr_t gdElementTypePtr) {
		NOTIMPL();
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

	void cGDriver::Clear(uint32_t mask) {
		DWORD d3dMask = 0;
		d3dMask |= (mask & 0x1000) ? D3DCLEAR_ZBUFFER : 0;
		d3dMask |= (mask & 0x2000) ? D3DCLEAR_STENCIL : 0;
		d3dMask |= (mask & 0x4000) ? D3DCLEAR_TARGET : 0;

		if (d3dDevice != nullptr && d3dMask != 0) {
			d3dDevice->Clear(0, nullptr, d3dMask, clearColor, clearDepth, clearStencil);
		}
	}

	void cGDriver::ClearColor(float red, float green, float blue, float alpha) {
		clearColor = D3DCOLOR_COLORVALUE(red, green, blue, alpha);
	}

	void cGDriver::ClearDepth(double depth) {
		clearDepth = static_cast<float>(depth);
	}

	void cGDriver::ClearStencil(int32_t s) {
		clearStencil = static_cast<uint32_t>(s);
	}

	void cGDriver::ColorMask(bool flag) {
		state.ColorMask(flag);
	}

	void cGDriver::DepthFunc(uint32_t gdFunc) {
		state.DepthFunc(gdFunc);
	}

	void cGDriver::DepthMask(bool flag) {
		state.DepthMask(flag);
	}

	void cGDriver::StencilFunc(uint32_t gdFunc, int32_t ref, uint32_t mask) {
		state.StencilFunc(gdFunc, ref, mask);
	}

	void cGDriver::StencilMask(uint32_t mask) {
		state.StencilMask(mask);
	}

	void cGDriver::StencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass) {
		state.StencilOp(fail, zfail, zpass);
	}

	void cGDriver::BlendFunc(uint32_t sfactor, uint32_t dfactor) {
		state.BlendFunc(sfactor, dfactor);
	}

	void cGDriver::AlphaFunc(uint32_t func, float ref) {
		state.AlphaFunc(func, ref);
	}

	void cGDriver::ShadeModel(uint32_t mode) {
		state.ShadeModel(mode);
	}

	void cGDriver::Fog(uint32_t gdFogParamType, uint32_t gdFogParam) {
		static DWORD fogParamMap[] = { D3DFOG_EXP, D3DFOG_EXP2, D3DFOG_LINEAR, D3DFOG_NONE, D3DFOG_NONE };
		SIZE_CHECK(gdFogParamType, fogParamTypeMap);
		SIZE_CHECK(gdFogParam, fogParamMap);

		if (d3dDevice != nullptr) {
			d3dDevice->SetRenderState(fogParamTypeMap[gdFogParamType], fogParamMap[gdFogParam]);
		}
	}

	void cGDriver::Fog(uint32_t gdFogParamType, float const* params) {
		SIZE_CHECK(gdFogParamType, fogParamTypeMap);
		if (d3dDevice == nullptr || params == nullptr) {
			return;
		}

		if (gdFogParamType == 1) {
			d3dDevice->SetRenderState(D3DRS_FOGCOLOR, D3DCOLOR_COLORVALUE(params[0], params[1], params[2], params[3]));
		}
		else {
			DWORD value;
			memcpy(&value, params, sizeof(value));
			d3dDevice->SetRenderState(fogParamTypeMap[gdFogParamType], value);
		}
	}

	void cGDriver::ColorMultiplier(float r, float g, float b) {
		state.ColorMultiplier(r, g, b);
	}

	void cGDriver::AlphaMultiplier(float a) {
		state.AlphaMultiplier(a);
	}

	void cGDriver::EnableVertexColors(bool ambient, bool diffuse) {
		state.EnableVertexColors(ambient, diffuse);
	}

	void cGDriver::MatrixMode(uint32_t mode) {
		state.MatrixMode(mode);
	}

	void cGDriver::LoadMatrix(float const* m) {
		state.LoadMatrix(m);
	}

	void cGDriver::LoadIdentity(void) {
		state.LoadIdentity();
	}

	void cGDriver::Enable(uint32_t gdCap) {
		state.Enable(gdCap);
	}

	void cGDriver::Disable(uint32_t gdCap) {
		state.Disable(gdCap);
	}

	bool cGDriver::IsEnabled(uint32_t gdCap) {
		return state.IsEnabled(gdCap);
	}

	void cGDriver::GetBoolean(uint32_t pname, bool* params) {
#ifndef NDEBUG
		if (pname != 0) {
			UNEXPECTED();
			return;
		}
#endif

		*params = false;
	}

	void cGDriver::GetInteger(uint32_t pname, int32_t* params) {
#ifndef NDEBUG
		if (pname != 0) {
			UNEXPECTED();
			return;
		}
#endif

		*params = 0;
	}

	void cGDriver::GetFloat(uint32_t pname, float* params) {
#ifndef NDEBUG
		if (pname != 0) {
			UNEXPECTED();
			return;
		}
#endif

		*params = 0.0f;
	}

	void cGDriver::PolygonOffset(int32_t offset) {
		float fOffset = (float)offset;
		if (offset < 0) {
			fOffset += 4294967296.0f;
		}

		if (d3dDevice != nullptr) {
			DWORD value;
			memcpy(&value, &fOffset, sizeof(value));
			d3dDevice->SetRenderState(D3DRS_DEPTHBIAS, value);
		}
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
