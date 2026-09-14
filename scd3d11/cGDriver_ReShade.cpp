/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// ReShade add-on integration.
//
// Installed as dxgi.dll or d3d11.dll next to SimCity 4.exe, ReShade wraps the device and swap chain
// SCD3D11 creates and renders its effects in Present, over the whole frame including the UI. When
// ReShade is loaded SCD3D11 registers itself as an add-on instead and
//  - renders the effects at the end of cSC43DRender::Draw, once the city view is complete and before
//    any UI is drawn over it;
//  - binds the scene depth to the DEPTH semantic, already encoded so that ReShade.fxh's perspective
//    linearization turns it back into SC4's depth, which is linear because the camera is orthographic
//    (cSC43DRender::UpdateCameraZoomAndRotationParams -> SetOrtho);
//  - for effects written for SC4 (shaders/SimCity4*.fx), binds the unencoded depth buffer to the SC4_DEPTH
//    semantic and sets float4 uniforms by their source annotation:
//      scd3d11_ortho  the camera's projection entries P[0][0], P[1][1], P[2][2], P[3][2]
//      scd3d11_sun    towards the sun along SC4's shadow direction, in view space; w: time of day in hours
//      scd3d11_light  towards the sun SC4 lights terrain and models with, in view space
//      scd3d11_up     world up in view space
//    so they can measure the city in meters at full depth precision and light it like SC4 does;
//  - while a technique annotated scd3d11_replaces_shadows = true is enabled, skips SC4's own shadows: the
//    shadow decals cSTEOverlayManager::DrawOverlays draws on the terrain for buildings, flora and networks, and
//    the hill shadows cSC4LightingManager bakes into the terrain colors. Only memory is changed, never SC4's
//    saved shadow option.
// Only effect runtime events and calls are used. ReShade's regular build (not only the "full add-on
// support" one) allows those for externally registered add-ons. Requires ReShade 6.0 or later.
// -ReShade:off on the command line leaves ReShade's default behaviour untouched.
//
// SC4 keeps its back buffer across Present and redraws only what changed, so effects rendered into it
// persist until the city view is redrawn. Frames that only redraw UI must not get effects again at
// Present; FinishReShadeFrame marks those as done.

#include "cGDriver.h"
#include "Diagnostics.h"

#include <cIGZMessage2Standard.h>
#include <cIGZMessageServer2.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <vector>

namespace nSCD3D11 {
	namespace {
		constexpr uintptr_t kImageBase = 0x00400000;
		// The cSC43DRender vtable slot for cSC43DRender::Draw, its only reference (SimCity 4 1.1.641).
		constexpr uintptr_t kDrawSlotVA = 0x00ABABB4;
		constexpr uintptr_t kDrawVA = 0x007CB530;
		constexpr uintptr_t kForceFullRedrawVA = 0x007C7DD0;
		// cSTEOverlayManager::DrawShadows and DrawShadowsRough draw the shadow decals: __thiscall, three stack
		// arguments, both opening with a six byte sub esp, imm32. They are hooked at their entry rather than at
		// their calls in DrawOverlays, which other plugins (sc4-render-services) retarget to wrappers that still
		// end up in these functions.
		constexpr uintptr_t kDrawShadowsVA = 0x00736BF0;
		constexpr uintptr_t kDrawShadowsRoughVA = 0x00737870;
		constexpr size_t kShadowEntryLength = 6;
		constexpr uint8_t kDrawShadowsEntry[kShadowEntryLength]{0x81, 0xEC, 0xC4, 0x01, 0x00, 0x00};
		constexpr uint8_t kDrawShadowsRoughEntry[kShadowEntryLength]{0x81, 0xEC, 0x9C, 0x00, 0x00, 0x00};
		// cSC43DRender::GetLightingManager returns this field.
		constexpr size_t kRenderLightingManager = 0x118;
		// cSC4LightingManager fields. The directions are world space, turned along with the view like the
		// buildings' pre-rendered lighting (DoZoomAndRotationChange).
		constexpr size_t kLightingTimeOfDay = 0x1C;     // hours
		constexpr size_t kLightingIsNight = 0x24;       // IsNight
		constexpr size_t kLightingSunDirection = 0x58;  // GetColor lights normals facing along it
		constexpr size_t kLightingShadowDirection = 0x64; // GetShadowDirection
		constexpr size_t kLightingChangeData1 = 0xBC;   // sent with every lighting change message
		constexpr size_t kLightingChangeData2 = 0xC0;
		constexpr size_t kLightingHillShadows = 0x1D4;  // tested by GetTerrainVertexColors
		// What cSC4LightingManager::DoMessage does when SC4's shadow quality changes (0xC9F775BB).
		constexpr uintptr_t kDoZoomAndRotationChangeVA = 0x007DB840;
		constexpr uintptr_t kViewUtilitiesPointerVA = 0x00B43DD8; // zoom at +0xC, rotation at +0x10
		constexpr uintptr_t kMessageServerPointerVA = 0x00B43CCC;
		constexpr uintptr_t kOperatorNewVA = 0x009133DA;
		constexpr uintptr_t kMessageConstructorVA = 0x009134D6; // cRZMessage2Standard
		constexpr uint32_t kLightingChangedMessage = 0xC9DA96EA; // the terrain relights itself on it

