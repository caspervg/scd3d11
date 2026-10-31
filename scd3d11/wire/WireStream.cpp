/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "WireStream.h"

#include <algorithm>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nSCDWire {
	BlobKey HashBlob(void const *data, uint32_t size) {
		XXH128_hash_t const hash = XXH3_128bits(data, size);
		return BlobKey{hash.low64, hash.high64, size};
	}

	namespace {
		constexpr uint64_t kPublishBytes = 8u << 20;
	}

	uint8_t *SinkOutput::Span(uint64_t at, size_t &length) {
		size_t const offset = static_cast<size_t>(at - base);
		if (offset >= buffer.size()) buffer.resize((std::max)(buffer.size() * 2, offset + (1u << 20)));
		length = buffer.size() - offset;
		return buffer.data() + offset;
	}

	void SinkOutput::Read(uint64_t at, void *data, size_t size) const {
		std::memcpy(data, buffer.data() + (at - base), size);
	}

	void SinkOutput::Overwrite(uint64_t at, void const *data, size_t size) {
		std::memcpy(buffer.data() + (at - base), data, size);
	}

	bool SinkOutput::Publish(uint64_t end) {
		// The writer publishes up to where it writes, so nothing stays behind.
		size_t const size = static_cast<size_t>(end - base);
		base = end;
		return sink(buffer.data(), size);
	}

	Writer::Writer(std::unique_ptr<Output> output, uint64_t blobBudget)
		: output(std::move(output)), blobBudget(blobBudget) {
		StreamHeader const header{};
		Put(header);
	}

	void Writer::Write(void const *data, size_t size) {
		uint8_t const *bytes = static_cast<uint8_t const *>(data);
		while (size != 0 && !failed) {
			if (spanLeft == 0) {
				span = output->Span(cursor, spanLeft);
				if (span == nullptr) {
					failed = true;
					spanLeft = 0;
					return;
				}
			}
			size_t const count = (std::min)(size, spanLeft);
			std::memcpy(span, bytes, count);
			span += count;
			spanLeft -= count;
			cursor += count;
			bytes += count;
			size -= count;
		}
	}

	void Writer::Seek(uint64_t position) {
		cursor = position;
		span = nullptr;
		spanLeft = 0;
	}

	void Writer::Begin(Op op) {
		current = op;
		recordStart = cursor;
		// The size is filled in by End.
		Put(RecordHeader{static_cast<uint16_t>(op), 0, 0});
	}

	void Writer::PutBlob(void const *data, uint32_t size) {
		BlobKey const key = size == 0 ? BlobKey{} : HashBlob(data, size);
		if (size != 0) {
			auto const found = blobs.find(key);
			if (found != blobs.end()) {
				blobOrder.splice(blobOrder.end(), blobOrder, found->second);
			} else if (!failed) {
				// The blob's record goes in front of the open one, whose bytes so far move behind it.
				moved.resize(static_cast<size_t>(cursor - recordStart));
				output->Read(recordStart, moved.data(), moved.size());
				Seek(recordStart);
				// Make room first. Blobs this record already uses were just moved to the back, so they go last.
				while (blobBytesHeld + size > blobBudget && !blobOrder.empty()) {
					BlobKey const victim = blobOrder.front();
					blobOrder.pop_front();
					blobs.erase(victim);
					blobBytesHeld -= victim.size;
					Append(Op::ForgetBlob, &victim, sizeof(victim));
				}
				Put(RecordHeader{static_cast<uint16_t>(Op::Blob), 0, static_cast<uint32_t>(sizeof(key) + size)});
				Put(key);
				PutBytes(data, size);
				PublishIfLarge();
				recordStart = cursor;
				PutBytes(moved.data(), moved.size());
				blobs.emplace(key, blobOrder.insert(blobOrder.end(), key));
				blobBytesHeld += size;
				blobBytesSent += size;
			}
		}
		Put(key);
	}

	void Writer::End() {
		if (!failed) {
			RecordHeader const header{static_cast<uint16_t>(current), 0,
			                          static_cast<uint32_t>(cursor - recordStart - sizeof(RecordHeader))};
			output->Overwrite(recordStart, &header, sizeof(header));
		}
		current = Op::Count;
		PublishIfLarge();
	}

	void Writer::Append(Op op, void const *data, size_t size) {
		Put(RecordHeader{static_cast<uint16_t>(op), 0, static_cast<uint32_t>(size)});
		PutBytes(data, size);
	}

	void Writer::PublishIfLarge() {
		if (cursor - published >= kPublishBytes) Flush();
	}

	bool Writer::Flush() {
		if (failed) return false;
		if (cursor == published) return true;
		if (!output->Publish(cursor)) failed = true;
		published = cursor;
		// The output may put the next bytes elsewhere.
		Seek(cursor);
		return !failed;
	}

	uint8_t const *Fields::Bytes(size_t count) {
		if (!ok || count > size - offset) {
			ok = false;
			return nullptr;
		}
		uint8_t const *result = data + offset;
		offset += static_cast<uint32_t>(count);
		return result;
	}

	Reader::Reader(Source source) : source(std::move(source)) {}

	bool Reader::Next(Op &op, Fields &fields) {
		if (!started) {
			StreamHeader header{};
			if (!source(&header, sizeof(header))) {
				error = "the stream is empty";
				return false;
			}
			if (header.magic != kMagic || header.version != kVersion) {
				error = "not an SCDW stream of this version";
				return false;
			}
			started = true;
		}
		for (;;) {
			RecordHeader header{};
			if (!source(&header, sizeof(header))) return false;
			if (header.op == 0 || header.op >= static_cast<uint16_t>(Op::Count)) {
				error = "unknown record";
				return false;
			}
			payload.resize(header.size);
			if (header.size != 0 && !source(payload.data(), header.size)) {
				error = "the stream ends inside a record";
				return false;
			}
			op = static_cast<Op>(header.op);
			if (op == Op::Blob) {
				BlobKey key{};
				if (header.size < sizeof(key)) {
					error = "short blob";
					return false;
				}
				std::memcpy(&key, payload.data(), sizeof(key));
				if (key.size != header.size - sizeof(key)) {
					error = "blob size mismatch";
					return false;
				}
				blobs[key].assign(payload.begin() + sizeof(key), payload.end());
				continue;
			}
			if (op == Op::ForgetBlob) {
				BlobKey key{};
				if (header.size == sizeof(key)) {
					std::memcpy(&key, payload.data(), sizeof(key));
					blobs.erase(key);
				}
				continue;
			}
			fields = Fields(payload.data(), header.size);
			return true;
		}
	}

	std::vector<uint8_t> const *Reader::FindBlob(BlobKey const &key) const {
		static std::vector<uint8_t> const empty;
		if (key.size == 0) return &empty;
		auto const found = blobs.find(key);
		return found == blobs.end() ? nullptr : &found->second;
	}

	char const *OpName(Op op) {
		static char const *const names[] = {
			"?", "Blob", "ForgetBlob", "Frame", "DrawArrays", "DrawElements", "Clear", "ClearColor", "ClearDepth",
			"ClearStencil", "ColorMask", "DepthFunc", "DepthMask", "StencilFunc", "StencilMask", "StencilOp",
			"BlendFunc", "AlphaFunc", "ShadeModel", "BindTexture", "TexImage2D", "PixelStore", "TexEnvInt",
			"TexEnvFloat", "TexParameter", "FogInt", "FogFloat", "ColorMultiplier", "AlphaMultiplier",
			"EnableVertexColors", "GenTextures", "DeleteTextures", "MatrixMode", "LoadMatrix", "LoadIdentity",
			"Enable", "Disable", "TexStage", "TexStageCoord", "TexStageMatrix", "TexStageCombineMode",
			"TexStageCombineSource", "TexStageCombineOperand", "TexStageCombineScale", "SetTexture",
			"CreateTexture", "LoadTextureLevel", "SetCombiner", "SetVideoMode", "PolygonOffset", "Blit",
			"SetViewportFull", "SetViewport", "Init", "Shutdown", "PuntBlitSourceAlpha", "NewBufferRegion",
			"DeleteBufferRegion", "ReadBufferRegion", "DrawBufferRegion", "DeleteAllBufferRegions",
			"EnableLighting", "EnableLight", "LightModelAmbient", "LightColor", "LightColors", "LightPosition",
			"LightDirection", "MaterialColor", "MaterialColors", "GetVertices", "ContinueVertices",
			"ReleaseVertices", "DrawPrims", "DrawPrimsIndexed", "ResetVertexBuffers", "CopyColorBuffer",
			"OccupantInsert", "OccupantRemove", "OccupantMove", "OwnerBegin", "OwnerEnd", "ModelInstance", "Resize",
			"GameFolders", "NetworkPiece", "CellTexture", "OccupantExemplar", "FreeCamera", "TerrainField",
		};
		static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Op::Count));
		size_t const index = static_cast<size_t>(op);
		return index < static_cast<size_t>(Op::Count) ? names[index] : "?";
	}
}
