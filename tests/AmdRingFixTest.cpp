/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "AmdRingFix.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace nSCD3D11::AmdRingFix;
using Microsoft::WRL::ComPtr;

namespace {
	// From amdxx32.dll 32.0.32015.7029, as the file has them (unrelocated, preferred base 0x10000000).
	// The replay loop from its opcode read to the handler table load.
	constexpr uint8_t kReplayLoop[] = {
		0x8b, 0x8e, 0xc0, 0x01, 0x00, 0x00, 0x8d, 0x42, 0x04, 0x8b, 0x39, 0x83, 0xc1, 0x04, 0x89, 0x8e, 0xc0, 0x01,
		0x00, 0x00, 0x89, 0x86, 0x00, 0x02, 0x00, 0x00, 0x81, 0xff, 0xad, 0xde, 0xff, 0xff, 0x75, 0x06, 0xc6, 0x45,
		0xf3, 0x00, 0xeb, 0x1b, 0x81, 0xff, 0xc5, 0x00, 0x00, 0x00, 0x73, 0x13, 0x8b, 0x3c, 0xbd, 0xe8, 0x49, 0x97,
		0x11, 0x8b, 0xcf, 0xff, 0x15, 0x80, 0xf4, 0xef, 0x10, 0x8b, 0xce, 0xff, 0xd7,
	};
	// Opcode 1's handler, which wraps.
	constexpr uint8_t kOpcode1[] = {
		0x53, 0x8b, 0xd9, 0x56, 0x57, 0x8b, 0x8b, 0xc0, 0x01, 0x00, 0x00, 0x8b, 0xc1, 0x8b, 0x93, 0x00, 0x02, 0x00,
		0x00, 0x2b, 0xc1, 0x03, 0xd0, 0x89, 0x8b, 0xc0, 0x01, 0x00, 0x00, 0x89, 0x93, 0x00, 0x02, 0x00, 0x00, 0x3b,
		0x8b, 0x40, 0x02, 0x00, 0x00, 0x72, 0x24, 0x8b, 0x49, 0x04, 0x8d, 0x42, 0x08, 0x89, 0x83, 0x00, 0x02, 0x00,
		0x00, 0x8b, 0x84, 0x8b, 0x88, 0x00, 0x00, 0x00, 0x89, 0x83, 0xc0, 0x01, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10,
		0x00, 0x89, 0x83, 0x40, 0x02, 0x00, 0x00, 0x8b, 0x7b, 0x40, 0x8b, 0x07, 0x8b, 0x70, 0x0c, 0x8b, 0xce, 0xff,
		0x15, 0x80, 0xf4, 0xef, 0x10, 0x8b,
	};
	// Opcode 0x3E's handler, which does not.
	constexpr uint8_t kOpcode3E[] = {
		0x56, 0x57, 0x8b, 0x79, 0x40, 0x8b, 0x07, 0x8b, 0xb0, 0xa8, 0x01, 0x00, 0x00, 0x8b,
		0xce, 0xff, 0x15, 0x80, 0xf4, 0xef, 0x10, 0x8b, 0xcf, 0xff, 0xd6, 0x5f, 0x5e, 0xc3,
	};

	// A fake ring context: the 0x300 bytes the worker uses.
	struct RingContext {
		alignas(4) uint8_t bytes[0x300]{};