		char const kShaderSource[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
Texture2D<float> sceneDepth : register(t0);
cbuffer SceneDepthConstants : register(b0) {
	float farPlane;
};
// Inverse of ReShade.fxh: linear = d / (farPlane - d * (farPlane - 1)).
float PSMain(float4 position : SV_POSITION) : SV_TARGET {
	float depth = sceneDepth.Load(int3(position.xy, 0));
	return depth * farPlane / (1.0f + depth * (farPlane - 1.0f));
}
)";

		using DrawFunction = bool(__fastcall *)(void *self, void *edx);
		using ForceFullRedrawFunction = void(__fastcall *)(void *self);
		using DoZoomAndRotationChangeFunction = void(__fastcall *)(void *self, void *edx, uint32_t zoom, uint32_t rotation);
		using OperatorNewFunction = void *(__cdecl *)(size_t size);
		using MessageConstructorFunction = cIGZMessage2Standard *(__fastcall *)(void *self, void *edx);

		enum class SceneValue { Ortho, Sun, Light, Up, Count };

		struct SceneUniform {
			reshade::api::effect_uniform_variable variable;
			SceneValue value;
		};

		DrawFunction gOriginalDraw = nullptr;
		// Where the shadow functions continue after their first instruction; read by the entry stubs.
		uintptr_t gDrawShadowsBody = 0;
		uintptr_t gDrawShadowsRoughBody = 0;
		bool gShadowHooksInstalled = false;
		cGDriver *gDriver = nullptr;
		reshade::api::effect_runtime *gRuntime = nullptr;
		ID3D11ShaderResourceView *gSceneDepthView = nullptr;
		bool gDepthBound = false;
		float gFarPlane = 1000.0f; // ReShade.fxh's default
		std::vector<SceneUniform> gSceneUniforms;
		std::vector<reshade::api::effect_technique> gShadowTechniques;
		bool gShadowsReplaced = false;
		uint8_t gSavedHillShadows = 0;

		uintptr_t Rebase(uintptr_t address) {
			return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + (address - kImageBase);
		}

