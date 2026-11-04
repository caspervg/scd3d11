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

#include "Diagnostics.h"
#include "Settings.h"

#include <windows.h>

#include <atomic>
#include <cstring>

static_assert(sizeof(void *) == 4, "amdxx32.dll is the 32-bit driver; the ring holds 32-bit pointers");

namespace nSCD3D11::AmdRingFix {
	namespace {
		// The worker's ring context.
		constexpr size_t kReadPointer = 0x1C0;
		constexpr size_t kConsumed = 0x200;
		constexpr size_t kSegmentEnd = 0x240;
		constexpr size_t kSegments = 0x88;
		constexpr uintptr_t kSegmentSize = 0x100000;
		constexpr uint32_t kBrokenOpcode = 0x3E;
		constexpr uint32_t kWrappingOpcode = 1;

		// A byte pattern, -1 for any byte.
		template <size_t N>
		bool Matches(uint8_t const *code, int const (&pattern)[N]) {
			for (size_t index = 0; index < N; ++index) {
				if (pattern[index] >= 0 && code[index] != pattern[index]) return false;
			}
			return true;
		}

		// The replay loop (ctx in esi, consumed in edx): read the opcode at the read pointer, advance it, count it,
		// stop on 0xFFFFDEAD, skip opcodes past the table, then load the handler from the table.
		constexpr int kDispatch[] = {
			0x8B, 0x8E, 0xC0, 0x01, 0x00, 0x00,       // mov ecx,[esi+1C0h]
			0x8D, 0x42, 0x04,                         // lea eax,[edx+4]
			0x8B, 0x39,                               // mov edi,[ecx]
			0x83, 0xC1, 0x04,                         // add ecx,4
			0x89, 0x8E, 0xC0, 0x01, 0x00, 0x00,       // mov [esi+1C0h],ecx
			0x89, 0x86, 0x00, 0x02, 0x00, 0x00,       // mov [esi+200h],eax
			0x81, 0xFF, 0xAD, 0xDE, 0xFF, 0xFF,       // cmp edi,0FFFFDEADh
			0x75, -1,                                 // jne
			0xC6, 0x45, -1, 0x00,                     // mov byte ptr [ebp-x],0
			0xEB, -1,                                 // jmp
			0x81, 0xFF, -1, -1, 0x00, 0x00,           // cmp edi,<opcode count>
			0x73, -1,                                 // jae
			0x8B, 0x3C, 0xBD, -1, -1, -1, -1,         // mov edi,[table+edi*4]
		};
		constexpr size_t kOpcodeCountOffset = 42;
		constexpr size_t kTableOffset = 51;

		// Opcode 0x3E: call the replay target's method and return. Nothing reads or moves the read pointer.
		constexpr int kUnwrappedHandler[] = {
			0x56,                                     // push esi
			0x57,                                     // push edi
			0x8B, 0x79, 0x40,                         // mov edi,[ecx+40h]
			0x8B, 0x07,                               // mov eax,[edi]
			0x8B, 0xB0, -1, -1, 0x00, 0x00,           // mov esi,[eax+method]
			0x8B, 0xCE,                               // mov ecx,esi
			0xFF, 0x15, -1, -1, -1, -1,               // call [__guard_check_icall_fptr]
			0x8B, 0xCF,                               // mov ecx,edi
			0xFF, 0xD6,                               // call esi
			0x5F,                                     // pop edi
			0x5E,                                     // pop esi
			0xC3,                                     // ret
		};

		// The wrap step as opcode 1's handler has it (ctx in ebx, read pointer in ecx, consumed in edx).
		constexpr int kWrapStep[] = {
			0x3B, 0x8B, 0x40, 0x02, 0x00, 0x00,       // cmp ecx,[ebx+240h]
			0x72, -1,                                 // jb
			0x8B, 0x49, 0x04,                         // mov ecx,[ecx+4]
			0x8D, 0x42, 0x08,                         // lea eax,[edx+8]
			0x89, 0x83, 0x00, 0x02, 0x00, 0x00,       // mov [ebx+200h],eax
			0x8B, 0x84, 0x8B, 0x88, 0x00, 0x00, 0x00, // mov eax,[ebx+ecx*4+88h]
			0x89, 0x83, 0xC0, 0x01, 0x00, 0x00,       // mov [ebx+1C0h],eax
			0x05, 0x00, 0x00, 0x10, 0x00,             // add eax,100000h
			0x89, 0x83, 0x40, 0x02, 0x00, 0x00,       // mov [ebx+240h],eax
		};
		// How far into a handler to look for it.
		constexpr size_t kWrapStepSearch = 0x80;

