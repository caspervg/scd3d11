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
#include "Diagnostics.h"

namespace nSCD3D11
{
	bool cGDriver::QueryInterface(uint32_t riid, void** ppvObj) {
		if (ppvObj == nullptr) {
			return false;
		}
		*ppvObj = nullptr;
		switch (riid)
		{
		case GZIID_cIGZUnknown:
		case GZIID_cIGZGDriver:
			*ppvObj = static_cast<cIGZGDriver*>(this);
			break;

		case GZIID_cIGZGBufferRegionExtension:
			*ppvObj = static_cast<cIGZGBufferRegionExtension*>(this);
			break;

		case GZIID_cIGZGDriverLightingExtension:
			*ppvObj = static_cast<cIGZGDriverLightingExtension*>(this);
			break;

		case GZIID_cIGZGDriverVertexBufferExtension:
			*ppvObj = static_cast<cIGZGDriverVertexBufferExtension*>(this);
			break;

		case GZIID_cIGZGSnapshotExtension:
			*ppvObj = static_cast<cIGZGSnapshotExtension*>(this);
			break;

		default:
			//sprintf_s(buf, "%x", riid);
			//MessageBoxA(NULL, buf, "Unknown interface ID in GDriver", MB_ICONERROR);
			Log(LogCategory::Grid, "QueryInterface(0x%08X) -> unsupported", riid);
			return false;
		}

		AddRef();
		return true;
	}

	uint32_t cGDriver::AddRef(void) {
		return cRZRefCount::AddRef();
	}

	uint32_t cGDriver::Release(void) {
		return cRZRefCount::Release();
	}

	bool cGDriver::FinalRelease(void) {
		return true;
	}
}
