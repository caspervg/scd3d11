/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Brackets SC4's draw calls by the game object they belong to, for the renderer on the other end of the
// stream (OwnerBegin and OwnerEnd records, wire/Wire.h).
//
// SimCity 4 Deluxe 1.1.641 only. cSC43DRender draws two kinds of things:
//
//   Grid drawables. DrawStaticView (0x007C7370) and DrawDynamicView (0x007C7830) call each model's
//   vtable +0x30, Draw(SC4DrawContext *, bool), __thiscall, popping 8. It returns 2 to be drawn again
//   in the same pass. The model's vtable +0x0C, GetOwner(), returns its occupant, or part of it.
//   View objects. Four loops call each object's vtable +0x0C, Update(SC4DrawContext *), popping 4.
//   They draw terrain, water, overlays (shadows, decals) and effects.
//
// The draw context's word at +0x50 is the pass (see DrawOwner::pass).
//
//   G1 0x007C76BB  FF 50 30 83 F8 02   static grid:   call [eax+30h] / cmp eax,2
//   G2 0x007C7CB1  FF 52 30 83 F8 02   dynamic grid:  call [edx+30h] / cmp eax,2
//   G3 0x007C7DA7  FF 52 30 47 3B FE   highlights:    call [edx+30h] / inc edi / cmp edi,esi
//   V1 0x007C3E3B  8B 36 50 FF 52 0C   pre-dynamic view objects:  mov esi,[esi] / push eax / call [edx+0Ch]
//   V2 0x007C3E7B  same               post-dynamic view objects
//   V3 0x007C3EBB  same               pre-static view objects
//   V4 0x007C3EFB  same               post-static view objects
//
// For cS3DModelInstance drawables (vtable 0x00ABD508, and 0x00ABD018 for the animated ones) a ModelInstance
// record follows the OwnerBegin. Every thunk of these classes starts with SUB ECX,[ECX-4], so the body's
// `this` is q = drawable - [drawable-4], minus 0x24 more for the animated class. From q:
//   q-0x78  cS3DResModel *, vtables 0x00ABD608 and 0x00ABD62C, its key at +0x10 type, +0x14 group, +0x18 instance
//   q-0x74  the transform: change count, part mask, 9 rotation floats, translation, scale (0x38 bytes)
//   q-0x24  20 bytes of draw attributes
//   q-0x0C  the animation frame, q-0x0A the properties
// These are plain loads of fields the draw itself reads.
//
// Plain network pieces (vtable 0x00AA7F10, Draw 0x0061A850) get a NetworkPiece record. From q:
//   q-0xE0  the quad: 4 vertices of 0x18 bytes (x, y, z, BGRA, u, v) in world space
//   q-0x80  the piece id, q-0x7C the under texture, q-0x78 rotation and flip, q-0x77 the variation
//   q-0x64  begin and q-0x60 end of the extra parts, 0x4C bytes each: a texture id and 3 vertices
//   q-0x40  the network flags: 0x2000000 and 0x4000000 draw the quad's two triangles, 0x1000000 picks the
//           other diagonal (indices 0,1,3 and 3,1,2 instead of 0,1,2 and 0,2,3)
// The draw context's word at +0x14 is the zoom.
//
// Lot cell textures (cSC4CellTextureModel, vtable 0x00AA2C30, Draw 0x005E0200) get a CellTexture record:
// q-0xB8 the texture key, cell x and z, orientation, mirror, RGB, alpha, mode and the binding's image (14 bytes),
// q-0xA8 whether the texture has alpha, q-0xA7 the zoom it was loaded for (the draw reloads it when the
// context's zoom differs, so the record carries the context's), q-0xA4 the texture binding, q-0x88
// the four vertices of a flat cell, q-0x28 whether it is flat.
//
// Each site becomes a call to a stub below plus a NOP. Every patch checks the bytes it replaces and is
// undone in PreAppShutdown.

#include "DrawOwnerHooks.h"

#include "cGDriverWire.h"
#include "Diagnostics.h"
#include "GameVersion.h"
#include "Settings.h"

#include <cIGZUnknown.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>

#include <windows.h>

namespace nSCD3D11::DrawOwnerHooks {
	namespace {
		constexpr uint32_t kOccupantIID = 0x26BB767E;
		constexpr ptrdiff_t kContextPass = 0x50;