		using Handler = void(__fastcall *)(uint8_t *);
		std::atomic<Handler> gOriginal{nullptr};
		std::atomic<uint32_t> gCalls{0};
		std::atomic<uint32_t> gWraps{0};
		void *const *gTable = nullptr;

		// Called by the worker as thiscall with the ring context in ecx, like AMD's own handlers. Wraps the way every
		// other handler does after its payload, then runs AMD's handler.
		void __fastcall FixedHandler(uint8_t *context) {
			gCalls.fetch_add(1, std::memory_order_relaxed);
			if (WrapAtSegmentEnd(context)) gWraps.fetch_add(1, std::memory_order_relaxed);
			gOriginal.load(std::memory_order_relaxed)(context);
		}

		uint32_t ReadDword(uint8_t const *at) {
			uint32_t value = 0;
			std::memcpy(&value, at, sizeof(value));
			return value;
		}

		struct Section {
			uint8_t const *start = nullptr;
			size_t size = 0;
		};

		Section TextSection(HMODULE module) {
			auto const *const image = reinterpret_cast<uint8_t const *>(module);
			auto const *const dos = reinterpret_cast<IMAGE_DOS_HEADER const *>(image);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
			auto const *const nt = reinterpret_cast<IMAGE_NT_HEADERS const *>(image + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
			IMAGE_SECTION_HEADER const *section = IMAGE_FIRST_SECTION(nt);
			for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
				if (std::memcmp(section->Name, ".text", 6) == 0) {
					return {image + section->VirtualAddress, section->Misc.VirtualSize};
				}
			}
			return {};
		}

		bool InSection(Section const &section, uintptr_t address, size_t size) {
			auto const start = reinterpret_cast<uintptr_t>(section.start);
			return address >= start && address - start <= section.size && section.size - (address - start) >= size;
		}

		bool InImage(HMODULE module, uintptr_t address, size_t size) {
			auto const *const image = reinterpret_cast<uint8_t const *>(module);
			auto const *const nt = reinterpret_cast<IMAGE_NT_HEADERS const *>(
				image + reinterpret_cast<IMAGE_DOS_HEADER const *>(image)->e_lfanew);
			auto const start = reinterpret_cast<uintptr_t>(image);
			size_t const imageSize = nt->OptionalHeader.SizeOfImage;
			return address >= start && address - start <= imageSize && imageSize - (address - start) >= size;
		}

		char const *Describe(Result result) {
			switch (result) {
			case Result::NotAmd: return "no AMD D3D11 driver loaded";
			case Result::NoMatch: return "the driver's code differs from the known layout, left unpatched";
			case Result::Patched: return "patched the command replay's opcode 0x3E handler";
			case Result::AlreadyPatched: return "already patched";
			case Result::Failed: return "the layout matched but the handler table could not be written";
			}
			return "?";
		}
	} // namespace

	std::optional<Dispatch> FindDispatch(uint8_t const *code, size_t size) {
		constexpr size_t length = sizeof(kDispatch) / sizeof(kDispatch[0]);
		std::optional<Dispatch> found;
		for (size_t offset = 0; offset + length <= size; ++offset) {
			if (code[offset] != 0x8B || !Matches(code + offset, kDispatch)) continue;
			// Two loops would leave it unclear which table the worker uses.
			if (found) return std::nullopt;
			found = Dispatch{ReadDword(code + offset + kTableOffset), ReadDword(code + offset + kOpcodeCountOffset)};
		}
		return found;
	}

	bool IsUnwrappedHandler(uint8_t const *code, size_t size) {
		return size >= sizeof(kUnwrappedHandler) / sizeof(kUnwrappedHandler[0]) && Matches(code, kUnwrappedHandler);
	}

	bool HasWrapStep(uint8_t const *code, size_t size) {
		constexpr size_t length = sizeof(kWrapStep) / sizeof(kWrapStep[0]);
		for (size_t offset = 0; offset + length <= size && offset < kWrapStepSearch; ++offset) {
			if (Matches(code + offset, kWrapStep)) return true;
		}
		return false;
	}