		void Put(size_t offset, void const *value) {
			uint32_t const address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value));
			std::memcpy(bytes + offset, &address, sizeof(address));
		}
		void PutNumber(size_t offset, uint32_t value) {
			std::memcpy(bytes + offset, &value, sizeof(value));
		}
		uintptr_t Get(size_t offset) const {
			uint32_t value = 0;
			std::memcpy(&value, bytes + offset, sizeof(value));
			return value;
		}
	};

	uintptr_t Address(void const *pointer, uintptr_t offset = 0) {
		return reinterpret_cast<uintptr_t>(pointer) + offset;
	}

	void TestPatterns() {
		std::vector<uint8_t> code(0x100, 0xCC);
		std::memcpy(code.data() + 0x40, kReplayLoop, sizeof(kReplayLoop));
		auto const dispatch = FindDispatch(code.data(), code.size());
		assert(dispatch && dispatch->table == 0x119749E8 && dispatch->opcodes == 0xC5);
		// Two replay loops: ambiguous, so no match.
		code.resize(0x200, 0xCC);
		std::memcpy(code.data() + 0x140, kReplayLoop, sizeof(kReplayLoop));
		assert(!FindDispatch(code.data(), code.size()));
		// A loop with other context offsets is not the layout the fix knows.
		std::vector<uint8_t> moved(kReplayLoop, kReplayLoop + sizeof(kReplayLoop));
		moved[2] = 0xC4;
		assert(!FindDispatch(moved.data(), moved.size()));

		assert(IsUnwrappedHandler(kOpcode3E, sizeof(kOpcode3E)));
		assert(!HasWrapStep(kOpcode3E, sizeof(kOpcode3E)));
		assert(HasWrapStep(kOpcode1, sizeof(kOpcode1)));
		assert(!IsUnwrappedHandler(kOpcode1, sizeof(kOpcode1)));
		// A driver whose 0x3E handler does anything more is left alone.
		std::vector<uint8_t> changed(kOpcode3E, kOpcode3E + sizeof(kOpcode3E));
		changed.back() = 0x90;
		assert(!IsUnwrappedHandler(changed.data(), changed.size()));
		assert(!IsUnwrappedHandler(kOpcode3E, sizeof(kOpcode3E) - 1));
	}

	void TestWrap() {
		RingContext context;
		alignas(4) uint32_t trailer[2] = {0x7890ABCD, 1};
		alignas(4) uint8_t segment0[16]{};
		alignas(4) uint8_t segment1[16]{};
		context.Put(0x88, segment0);
		context.Put(0x8C, segment1);
		context.PutNumber(0x200, 0x30);

		// Before the end: nothing changes.
		context.Put(0x1C0, trailer);
		context.Put(0x240, trailer + 1);
		assert(!WrapAtSegmentEnd(context.bytes));
		assert(context.Get(0x1C0) == Address(trailer) && context.Get(0x200) == 0x30);

		// At the end: follow the trailer to segment 1, and count its 8 bytes.
		context.Put(0x240, trailer);
		assert(WrapAtSegmentEnd(context.bytes));
		assert(context.Get(0x1C0) == Address(segment1));
		assert(context.Get(0x240) == Address(segment1, 0x100000));
		assert(context.Get(0x200) == 0x38);
	}

	// Stands in for the replay target's method that AMD's 0x3E handler calls (vtable offset 0x1A8, thiscall).
	uint32_t gTargetCalls = 0;
	void __fastcall TargetMethod(void *) {
		++gTargetCalls;
	}

	// Calls the patched 0x3E slot the way AMD's worker does: thiscall, ring context in ecx. That runs the
	// replacement, then AMD's own handler code, which calls the method on the target at [context+0x40].
	void ReplayThroughPatchedSlot() {
		void *vtable[0x200 / sizeof(void *)]{};
		vtable[0x1A8 / sizeof(void *)] = reinterpret_cast<void *>(&TargetMethod);
		void **target = vtable;
		RingContext context;
		alignas(4) uint32_t trailer[2] = {0x7890ABCD, 0};
		alignas(4) uint8_t segment0[16]{};
		context.Put(0x40, &target);
		context.Put(0x88, segment0);
		context.PutNumber(0x200, 0x30);

		auto const slot = reinterpret_cast<void(__fastcall *)(uint8_t *)>(PatchedHandlerSlot()[0x3E]);

		// A 0x3E that ended exactly on the segment end: the replacement wraps, AMD's handler still runs.
		context.Put(0x1C0, trailer);
		context.Put(0x240, trailer);
		slot(context.bytes);
		assert(gTargetCalls == 1);
		assert(context.Get(0x1C0) == Address(segment0));
		assert(context.Get(0x240) == Address(segment0, 0x100000));
		assert(context.Get(0x200) == 0x38);

		// Mid-segment: no wrap.
		context.Put(0x1C0, segment0 + 4);
		slot(context.bytes);
		assert(gTargetCalls == 2);
		assert(context.Get(0x1C0) == Address(segment0, 4) && context.Get(0x200) == 0x38);

		Counts const counts = GetCounts();
		assert(counts.calls == 2 && counts.wraps == 1);
	}

	// With AMD's driver loaded: patch it for real, run the patched slot, and check the device still works.
	int TestDriver() {
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
		                             &device, nullptr, &context))) {
			std::printf("no hardware device, driver test skipped\n");
			return 0;
		}
		Result const result = Apply();
		if (result == Result::NotAmd) {
			std::printf("not an AMD driver, driver test skipped\n");
			return 0;
		}
		std::printf("amdxx32.dll: %s\n", result == Result::Patched ? "patched" : "not patched (layout differs)");
		if (result != Result::Patched) return 0;
		assert(Apply() == Result::AlreadyPatched);
		ReplayThroughPatchedSlot();

		D3D11_TEXTURE2D_DESC description{};
		description.Width = 256;
		description.Height = 256;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		ComPtr<ID3D11Texture2D> target;
		ComPtr<ID3D11Texture2D> copy;
		assert(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &target)));
		assert(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &copy)));
		ComPtr<ID3D11RenderTargetView> view;
		assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &view)));
		std::vector<uint32_t> pixels(64 * 64, 0xFF336699);
		D3D11_BOX box{0, 0, 0, 64, 64, 1};
		// Enough commands for the ring to wrap many times.
		for (uint32_t round = 0; round < 200000; ++round) {
			float const color[4] = {static_cast<float>(round % 256) / 255.0f, 0.5f, 0.25f, 1.0f};
			context->ClearRenderTargetView(view.Get(), color);
			context->UpdateSubresource(target.Get(), 0, &box, pixels.data(), 64 * 4, 0);
			context->CopyResource(copy.Get(), target.Get());
			if (round % 64 == 0) context->Flush();
		}

		// The device still works: read a pixel back.
		description.Usage = D3D11_USAGE_STAGING;
		description.BindFlags = 0;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> staging;
		assert(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &staging)));
		context->CopyResource(staging.Get(), copy.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		assert(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
		uint32_t const pixel = *static_cast<uint32_t const *>(mapped.pData);
		context->Unmap(staging.Get(), 0);
		assert(pixel == 0xFF336699);
		return 0;
	}
} // namespace

int main() {
	TestPatterns();
	TestWrap();
	return TestDriver();
}
