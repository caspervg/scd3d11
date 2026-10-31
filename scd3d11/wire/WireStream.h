/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "Wire.h"

#include <cstddef>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace nSCDWire {
	struct BlobKeyHash {
		size_t operator()(BlobKey const &key) const { return static_cast<size_t>(key.low ^ key.high); }
	};

	BlobKey HashBlob(void const *data, uint32_t size);

	// Where a Writer's bytes go. The writer fills the spans it is handed, at stream positions counted from 0,
	// and publishes up to a record boundary: the reader sees nothing before that. Bytes not yet published can
	// be read back and overwritten, for the header of the open record and to move it behind a blob it needs.
	class Output {
	public:
		virtual ~Output() = default;
		// Writable bytes from position at, at least one and as many as lie in one piece. Waits for room. Null
		// when the stream can no longer be written.
		virtual uint8_t *Span(uint64_t at, size_t &length) = 0;
		virtual void Read(uint64_t at, void *data, size_t size) const = 0;
		virtual void Overwrite(uint64_t at, void const *data, size_t size) = 0;
		// Hands the bytes before end to the reader, and with them every span handed out so far. False when the
		// stream can no longer be written.
		virtual bool Publish(uint64_t end) = 0;
	};

	// Keeps the bytes in memory until they are published, then hands them to a function.
	class SinkOutput final : public Output {
	public:
		// Receives published bytes, in order. Returns false when the stream can no longer be written.
		using Sink = std::function<bool(void const *data, size_t size)>;

		explicit SinkOutput(Sink sink) : sink(std::move(sink)) {}
		uint8_t *Span(uint64_t at, size_t &length) override;
		void Read(uint64_t at, void *data, size_t size) const override;
		void Overwrite(uint64_t at, void const *data, size_t size) override;
		bool Publish(uint64_t end) override;

	private:
		Sink sink;
		std::vector<uint8_t> buffer;
		// The position of buffer[0].
		uint64_t base = 0;
	};

	// Builds records and hands them to an Output, each byte copied once from the caller into the output. Blobs
	// the reader already holds are sent as their key alone. Not thread safe: SC4 drives its graphics driver
	// from one thread.
	class Writer {
	public:
		using Sink = SinkOutput::Sink;

		explicit Writer(std::unique_ptr<Output> output, uint64_t blobBudget = 512ull << 20);
		explicit Writer(Sink sink, uint64_t blobBudget = 512ull << 20)
			: Writer(std::make_unique<SinkOutput>(std::move(sink)), blobBudget) {}

		void Begin(Op op);

		template<typename T>
		void Put(T const &value) {
			static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
			PutBytes(&value, sizeof(T));
		}

		void PutBytes(void const *data, size_t size) {
			if (size <= spanLeft) {
				std::memcpy(span, data, size);
				span += size;
				spanLeft -= size;
				cursor += size;
				return;
			}
			Write(data, size);
		}
		// Writes the blob's key into the current record, sending the bytes first if the reader lacks them.
		void PutBlob(void const *data, uint32_t size);
		void End();

		// Publishes everything up to the last finished record. Done at the end of every frame.
		bool Flush();
		bool Failed() const { return failed; }

		uint64_t BytesWritten() const { return published; }
		uint64_t BlobBytesSent() const { return blobBytesSent; }

	private:
		// PutBytes past the current span.
		void Write(void const *data, size_t size);
		void Append(Op op, void const *data, size_t size);
		// Moves the write position, to the start of the open record when a blob goes in front of it.
		void Seek(uint64_t position);
		// Publishes once this much is waiting, so large uploads go out at once rather than at the frame's end.
		void PublishIfLarge();

		std::unique_ptr<Output> output;
		uint64_t blobBudget;
		uint64_t blobBytesHeld = 0;
		uint64_t blobBytesSent = 0;
		bool failed = false;
		Op current = Op::Count;
		// Where the next byte goes, where the open record's header is, and how far the reader may read.
		uint64_t cursor = 0;
		uint64_t recordStart = 0;
		uint64_t published = 0;
		// The writable bytes at cursor.
		uint8_t *span = nullptr;
		size_t spanLeft = 0;
		// The open record's bytes so far, while a blob goes in front of it.
		std::vector<uint8_t> moved;
		// Least recently used first.
		std::list<BlobKey> blobOrder;
		std::unordered_map<BlobKey, std::list<BlobKey>::iterator, BlobKeyHash> blobs;
	};

	// Reads fields back in the order the writer put them. A short or damaged record sets ok to false and
	// returns zeros from then on, so a decoder checks ok once after reading all fields.
	class Fields {
	public:
		Fields(uint8_t const *data, uint32_t size) : data(data), size(size) {}

		template<typename T>
		T Get() {
			static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
			T value{};
			if (uint8_t const *bytes = Bytes(sizeof(T))) std::memcpy(&value, bytes, sizeof(T));
			return value;
		}

		uint8_t const *Bytes(size_t count);
		bool ok = true;

	private:
		uint8_t const *data;
		uint32_t size;
		uint32_t offset = 0;
	};

	// Splits a stream into records. Blob and ForgetBlob are handled here, so Next only returns the others.
	class Reader {
	public:
		// Fills exactly `size` bytes, or returns false at the end of the stream.
		using Source = std::function<bool(void *data, size_t size)>;

		explicit Reader(Source source);

		// False with an empty error at a clean end of the stream.
		bool Next(Op &op, Fields &fields);
		std::vector<uint8_t> const *FindBlob(BlobKey const &key) const;

		char const *Error() const { return error; }

	private:
		Source source;
		bool started = false;
		char const *error = "";
		std::vector<uint8_t> payload;
		std::unordered_map<BlobKey, std::vector<uint8_t>, BlobKeyHash> blobs;
	};
}