	bool WrapAtSegmentEnd(uint8_t *context) {
		uintptr_t readPointer = 0;
		uintptr_t segmentEnd = 0;
		std::memcpy(&readPointer, context + kReadPointer, sizeof(readPointer));
		std::memcpy(&segmentEnd, context + kSegmentEnd, sizeof(segmentEnd));
		if (readPointer < segmentEnd) return false;
		// The trailer the recording side wrote: {0x7890ABCD, next segment}.
		uint32_t const next = ReadDword(reinterpret_cast<uint8_t const *>(readPointer) + 4);
		uint32_t consumed = ReadDword(context + kConsumed) + 8;
		std::memcpy(context + kConsumed, &consumed, sizeof(consumed));
		uintptr_t segment = 0;
		std::memcpy(&segment, context + kSegments + next * sizeof(uint32_t), sizeof(segment));
		uintptr_t const end = segment + kSegmentSize;
		std::memcpy(context + kReadPointer, &segment, sizeof(segment));
		std::memcpy(context + kSegmentEnd, &end, sizeof(end));
		return true;
	}

	void LogCounts() {
		if (gTable == nullptr) return;
		Counts const counts = GetCounts();
		Log(LogCategory::Initialization, "AMD driver fix: opcode 0x3E replayed %u times, %u of them at a segment end",
		    counts.calls, counts.wraps);
	}

	void *const *PatchedHandlerSlot() {
		return gTable;
	}

	Counts GetCounts() {
		return {gCalls.load(), gWraps.load()};
	}

	Result Apply() {
		HMODULE const module = GetModuleHandleW(L"amdxx32.dll");
		if (module == nullptr) return Result::NotAmd;
		Section const text = TextSection(module);
		if (text.start == nullptr) return Result::NoMatch;
		std::optional<Dispatch> const dispatch = FindDispatch(text.start, text.size);
		if (!dispatch || dispatch->opcodes <= kBrokenOpcode ||
		    !InImage(module, dispatch->table, dispatch->opcodes * sizeof(uint32_t))) {
			return Result::NoMatch;
		}
		auto *const table = reinterpret_cast<uintptr_t *>(dispatch->table);
		uintptr_t const broken = table[kBrokenOpcode];
		if (broken == reinterpret_cast<uintptr_t>(&FixedHandler)) return Result::AlreadyPatched;
		uintptr_t const wrapping = table[kWrappingOpcode];
		if (!InSection(text, broken, sizeof(kUnwrappedHandler) / sizeof(kUnwrappedHandler[0])) ||
		    !InSection(text, wrapping, kWrapStepSearch + sizeof(kWrapStep) / sizeof(kWrapStep[0]))) {
			return Result::NoMatch;
		}
		auto const *const brokenCode = reinterpret_cast<uint8_t const *>(broken);
		auto const *const wrappingCode = reinterpret_cast<uint8_t const *>(wrapping);
		if (!IsUnwrappedHandler(brokenCode, sizeof(kUnwrappedHandler) / sizeof(kUnwrappedHandler[0])) ||
		    !HasWrapStep(wrappingCode, kWrapStepSearch + sizeof(kWrapStep) / sizeof(kWrapStep[0]))) {
			return Result::NoMatch;
		}

		// The worker calls the replacement until the process ends, so SCD3D11 must never unload.
		HMODULE self = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		                        reinterpret_cast<LPCWSTR>(&FixedHandler), &self)) {
			return Result::Failed;
		}
		gOriginal.store(reinterpret_cast<Handler>(broken));
		DWORD protection = 0;
		if (!VirtualProtect(&table[kBrokenOpcode], sizeof(uintptr_t), PAGE_READWRITE, &protection)) return Result::Failed;
		// One aligned pointer: a worker replaying meanwhile sees either handler.
		InterlockedExchange(reinterpret_cast<LONG volatile *>(&table[kBrokenOpcode]),
		                    static_cast<LONG>(reinterpret_cast<uintptr_t>(&FixedHandler)));
		DWORD ignored = 0;
		VirtualProtect(&table[kBrokenOpcode], sizeof(uintptr_t), protection, &ignored);
		gTable = reinterpret_cast<void *const *>(table);
		return Result::Patched;
	}

	void Install() {
		if (!Settings::Get().amdCrashFix) return;
		static bool logged = false;
		Result const result = Apply();
		if (result == Result::NotAmd || (logged && result == Result::AlreadyPatched)) return;
		logged = true;
		Log(LogCategory::Initialization, "AMD driver fix: %s", Describe(result));
	}
} // namespace nSCD3D11::AmdRingFix
