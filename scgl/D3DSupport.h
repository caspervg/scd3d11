/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *
 *  Direct3D 9 backend support.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>

constexpr size_t MAX_TEXTURE_UNITS = 2;

extern FILE* gLogFile;

#ifdef NDEBUG
#define NOTIMPL()
#define SIZE_CHECK(...)
#define SIZE_CHECK_RETVAL(...)
#else
inline void SCGLDebugLogNotImpl(char const* functionName)
{
	if (gLogFile == nullptr) {
		fopen_s(&gLogFile, "C:\\temp\\cGDriver.notimpl.log", "w");
	}

	if (gLogFile != nullptr) {
		fprintf(gLogFile, "%s\n", functionName);
		fflush(gLogFile);
	}
}

#define NOTIMPL() SCGLDebugLogNotImpl(__FUNCSIG__)
#define UNEXPECTED NOTIMPL
#define SIZE_CHECK(param, map) if ((param) >= sizeof(map) / sizeof((map)[0])) { UNEXPECTED(); return; }
#define SIZE_CHECK_RETVAL(param, map, ret) if ((param) >= sizeof(map) / sizeof((map)[0])) { UNEXPECTED(); return ret; }
#endif

struct D3DTextureHandle
{
	IDirect3DTexture9* texture;
	D3DFORMAT format;
	uint32_t width;
	uint32_t height;
	uint32_t levels;

	D3DTextureHandle() :
		texture(nullptr),
		format(D3DFMT_UNKNOWN),
		width(0),
		height(0),
		levels(0)
	{
	}
};

struct D3DBufferRegion
{
	IDirect3DSurface9* surface;
	uint32_t type;

	D3DBufferRegion() :
		surface(nullptr),
		type(0)
	{
	}
};