		reshade::api::resource_view ViewHandle(ID3D11View *view) {
			return reshade::api::resource_view{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view))};
		}

		uint8_t *LightingManager(void *render) {
			return *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(render) + kRenderLightingManager);
		}

		// Replace the first instruction of DrawShadows / DrawShadowsRough: return at once while effects draw the
		// shadows, otherwise run that instruction and continue into the function.
		__declspec(naked) void DrawShadowsEntryStub() {
			__asm {
				cmp  byte ptr [gShadowsReplaced], 0
				jne  skip
				sub  esp, 0x1C4
				jmp  dword ptr [gDrawShadowsBody]
			skip:
				ret  0x0C
			}
		}

		__declspec(naked) void DrawShadowsRoughEntryStub() {
			__asm {
				cmp  byte ptr [gShadowsReplaced], 0
				jne  skip
				sub  esp, 0x9C
				jmp  dword ptr [gDrawShadowsRoughBody]
			skip:
				ret  0x0C
			}
		}

		void ForceFullRedraw(void *render) {
			reinterpret_cast<ForceFullRedrawFunction>(Rebase(kForceFullRedrawVA))(render);
		}

		// SC4's own response to a change of its shadow quality: recompute the hill shadows for the current view,
		// tell the terrain the lighting changed so it relights every cell, and redraw the city.
		void RelightTerrain(void *render, uint8_t *lighting) {
			auto const *const viewUtilities = *reinterpret_cast<uint8_t const **>(Rebase(kViewUtilitiesPointerVA));
			auto *const messageServer = *reinterpret_cast<cIGZMessageServer2 **>(Rebase(kMessageServerPointerVA));
			if (viewUtilities != nullptr && messageServer != nullptr) {
				uint32_t zoom = 0;
				uint32_t rotation = 0;
				memcpy(&zoom, viewUtilities + 0xC, sizeof(zoom));
				memcpy(&rotation, viewUtilities + 0x10, sizeof(rotation));
				reinterpret_cast<DoZoomAndRotationChangeFunction>(Rebase(kDoZoomAndRotationChangeVA))(
					lighting, nullptr, zoom, rotation);
				// Allocated and constructed by SC4's own code, so its Release frees it with the matching heap.
				if (void *const memory = reinterpret_cast<OperatorNewFunction>(Rebase(kOperatorNewVA))(0x2C)) {
					cIGZMessage2Standard *const message =
						reinterpret_cast<MessageConstructorFunction>(Rebase(kMessageConstructorVA))(memory, nullptr);
					message->AddRef();
					message->SetType(kLightingChangedMessage);
					int32_t data = 0;
					memcpy(&data, lighting + kLightingChangeData1, sizeof(data));
					message->SetData1(data);
					memcpy(&data, lighting + kLightingChangeData2, sizeof(data));
					message->SetData2(data);
					messageServer->MessageSend(message);
					message->Release();
				}
			}
			ForceFullRedraw(render);
		}

		// Keeps SC4's shadows off while an enabled technique draws its own.
		void ReplaceStaticShadows(void *render) {
			bool replace = false;
			// Not while ReShade's effects toggle has every effect off, which would leave the city without shadows.
			if (gRuntime != nullptr && gShadowHooksInstalled && gRuntime->get_effects_state()) {
				for (reshade::api::effect_technique const technique: gShadowTechniques) {
					replace = replace || gRuntime->get_technique_state(technique);
				}
			}
			uint8_t *const lighting = LightingManager(render);
			// Checked every frame: loading a city or changing SC4's shadow options turns hill shadows on again.
			if (replace && lighting != nullptr && lighting[kLightingHillShadows] != 0) {
				gSavedHillShadows = lighting[kLightingHillShadows];
				lighting[kLightingHillShadows] = 0;
				RelightTerrain(render, lighting);
			}
			if (replace == gShadowsReplaced) return;
			gShadowsReplaced = replace;
			if (!replace && lighting != nullptr && gSavedHillShadows != 0) {
				lighting[kLightingHillShadows] = gSavedHillShadows;
				gSavedHillShadows = 0;
				RelightTerrain(render, lighting);
			} else {
				// The shadow decals are part of the static view in SC4's backing store.
				ForceFullRedraw(render);
			}
			Log(LogCategory::Initialization, "reshade: SC4's static shadows %s", replace ? "replaced by effects" : "restored");
		}

		// Rotates a world direction into view space with the view the terrain was last drawn with.
		bool ToViewSpace(float const *view, float const *world, float *out) {
			for (int row = 0; row < 3; ++row) {
				out[row] = view[row] * world[0] + view[row + 4] * world[1] + view[row + 8] * world[2];
			}
			float const length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
			if (!(length > 1e-6f)) return false;
			for (int i = 0; i < 3; ++i) out[i] /= length;
			return true;
		}

		bool TowardsSun(float const *view, uint8_t const *lighting, size_t field, float *out) {
			float direction[3];
			memcpy(direction, lighting + field, sizeof(direction));
			// The sun is above the ground, whichever way round SC4 stores the vector.
			if (direction[1] < 0.0f) for (float &component: direction) component = -component;
			return ToViewSpace(view, direction, out);
		}

		void ReadSunlight(void *render, float const *view, float values[][4]) {
			uint8_t const *const lighting = LightingManager(render);
			float const worldUp[3]{0.0f, 1.0f, 0.0f};
			float *const sun = values[static_cast<size_t>(SceneValue::Sun)];
			float *const light = values[static_cast<size_t>(SceneValue::Light)];
			float *const up = values[static_cast<size_t>(SceneValue::Up)];
			if (lighting == nullptr || !ToViewSpace(view, worldUp, up) ||
			    !TowardsSun(view, lighting, kLightingShadowDirection, sun) ||
			    !TowardsSun(view, lighting, kLightingSunDirection, light)) {
				memset(sun, 0, sizeof(values[0]) * 3);
				return;
			}
			memcpy(&sun[3], lighting + kLightingTimeOfDay, sizeof(float));
			light[3] = lighting[kLightingIsNight] != 0 ? 0.0f : 1.0f;
			up[3] = 1.0f;
		}

		bool __fastcall DrawHook(void *self, void *edx) {
			if (gDriver != nullptr) ReplaceStaticShadows(self);
			bool const drawn = gOriginalDraw(self, edx);
			if (drawn && gDriver != nullptr) gDriver->RenderSceneEffects(self);
			return drawn;
		}

		void OnReloadedEffects(reshade::api::effect_runtime *runtime) {
			char value[32]{};
			float const farPlane = runtime->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", value)
				                       ? std::strtof(value, nullptr)
				                       : 1000.0f;
			gFarPlane = farPlane >= 1.0f ? farPlane : 1000.0f;
			// Also raised right after effects are destroyed, which is when old handles must go.
			gSceneUniforms.clear();
			gShadowTechniques.clear();
			runtime->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime *runtime,
			                                                 reshade::api::effect_uniform_variable variable) {
				static constexpr struct {
					char const *source;
					SceneValue value;
				} kSources[]{
					{"scd3d11_ortho", SceneValue::Ortho}, {"scd3d11_sun", SceneValue::Sun},
					{"scd3d11_light", SceneValue::Light}, {"scd3d11_up", SceneValue::Up}
				};
				char source[32]{};
				if (!runtime->get_annotation_string_from_uniform_variable(variable, "source", source)) return;
				if (std::strcmp(source, "bufready_depth") == 0) runtime->set_uniform_value_bool(variable, true);
				for (auto const &known: kSources) {
					if (std::strcmp(source, known.source) == 0) gSceneUniforms.push_back({variable, known.value});
				}
			});
			runtime->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *runtime,
			                                          reshade::api::effect_technique technique) {
				bool replaces = false;
				if (runtime->get_annotation_bool_from_technique(technique, "scd3d11_replaces_shadows", &replaces, 1) &&
				    replaces) {
					gShadowTechniques.push_back(technique);
				}
			});
		}

		void OnInitEffectRuntime(reshade::api::effect_runtime *runtime) {
			gRuntime = runtime;
			OnReloadedEffects(runtime);
			Log(LogCategory::Initialization, "reshade: effect runtime attached");
		}

		void OnDestroyEffectRuntime(reshade::api::effect_runtime *runtime) {
			if (runtime != gRuntime) return;
			gRuntime = nullptr;
			gSceneUniforms.clear();
			gShadowTechniques.clear();
		}

		// Runs inside render_effects, after ReShade's own Generic Depth add-on picked its depth buffer.
		void OnBeginEffects(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
		                    reshade::api::resource_view, reshade::api::resource_view) {
			if (gSceneDepthView == nullptr) return;
			runtime->update_texture_bindings("DEPTH", ViewHandle(gSceneDepthView), ViewHandle(gSceneDepthView));
			gDepthBound = true;
		}

		bool PatchDrawSlot(void *replacement, void *expected) {
			auto *const slot = reinterpret_cast<void **>(Rebase(kDrawSlotVA));
			DWORD protection = 0;
			if (*slot != expected || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return false;
			*slot = replacement;
			VirtualProtect(slot, sizeof(*slot), protection, &protection);
			return true;
		}

		bool EntryMatches(uintptr_t function, uint8_t const *expected) {
			auto const *const entry = reinterpret_cast<uint8_t const *>(Rebase(function));
			if (memcmp(entry, expected, kShadowEntryLength) == 0) return true;
			Log(LogCategory::Initialization, "reshade: 0x%08X starts %02X %02X %02X %02X %02X %02X, expected %02X %02X %02X %02X %02X %02X",
			    static_cast<unsigned>(function), entry[0], entry[1], entry[2], entry[3], entry[4], entry[5],
			    expected[0], expected[1], expected[2], expected[3], expected[4], expected[5]);
			return false;
		}

		void JumpFromEntry(uintptr_t function, void *stub) {
			auto *const entry = reinterpret_cast<uint8_t *>(Rebase(function));
			int32_t const displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (Rebase(function) + 5));
			DWORD protection = 0;
			VirtualProtect(entry, kShadowEntryLength, PAGE_EXECUTE_READWRITE, &protection);
			entry[0] = 0xE9;
			memcpy(entry + 1, &displacement, sizeof(displacement));
			entry[5] = 0x90;
			VirtualProtect(entry, kShadowEntryLength, protection, &protection);
			FlushInstructionCache(GetCurrentProcess(), entry, kShadowEntryLength);
		}
	}

	void cGDriver::InstallReShadeAddon(void) {
		static bool attempted = false;
		if (attempted) {
			if (gOriginalDraw != nullptr) gDriver = this;
			return;
		}
		attempted = true;
		if (std::strstr(GetCommandLineA(), "-ReShade:off") != nullptr) return;

		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        reinterpret_cast<LPCWSTR>(&DrawHook), &module) ||
		    !reshade::register_addon(module)) {
			return; // ReShade is not loaded, or is older than 6.0
		}

		auto const draw = reinterpret_cast<void *>(Rebase(kDrawVA));
		if (!PatchDrawSlot(reinterpret_cast<void *>(&DrawHook), draw)) {
			Log(LogCategory::Initialization, "reshade: cSC43DRender::Draw not found, using ReShade's default rendering");
			reshade::unregister_addon(module);
			return;
		}
		gOriginalDraw = reinterpret_cast<DrawFunction>(draw);
		gDriver = this;
		if (EntryMatches(kDrawShadowsVA, kDrawShadowsEntry) && EntryMatches(kDrawShadowsRoughVA, kDrawShadowsRoughEntry)) {
			gDrawShadowsBody = Rebase(kDrawShadowsVA) + kShadowEntryLength;
			gDrawShadowsRoughBody = Rebase(kDrawShadowsRoughVA) + kShadowEntryLength;
			JumpFromEntry(kDrawShadowsVA, reinterpret_cast<void *>(&DrawShadowsEntryStub));
			JumpFromEntry(kDrawShadowsRoughVA, reinterpret_cast<void *>(&DrawShadowsRoughEntryStub));
			gShadowHooksInstalled = true;
		} else {
			Log(LogCategory::Initialization, "reshade: shadow decal functions already modified, SC4's shadows cannot be replaced");
		}

		reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
		Log(LogCategory::Initialization, "reshade: add-on registered, effects render before the UI");
	}

	void cGDriver::UninstallReShadeAddon(void) {
		// ponytail: the vtable hook stays for the process lifetime; it is inert without a driver.
		if (gDriver == this) gDriver = nullptr;
	}

	HRESULT cGDriver::UpdateSceneDepth(void) {
		if (!depthShaderView) return E_POINTER;
		SceneDepthPipeline &pipeline = sceneDepth;
		HRESULT result = S_OK;
		if (!pipeline.pixelShader) {
			Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
			Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
			result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "SCD3D11SceneDepth", nullptr, nullptr,
			                    "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexBytecode, nullptr);
			if (SUCCEEDED(result)) {
				result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "SCD3D11SceneDepth", nullptr, nullptr,
				                    "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelBytecode, nullptr);
			}
			if (SUCCEEDED(result)) {
				result = d3dDevice->CreateVertexShader(vertexBytecode->GetBufferPointer(),
				                                       vertexBytecode->GetBufferSize(), nullptr, &pipeline.vertexShader);
			}
			if (SUCCEEDED(result)) {
				result = d3dDevice->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
				                                      nullptr, &pipeline.pixelShader);
			}
			if (SUCCEEDED(result)) {
				D3D11_BUFFER_DESC const description{
					sizeof(float) * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0
				};
				result = d3dDevice->CreateBuffer(&description, nullptr, &pipeline.constants);
			}
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "reshade scene depth pipeline", result);
				pipeline = SceneDepthPipeline{};
				return result;
			}
		}

		D3D11_TEXTURE2D_DESC current{};
		if (pipeline.texture) pipeline.texture->GetDesc(&current);
		if (!pipeline.texture || current.Width != static_cast<UINT>(windowWidth) ||
		    current.Height != static_cast<UINT>(windowHeight)) {
			pipeline.view.Reset();
			pipeline.target.Reset();
			pipeline.texture.Reset();
			D3D11_TEXTURE2D_DESC description{};
			description.Width = static_cast<UINT>(windowWidth);
			description.Height = static_cast<UINT>(windowHeight);
			description.MipLevels = 1;
			description.ArraySize = 1;
			description.Format = DXGI_FORMAT_R32_FLOAT;
			description.SampleDesc.Count = 1;
			description.Usage = D3D11_USAGE_DEFAULT;
			description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			result = d3dDevice->CreateTexture2D(&description, nullptr, &pipeline.texture);
			if (SUCCEEDED(result)) result = d3dDevice->CreateRenderTargetView(pipeline.texture.Get(), nullptr, &pipeline.target);
			if (SUCCEEDED(result)) result = d3dDevice->CreateShaderResourceView(pipeline.texture.Get(), nullptr, &pipeline.view);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "reshade scene depth texture", result);
				pipeline.view.Reset();
				pipeline.target.Reset();
				pipeline.texture.Reset();
				return result;
			}
		}

		if (pipeline.encodedFarPlane != gFarPlane) {
			float const constants[4]{gFarPlane, 0.0f, 0.0f, 0.0f};
			d3dContext->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, constants, 0, 0);
			pipeline.encodedFarPlane = gFarPlane;
		}

		// The depth buffer cannot be sampled while it is bound as the output.
		ID3D11RenderTargetView *const target = pipeline.target.Get();
		d3dContext->OMSetRenderTargets(1, &target, nullptr);
		D3D11_VIEWPORT const viewport{
			0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, 1.0f
		};
		d3dContext->RSSetViewports(1, &viewport);
		d3dContext->RSSetState(nullptr);
		d3dContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		d3dContext->OMSetDepthStencilState(nullptr, 0);
		d3dContext->IASetInputLayout(nullptr);
		d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		d3dContext->VSSetShader(pipeline.vertexShader.Get(), nullptr, 0);
		d3dContext->PSSetShader(pipeline.pixelShader.Get(), nullptr, 0);
		ID3D11Buffer *const constants = pipeline.constants.Get();
		d3dContext->PSSetConstantBuffers(0, 1, &constants);
		ID3D11ShaderResourceView *depth = depthShaderView.Get();
		d3dContext->PSSetShaderResources(0, 1, &depth);
		d3dContext->Draw(3, 0);
		depth = nullptr;
		d3dContext->PSSetShaderResources(0, 1, &depth);
		return S_OK;
	}

	void cGDriver::RenderSceneEffects(void *render) {
		if (gRuntime == nullptr || !IsDeviceReady()) return;

		gSceneDepthView = SUCCEEDED(UpdateSceneDepth()) ? sceneDepth.view.Get() : nullptr;
		float values[static_cast<size_t>(SceneValue::Count)][4]{
			{sceneProjection[0], sceneProjection[5], sceneProjection[10], sceneProjection[14]}
		};
		if (!gSceneUniforms.empty()) ReadSunlight(render, sceneView, values);
		for (SceneUniform const &uniform: gSceneUniforms) {
			gRuntime->set_uniform_value_float(uniform.variable, values[static_cast<size_t>(uniform.value)], 4);
		}
		// ReShade binds its own render targets for the effects, so the depth buffer is not an output meanwhile.
		gRuntime->update_texture_bindings("SC4_DEPTH", ViewHandle(depthShaderView.Get()), ViewHandle(depthShaderView.Get()));
		reshade::api::command_list *const commands = gRuntime->get_command_queue()->get_immediate_command_list();
		gRuntime->render_effects(commands, ViewHandle(renderTargetView.Get()), ViewHandle(renderTargetViewSrgb.Get()));
		gRuntime->update_texture_bindings("SC4_DEPTH", reshade::api::resource_view{0}, reshade::api::resource_view{0});
		// Effects ReShade renders at Present outside the city view must not measure with this camera.
		for (SceneUniform const &uniform: gSceneUniforms) {
			gRuntime->set_uniform_value_float(uniform.variable, 0.0f, 0.0f, 0.0f, 0.0f);
		}
		if (gDepthBound) {
			// Never leave ReShade holding a view that a resize or device loss may release.
			gRuntime->update_texture_bindings("DEPTH", reshade::api::resource_view{0}, reshade::api::resource_view{0});
			gDepthBound = false;
		}
		gSceneDepthView = nullptr;
		reshadeEffectsInBackBuffer = reshadeEffectsThisFrame = true;

		// The depth pass and the effects touched state SCD3D11 caches; start from a clean slate as Flush does.
		d3dContext->ClearState();
		InvalidateD3D11StateCache();
		ID3D11RenderTargetView *const renderTarget = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &renderTarget, depthStencilView.Get());
		if (scissorEnabled) SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
		else SetViewport();
	}

	void cGDriver::FinishReShadeFrame(void) {
		// A null target only marks the frame's effects as rendered, so Present does not stack another pass
		// onto a back buffer that still holds the effects of the last city view redraw.
		if (gRuntime != nullptr && reshadeEffectsInBackBuffer && !reshadeEffectsThisFrame) {
			gRuntime->render_effects(gRuntime->get_command_queue()->get_immediate_command_list(),
			                         reshade::api::resource_view{0}, reshade::api::resource_view{0});
		}
		reshadeEffectsThisFrame = false;
	}
}