		struct Site {
			uintptr_t address;
			std::array<uint8_t, 6> expected;
			void *stub;
			std::array<uint8_t, 6> saved{};
			bool installed = false;
		};

		bool gInstalled = false;

		// The occupant behind a drawable's owner, with the same id OccupantTracker gives it.
		uint64_t CanonicalOccupant(void *owner) {
			if (owner == nullptr) return 0;
			void *occupant = nullptr;
			if (!static_cast<cIGZUnknown *>(owner)->QueryInterface(kOccupantIID, &occupant) || occupant == nullptr) {
				return 0;
			}
			static_cast<cIGZUnknown *>(occupant)->Release();
			return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(occupant));
		}

		nSCDWire::DrawOwner Describe(void *drawable, void *context, nSCDWire::DrawOwnerKind kind, uint64_t occupant) {
			nSCDWire::DrawOwner owner{};
			owner.occupant = occupant;
			owner.drawable = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(drawable));
			owner.vtable = static_cast<uint32_t>(*static_cast<uintptr_t *>(drawable));
			owner.kind = kind;
			owner.pass = context != nullptr ? *reinterpret_cast<uint8_t *>(static_cast<uint8_t *>(context) + kContextPass) : 0;
			return owner;
		}

		constexpr uintptr_t kModelInstanceVtable = 0x00ABD508;
		constexpr uintptr_t kAnimatedModelInstanceVtable = 0x00ABD018;
		constexpr uintptr_t kResModelVtable = 0x00ABD608;
		constexpr uintptr_t kResModelPersistVtable = 0x00ABD62C;

		bool ReadModelInstance(void *drawable, uintptr_t vtable, nSCDWire::ModelInstance &model) {
			if (vtable != kModelInstanceVtable && vtable != kAnimatedModelInstanceVtable) return false;
			auto const *const p = static_cast<uint8_t const *>(drawable);
			uint8_t const *q = p - *reinterpret_cast<int32_t const *>(p - 4);
			if (vtable == kAnimatedModelInstanceVtable) q -= 0x24;
			model = {};
			auto const *const resource = *reinterpret_cast<uint8_t const *const *>(q - 0x78);
			if (resource != nullptr && *reinterpret_cast<uintptr_t const *>(resource) == kResModelVtable &&
			    *reinterpret_cast<uintptr_t const *>(resource + 4) == kResModelPersistVtable) {
				model.type = *reinterpret_cast<uint32_t const *>(resource + 0x10);
				model.group = *reinterpret_cast<uint32_t const *>(resource + 0x14);
				model.instance = *reinterpret_cast<uint32_t const *>(resource + 0x18);
			}
			model.changeCount = q[-0x74];
			model.parts = q[-0x73];
			std::memcpy(model.rotation, q - 0x70, sizeof(model.rotation) + sizeof(model.translation) + sizeof(model.scale));
			std::memcpy(model.attributes, q - 0x24, sizeof(model.attributes));
			model.frame = *reinterpret_cast<uint16_t const *>(q - 0x0C);
			model.properties = *reinterpret_cast<uint16_t const *>(q - 0x0A);
			return true;
		}

		constexpr uintptr_t kNetworkPieceVtable = 0x00AA7F10;
		constexpr uintptr_t kCellTextureVtable = 0x00AA2C30;
		constexpr ptrdiff_t kContextZoom = 0x14;
		// More would mean the fields are not what they seem.
		constexpr size_t kMaxNetworkParts = 64;

		uint8_t const *Body(void *drawable) {
			auto const *const p = static_cast<uint8_t const *>(drawable);
			return p - *reinterpret_cast<int32_t const *>(p - 4);
		}

		uint8_t ContextZoom(void *context) {
			return context != nullptr ? static_cast<uint8_t>(*reinterpret_cast<uint32_t const *>(
				static_cast<uint8_t const *>(context) + kContextZoom)) : 0;
		}

		void RecordNetworkPiece(cGDriverWire &wire, void *drawable, void *context, uint32_t flag) {
			uint8_t const *const q = Body(drawable);
			nSCDWire::NetworkPiece piece{};
			std::memcpy(piece.corners, q - 0xE0, sizeof(piece.corners));
			piece.piece = *reinterpret_cast<uint32_t const *>(q - 0x80);
			piece.underTexture = *reinterpret_cast<uint32_t const *>(q - 0x7C);
			piece.rotationFlip = q[-0x78];
			piece.variation = q[-0x77];
			piece.zoom = ContextZoom(context);
			piece.highlighted = flag != 0;
			piece.flags = *reinterpret_cast<uint32_t const *>(q - 0x40);
			auto const *const begin = *reinterpret_cast<nSCDWire::NetworkPart const *const *>(q - 0x64);
			auto const *const end = *reinterpret_cast<nSCDWire::NetworkPart const *const *>(q - 0x60);
			size_t const count = begin != nullptr && end > begin ? static_cast<size_t>(end - begin) : 0;
			piece.partCount = static_cast<uint32_t>(count <= kMaxNetworkParts ? count : 0);
			wire.RecordNetworkPiece(piece, begin);
		}

		void RecordCellTexture(cGDriverWire &wire, void *drawable, void *context, uint32_t flag) {
			uint8_t const *const q = Body(drawable);
			nSCDWire::CellTexture cell{};
			// Key to element: the record SetTexture copies.
			std::memcpy(&cell, q - 0xB8, offsetof(nSCDWire::CellTexture, textureAlpha));
			cell.textureAlpha = q[-0xA8];
			cell.zoom = ContextZoom(context);
			cell.highlighted = flag != 0;
			cell.bound = *reinterpret_cast<void *const *>(q - 0xA4) != nullptr;
			cell.flat = q[-0x28];
			std::memcpy(cell.vertices, q - 0x88, sizeof(cell.vertices));
			wire.RecordCellTexture(cell);
		}

		uint32_t __fastcall DrawGrid(void *drawable, void *, void *context, uint32_t flag) {
			auto **const vtable = *static_cast<void ***>(drawable);
			using Draw = uint32_t(__thiscall *)(void *, void *, uint32_t);
			cGDriverWire *const wire = cGDriverWire::Current();
			if (wire == nullptr) return reinterpret_cast<Draw>(vtable[12])(drawable, context, flag);
			using GetOwner = void *(__thiscall *)(void *);
			uint64_t const occupant = CanonicalOccupant(reinterpret_cast<GetOwner>(vtable[3])(drawable));
			nSCDWire::DrawOwner const owner = Describe(drawable, context, nSCDWire::DrawOwnerKind::Grid, occupant);
			wire->RecordOwnerBegin(owner);
			nSCDWire::ModelInstance model;
			if (ReadModelInstance(drawable, owner.vtable, model)) wire->RecordModelInstance(model);
			else if (owner.vtable == kNetworkPieceVtable) RecordNetworkPiece(*wire, drawable, context, flag);
			else if (owner.vtable == kCellTextureVtable) RecordCellTexture(*wire, drawable, context, flag);
			uint32_t const result = reinterpret_cast<Draw>(vtable[12])(drawable, context, flag);
			wire->RecordOwnerEnd();
			return result;
		}

		void __fastcall DrawViewObject(void *object, void *, void *context) {
			auto **const vtable = *static_cast<void ***>(object);
			using Update = void(__thiscall *)(void *, void *);
			cGDriverWire *const wire = cGDriverWire::Current();
			if (wire != nullptr) wire->RecordOwnerBegin(Describe(object, context, nSCDWire::DrawOwnerKind::View, 0));
			reinterpret_cast<Update>(vtable[3])(object, context);
			if (wire != nullptr) wire->RecordOwnerEnd();
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		// Entered by a call in place of `call [reg+30h]`: the drawable is in ECX and the original two
		// arguments sit above the return address. Repeats the displaced comparison, then returns past
		// the patch, dropping the arguments the way the original callee did.
		__declspec(naked) void GridStub() {
			__asm {
				push dword ptr [esp + 8]
				push dword ptr [esp + 8]
				call DrawGrid
				cmp  eax, 2
				ret  8
			}
		}

		__declspec(naked) void HighlightStub() {
			__asm {
				push dword ptr [esp + 8]
				push dword ptr [esp + 8]
				call DrawGrid
				inc  edi
				cmp  edi, esi
				ret  8
			}
		}

		// In place of `mov esi,[esi] / push eax / call [edx+0Ch]`: the object is in ECX, the context in EAX.
		__declspec(naked) void ViewStub() {
			__asm {
				mov  esi, dword ptr [esi]
				push eax
				call DrawViewObject
				ret
			}
		}
#endif

		std::array<Site, 7> gSites;

		bool Patch(Site &site) {
			auto *const target = reinterpret_cast<uint8_t *>(site.address);
			if (std::memcmp(target, site.expected.data(), site.expected.size()) != 0) return false;
			std::array<uint8_t, 6> replacement{0xE8, 0, 0, 0, 0, 0x90};
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(site.stub) -
			                                                 (site.address + 5));
			std::memcpy(replacement.data() + 1, &displacement, sizeof(displacement));
			DWORD protection = 0;
			if (!VirtualProtect(target, replacement.size(), PAGE_EXECUTE_READWRITE, &protection)) return false;
			std::memcpy(site.saved.data(), target, site.saved.size());
			std::memcpy(target, replacement.data(), replacement.size());
			DWORD ignored = 0;
			VirtualProtect(target, replacement.size(), protection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), target, replacement.size());
			site.installed = true;
			return true;
		}

		void Unpatch(Site &site) {
			if (!site.installed) return;
			auto *const target = reinterpret_cast<uint8_t *>(site.address);
			DWORD protection = 0;
			if (VirtualProtect(target, site.saved.size(), PAGE_EXECUTE_READWRITE, &protection)) {
				std::memcpy(target, site.saved.data(), site.saved.size());
				DWORD ignored = 0;
				VirtualProtect(target, site.saved.size(), protection, &ignored);
				FlushInstructionCache(GetCurrentProcess(), target, site.saved.size());
			}
			site.installed = false;
		}
	}

	bool Install() {
#if defined(_MSC_VER) && defined(_M_IX86)
		if (gInstalled) return true;
		Settings::Values const &settings = Settings::Get();
		if (!settings.server && settings.record.empty()) return false;
		if (!IsGameVersion641()) {
			Log(LogCategory::Initialization, "draw owners: requires SimCity 4 1.1.641, skipping");
			return false;
		}
		std::array<uint8_t, 6> const gridCall{0xFF, 0x50, 0x30, 0x83, 0xF8, 0x02};
		std::array<uint8_t, 6> const dynamicCall{0xFF, 0x52, 0x30, 0x83, 0xF8, 0x02};
		std::array<uint8_t, 6> const highlightCall{0xFF, 0x52, 0x30, 0x47, 0x3B, 0xFE};
		std::array<uint8_t, 6> const viewCall{0x8B, 0x36, 0x50, 0xFF, 0x52, 0x0C};
		void *const grid = reinterpret_cast<void *>(&GridStub);
		void *const view = reinterpret_cast<void *>(&ViewStub);
		gSites = {{
			{0x007C76BB, gridCall, grid},
			{0x007C7CB1, dynamicCall, grid},
			{0x007C7DA7, highlightCall, reinterpret_cast<void *>(&HighlightStub)},
			{0x007C3E3B, viewCall, view},
			{0x007C3E7B, viewCall, view},
			{0x007C3EBB, viewCall, view},
			{0x007C3EFB, viewCall, view},
		}};
		// All or nothing: a draw bracketed in one pass but not another would mislead the renderer.
		for (Site const &site: gSites) {
			if (std::memcmp(reinterpret_cast<void *>(site.address), site.expected.data(), site.expected.size()) != 0) {
				Log(LogCategory::Initialization, "draw owners: unexpected bytes at 0x%08lX, not installed",
				    static_cast<unsigned long>(site.address));
				return false;
			}
		}
		for (Site &site: gSites) {
			if (!Patch(site)) {
				Uninstall();
				Log(LogCategory::Initialization, "draw owners: patching 0x%08lX failed",
				    static_cast<unsigned long>(site.address));
				return false;
			}
		}
		gInstalled = true;
		Log(LogCategory::Initialization, "draw owners: grid and view object draws are bracketed");
		return true;
#else
		return false;
#endif
	}

	void Uninstall() {
		for (Site &site: gSites) Unpatch(site);
		gInstalled = false;
	}
}
