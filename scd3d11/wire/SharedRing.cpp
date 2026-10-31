/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "SharedRing.h"

#include <algorithm>
#include <cstring>

namespace nSCDWire {
	namespace {
		constexpr DWORD kWaitSliceMs = 50;
		constexpr size_t kReadChunk = 1u << 20;
		constexpr ULONGLONG kStalledReaderMs = 10000;
	}

	SharedRing::~SharedRing() {
		if (control != nullptr) UnmapViewOfFile(control);
		for (HANDLE handle: {mapping, dataEvent, spaceEvent, frameEvent}) {
			if (handle != nullptr) CloseHandle(handle);
		}
	}

	bool SharedRing::Create(std::wstring const &name, uint64_t capacity) {
		uint64_t const total = sizeof(RingControl) + capacity;
		mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(total >> 32),
		                             static_cast<DWORD>(total), name.c_str());
		if (mapping == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) return false;
		control = static_cast<RingControl *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (control == nullptr) return false;
		new(control) RingControl{};
		control->magic = kRingMagic;
		control->version = kRingVersion;
		control->capacity = capacity;
		bytes = reinterpret_cast<uint8_t *>(control + 1);
		dataEvent = CreateEventW(nullptr, FALSE, FALSE, (name + L"-data").c_str());
		spaceEvent = CreateEventW(nullptr, FALSE, FALSE, (name + L"-space").c_str());
		frameEvent = CreateEventW(nullptr, FALSE, FALSE, (name + L"-frame").c_str());
		return dataEvent != nullptr && spaceEvent != nullptr && frameEvent != nullptr;
	}

	bool SharedRing::Open(std::wstring const &name) {
		mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
		if (mapping == nullptr) return false;
		control = static_cast<RingControl *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (control == nullptr || control->magic != kRingMagic || control->version != kRingVersion) return false;
		bytes = reinterpret_cast<uint8_t *>(control + 1);
		dataEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, (name + L"-data").c_str());
		spaceEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, (name + L"-space").c_str());
		frameEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, (name + L"-frame").c_str());
		return dataEvent != nullptr && spaceEvent != nullptr && frameEvent != nullptr;
	}

	bool SharedRing::Write(void const *data, size_t size, std::function<bool()> const &alive) {
		uint8_t const *source = static_cast<uint8_t const *>(data);
		uint64_t const capacity = control->capacity;
		while (size != 0) {
			uint64_t const written = control->written.load(std::memory_order_relaxed);
			uint64_t const free = capacity - (written - control->read.load(std::memory_order_acquire));
			if (free == 0) {
				SetEvent(dataEvent);
				if (WaitForSingleObject(spaceEvent, kWaitSliceMs) == WAIT_TIMEOUT && !alive()) return false;
				continue;
			}
			uint64_t const offset = written % capacity;
			size_t const count = static_cast<size_t>((std::min)({static_cast<uint64_t>(size), free, capacity - offset}));
			std::memcpy(bytes + offset, source, count);
			control->written.store(written + count, std::memory_order_release);
			source += count;
			size -= count;
		}
		SetEvent(dataEvent);
		return true;
	}

	uint8_t *SharedRing::WriteSpan(uint64_t at, size_t &length, std::function<bool()> const &alive) {
		uint64_t const capacity = control->capacity;
		// The reader cannot read what is not published, so a record a whole ring long never finds room.
		if (at - control->written.load(std::memory_order_relaxed) >= capacity) return nullptr;
		// A reader that said it is ready and then takes nothing for this long is stuck, even if its process still
		// runs. One still starting (creating its device, say) is waited for as long as it lives.
		ULONGLONG deadline = GetTickCount64() + kStalledReaderMs;
		uint64_t lastRead = control->read.load(std::memory_order_acquire);
		for (;;) {
			uint64_t const read = control->read.load(std::memory_order_acquire);
			if (read != lastRead || control->serverReady.load(std::memory_order_acquire) == 0) {
				lastRead = read;
				deadline = GetTickCount64() + kStalledReaderMs;
			}
			uint64_t const free = capacity - (at - read);
			if (free != 0) {
				uint64_t const offset = at % capacity;
				length = static_cast<size_t>((std::min)(free, capacity - offset));
				return bytes + offset;
			}
			SetEvent(dataEvent);
			if (WaitForSingleObject(spaceEvent, kWaitSliceMs) == WAIT_TIMEOUT &&
			    (!alive() || GetTickCount64() > deadline)) {
				return nullptr;
			}
		}
	}

	void SharedRing::CopyOut(uint64_t at, void *data, size_t size) const {
		uint64_t const capacity = control->capacity;
		uint8_t *target = static_cast<uint8_t *>(data);
		while (size != 0) {
			uint64_t const offset = at % capacity;
			size_t const count = static_cast<size_t>((std::min)(static_cast<uint64_t>(size), capacity - offset));
			std::memcpy(target, bytes + offset, count);
			target += count;
			at += count;
			size -= count;
		}
	}

	void SharedRing::CopyIn(uint64_t at, void const *data, size_t size) {
		uint64_t const capacity = control->capacity;
		uint8_t const *source = static_cast<uint8_t const *>(data);
		while (size != 0) {
			uint64_t const offset = at % capacity;
			size_t const count = static_cast<size_t>((std::min)(static_cast<uint64_t>(size), capacity - offset));
			std::memcpy(bytes + offset, source, count);
			source += count;
			at += count;
			size -= count;
		}
	}

	void SharedRing::Publish(uint64_t end) {
		control->written.store(end, std::memory_order_release);
		SetEvent(dataEvent);
	}

	RingOutput::RingOutput(SharedRing &ring, std::function<bool()> alive, std::function<void()> failed,
	                       std::shared_ptr<std::FILE> file)
		: ring(ring), alive(std::move(alive)), failed(std::move(failed)), file(std::move(file)),
		  origin(ring.Control()->written.load(std::memory_order_relaxed)) {}

	bool RingOutput::Fail() {
		if (failed) {
			auto const call = std::move(failed);
			failed = nullptr;
			call();
		}
		return false;
	}

	uint8_t *RingOutput::Span(uint64_t at, size_t &length) {
		uint8_t *const span = alive() ? ring.WriteSpan(origin + at, length, alive) : nullptr;
		if (span == nullptr) Fail();
		return span;
	}

	void RingOutput::Read(uint64_t at, void *data, size_t size) const { ring.CopyOut(origin + at, data, size); }

	void RingOutput::Overwrite(uint64_t at, void const *data, size_t size) { ring.CopyIn(origin + at, data, size); }

	bool RingOutput::Publish(uint64_t end) {
		if (!alive()) return Fail();
		uint64_t const from = ring.Control()->written.load(std::memory_order_relaxed);
		uint64_t const to = origin + end;
		if (file != nullptr) {
			// From the ring, before the reader can free those bytes for the next ones (only this side writes).
			uint64_t const capacity = ring.Control()->capacity;
			bool ok = true;
			for (uint64_t at = from; at < to && ok;) {
				uint64_t const offset = at % capacity;
				size_t const count = static_cast<size_t>((std::min)(to - at, capacity - offset));
				ok = std::fwrite(ring.Data() + offset, 1, count, file.get()) == count;
				at += count;
			}
			if (!ok || std::fflush(file.get()) != 0) return Fail();
		}
		ring.Publish(to);
		return true;
	}

	void SharedRing::Close() {
		control->closed.store(1, std::memory_order_release);
		SetEvent(dataEvent);
	}

	bool SharedRing::Read(void *data, size_t size, std::function<bool()> const &alive) {
		uint8_t *target = static_cast<uint8_t *>(data);
		uint64_t const capacity = control->capacity;
		while (size != 0) {
			if (chunkOffset < chunk.size()) {
				size_t const count = (std::min)(size, chunk.size() - chunkOffset);
				std::memcpy(target, chunk.data() + chunkOffset, count);
				chunkOffset += count;
				target += count;
				size -= count;
				continue;
			}
			uint64_t const read = control->read.load(std::memory_order_relaxed);
			uint64_t const available = control->written.load(std::memory_order_acquire) - read;
			if (available == 0) {
				if (control->closed.load(std::memory_order_acquire) != 0) return false;
				if (WaitForSingleObject(dataEvent, kWaitSliceMs) == WAIT_TIMEOUT && !alive()) return false;
				continue;
			}
			uint64_t const offset = read % capacity;
			size_t const count = static_cast<size_t>(
				(std::min)({available, static_cast<uint64_t>(kReadChunk), capacity - offset}));
			chunk.assign(bytes + offset, bytes + offset + count);
			chunkOffset = 0;
			control->read.store(read + count, std::memory_order_release);
			SetEvent(spaceEvent);
		}
		return true;
	}
}
