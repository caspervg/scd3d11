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
//    (cSC43DRender::UpdateCameraZoomAndRotationParams -> SetOrtho).
// Only effect runtime events and calls are used. ReShade's regular build (not only the "full add-on
// support" one) allows those for externally registered add-ons. Requires ReShade 6.0 or later.
// -ReShade:off on the command line leaves ReShade's default behaviour untouched.
//
// SC4 keeps its back buffer across Present and redraws only what changed, so effects rendered into it
// persist until the city view is redrawn. Frames that only redraw UI must not get effects again at
// Present; FinishReShadeFrame marks those as done.

#include "cGDriver.h"
#include "Diagnostics.h"
#include "NativeShadowMasks.h"

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
		// DrawPostStaticView is called after every static-view submission, including
		// dirty-rectangle updates, and before SC4 saves that image to its backing store.
		constexpr uintptr_t kDrawPostStaticViewVA = 0x007C3ED0;
		constexpr uintptr_t kDrawPostStaticViewRejoinVA = 0x007C3EDA;

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

		DrawFunction gOriginalDraw = nullptr;
		uintptr_t gDrawPostStaticViewRejoin = 0;
		cGDriver *gDriver = nullptr;
		reshade::api::effect_runtime *gRuntime = nullptr;
		ID3D11ShaderResourceView *gSceneDepthView = nullptr;
		bool gDepthBound = false;
		float gFarPlane = 1000.0f; // ReShade.fxh's default

		// Uniforms carrying a "source" annotation are never written by ReShade itself, so an add-on
		// owns them. Handles are resolved once per effect reload rather than per frame.
		struct ShadowUniformBindings {
			std::vector<reshade::api::effect_uniform_variable> valid;
			std::vector<reshade::api::effect_uniform_variable> depthScale;
			std::vector<reshade::api::effect_uniform_variable> worldPerScreen;

			void Clear(void) {
				valid.clear();
				depthScale.clear();
				worldPerScreen.clear();
			}

			bool Any(void) const {
				return !valid.empty() || !depthScale.empty() || !worldPerScreen.empty();
			}
		};

		ShadowUniformBindings gShadowBindings;

		reshade::api::resource_view ViewHandle(ID3D11View *view) {
			return reshade::api::resource_view{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view))};
		}

		bool __fastcall DrawHook(void *self, void *edx) {
			// AttemptTranslatedViewUpdate shifts the existing backing-store pixels and
			// redraws only the newly exposed strips. That dirty region knows the prop's
			// original footprint, but not LiveShadows' displaced footprint, so baked
			// shadows otherwise leave trails and get blended repeatedly. Invalidate only
			// translated updates while native network or prop shadows are enabled; SC4
			// then takes its own established full-redraw path and builds a clean backing
			// store at the new view.
			if (self != nullptr && NativeShadowMasks::RequiresCleanTranslatedRedraw()) {
				auto *const bytes = static_cast<uint8_t *>(self);
				uint32_t const horizontal = *reinterpret_cast<uint32_t const *>(bytes + 0xE0);
				uint32_t const vertical = *reinterpret_cast<uint32_t const *>(bytes + 0xE4);
				bool translated = horizontal != 0 || vertical != 0;
				// A dirty-rectangle update that ran last draw cannot have
				// produced correct shadows, whether or not the view also moved,
				// so it asks for the same clean rebuild a translation does.
				bool const partialShadowPass =
					gDriver != nullptr && gDriver->ConsumeLiveShadowCleanRedraw();

				// cSC43DRender+0x8c owns cSC4CameraControl. Its current zoom and
				// rotation are the integers at +0x108/+0x10c, written by
				// cSC4CameraControl::SetZoomAndRotation before Draw is entered.
				// Detect them here, before RestoreBackingStore can reuse pixels made
				// with the previous projection.
				static void *lastRender = nullptr;
				static int32_t lastZoom = 0;
				static int32_t lastRotation = 0;
				static bool haveCameraState = false;
				auto *const camera = *reinterpret_cast<uint8_t **>(bytes + 0x8C);
				bool cameraChanged = false;
				if (camera != nullptr) {
					int32_t const zoom = *reinterpret_cast<int32_t const *>(camera + 0x108);
					int32_t const rotation = *reinterpret_cast<int32_t const *>(camera + 0x10C);
					cameraChanged = haveCameraState && lastRender == self &&
					                (zoom != lastZoom || rotation != lastRotation);
					lastRender = self;
					lastZoom = zoom;
					lastRotation = rotation;
					haveCameraState = true;
				}

				if (translated || cameraChanged || partialShadowPass) {
					bytes[0x65] = 0; // cSC43DRender::mbBackingStoreValid
					static bool loggedTranslationInvalidation = false, loggedCameraInvalidation = false;
					if (translated && !loggedTranslationInvalidation) {
						loggedTranslationInvalidation = true;
						Log(LogCategory::Initialization,
						    "native shadows: translated backing-store updates force a clean static redraw");
					}
					if (cameraChanged && !loggedCameraInvalidation) {
						loggedCameraInvalidation = true;
						Log(LogCategory::Initialization,
						    "native shadows: zoom/rotation changes force a clean static redraw");
					}
				}
			}
			bool const drawn = gOriginalDraw(self, edx);
			if (drawn && gDriver != nullptr) gDriver->RenderSceneEffects();
			return drawn;
		}

#if defined(_MSC_VER) && defined(_M_IX86)
		__declspec(naked) void __fastcall DrawPostStaticViewOriginal(void *, void *) {
			__asm {
				push esi
				push edi
				mov  edi, ecx
				mov  eax, dword ptr [edi + 0x11c]
				jmp  dword ptr [gDrawPostStaticViewRejoin]
			}
		}

		void __fastcall DrawPostStaticViewHook(void *self, void *edx) {
			DrawPostStaticViewOriginal(self, edx);
			if (gDriver != nullptr) gDriver->RenderLivePropShadows();
		}
#endif

		void OnReloadedEffects(reshade::api::effect_runtime *runtime) {
			char value[32]{};
			float const farPlane = runtime->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", value)
				                       ? std::strtof(value, nullptr)
				                       : 1000.0f;
			gFarPlane = farPlane >= 1.0f ? farPlane : 1000.0f;
			gShadowBindings.Clear();
			runtime->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime *runtime,
			                                                 reshade::api::effect_uniform_variable variable) {
				char source[32]{};
				if (!runtime->get_annotation_string_from_uniform_variable(variable, "source", source)) return;
				if (std::strcmp(source, "bufready_depth") == 0) runtime->set_uniform_value_bool(variable, true);
				else if (std::strcmp(source, "sc4_sun_valid") == 0) gShadowBindings.valid.push_back(variable);
				else if (std::strcmp(source, "sc4_depth_scale") == 0) gShadowBindings.depthScale.push_back(variable);
				else if (std::strcmp(source, "sc4_world_per_screen_height") == 0) gShadowBindings.worldPerScreen.push_back(variable);
			});
		}

		// Pushes the snapshotted projection parameters into every effect that asked for them. Effects that do
		// not declare the uniforms cost nothing here.
		void PublishShadowUniforms(reshade::api::effect_runtime *runtime, bool valid, float depthScale,
		                           float worldPerScreenHeight) {
			if (!gShadowBindings.Any()) return;
			for (auto const variable : gShadowBindings.valid) runtime->set_uniform_value_bool(variable, valid);
			if (!valid) return;
			for (auto const variable : gShadowBindings.depthScale) runtime->set_uniform_value_float(variable, depthScale);
			for (auto const variable : gShadowBindings.worldPerScreen)
				runtime->set_uniform_value_float(variable, worldPerScreenHeight);
		}

		void OnInitEffectRuntime(reshade::api::effect_runtime *runtime) {
			gRuntime = runtime;
			OnReloadedEffects(runtime);
			Log(LogCategory::Initialization, "reshade: effect runtime attached");
		}

		void OnDestroyEffectRuntime(reshade::api::effect_runtime *runtime) {
			if (runtime == gRuntime) gRuntime = nullptr;
		}

		// Runs inside render_effects, after ReShade's own Generic Depth add-on picked its depth buffer.
		void OnBeginEffects(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
		                    reshade::api::resource_view, reshade::api::resource_view) {
			if (gSceneDepthView == nullptr) return;
			runtime->update_texture_bindings("DEPTH", ViewHandle(gSceneDepthView), ViewHandle(gSceneDepthView));
			gDepthBound = true;
		}

		bool PatchDrawSlot(void *replacement, void *expected) {
			auto *const slot = reinterpret_cast<void **>(
				reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + (kDrawSlotVA - kImageBase));
			DWORD protection = 0;
			if (*slot != expected || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return false;
			*slot = replacement;
			VirtualProtect(slot, sizeof(*slot), protection, &protection);
			return true;
		}

		bool PatchDrawPostStaticView(void *module) {
#if defined(_MSC_VER) && defined(_M_IX86)
			auto *const target = static_cast<uint8_t *>(module) + (kDrawPostStaticViewVA - kImageBase);
			uint8_t const expected[]{0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0x1C, 0x01, 0x00, 0x00};
			if (std::memcmp(target, expected, sizeof(expected)) != 0) return false;
			uint8_t replacement[sizeof(expected)]{};
			std::memset(replacement, 0x90, sizeof(replacement));
			replacement[0] = 0xE9;
			int32_t const displacement = static_cast<int32_t>(
				reinterpret_cast<uintptr_t>(&DrawPostStaticViewHook) - (reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(replacement + 1, &displacement, sizeof(displacement));
			gDrawPostStaticViewRejoin = reinterpret_cast<uintptr_t>(target + sizeof(expected));
			DWORD protection = 0;
			if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE, &protection)) return false;
			std::memcpy(target, replacement, sizeof(replacement));
			DWORD ignored = 0;
			VirtualProtect(target, sizeof(expected), protection, &ignored);
			FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
			return true;
#else
			(void)module;
			return false;
#endif
		}
	}

	void cGDriver::InstallReShadeAddon(void) {
		static bool attempted = false;
		if (attempted) {
			if (gOriginalDraw != nullptr) gDriver = this;
			return;
		}
		attempted = true;
		void *const executable = GetModuleHandleW(nullptr);
		auto const draw = reinterpret_cast<void *>(
			reinterpret_cast<uintptr_t>(executable) + (kDrawVA - kImageBase));
		if (!PatchDrawSlot(reinterpret_cast<void *>(&DrawHook), draw)) {
			Log(LogCategory::Initialization, "scene effects: cSC43DRender::Draw not found");
			return;
		}
		gOriginalDraw = reinterpret_cast<DrawFunction>(draw);
		gDriver = this;
		if (!PatchDrawPostStaticView(executable)) {
			Log(LogCategory::Initialization,
			    "live shadows: DrawPostStaticView byte guard failed; using end-of-scene fallback");
		} else {
			Log(LogCategory::Initialization,
			    "live shadows: static pass hooked before SC4 backing-store save");
		}
		if (std::strstr(GetCommandLineA(), "-ReShade:off") != nullptr) return;

		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        reinterpret_cast<LPCWSTR>(&DrawHook), &module) ||
		    !reshade::register_addon(module)) {
			return; // ReShade is not loaded, or is older than 6.0
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

	// Derives world-space depth and screen scales from the orthographic projection used by the last
	// scene draw. The shadow effect still supplies the sun direction and slope, but these scales keep
	// that slope and all depth thresholds stable across zoom levels.
	void cGDriver::CaptureShadowUniforms(void) {
		// OpenGL column-major, m[column * 4 + row]. For SC4's orthographic projection
		// p[5] = 2 / (top - bottom) and p[10] = -2 / (far - near).
		float const *const p = matrices[PROJECTION];

		// A perspective matrix has a non-zero w row; anything degenerate is not a usable ortho either.
		bool const orthographic = p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f;
		if (!orthographic || std::fabs(p[5]) < 1e-12f || std::fabs(p[10]) < 1e-12f) {
			shadowUniforms.valid = false;
			return;
		}

		// SC4 supplies a GL-convention projection (z in -1..1) which the vertex shader remaps to D3D's
		// 0..1 with (z + w) * 0.5, so the 0..1 depth range spans (far - near) = 2 / |p[10]| world units.
		// Multiplying buffer depth by that returns world units, which lets an effect express its
		// thresholds in metres and keeps them valid at every zoom level.
		shadowUniforms.depthScale = 2.0f / std::fabs(p[10]);

		// World units spanned by the viewport height. A ray march expressed as a slope (rise over run)
		// can then be converted to world units without knowing the zoom.
		shadowUniforms.worldPerScreenHeight = 2.0f / std::fabs(p[5]);
		shadowUniforms.valid = true;
	}

	void cGDriver::RenderSceneEffects(void) {
		RenderLivePropShadows();
		if (gRuntime == nullptr || !IsDeviceReady()) return;

		gSceneDepthView = SUCCEEDED(UpdateSceneDepth()) ? sceneDepth.view.Get() : nullptr;
		PublishShadowUniforms(gRuntime, shadowUniforms.valid, shadowUniforms.depthScale,
		                      shadowUniforms.worldPerScreenHeight);
		static bool loggedValid = false;
		static bool loggedInvalid = false;
		if (shadowUniforms.valid && !loggedValid) {
			loggedValid = true;
			Log(LogCategory::Initialization,
			    "reshade: shadow uniforms published (depth scale %.4f, world per screen height %.4f, %u bindings)",
			    shadowUniforms.depthScale, shadowUniforms.worldPerScreenHeight,
			    static_cast<unsigned>(gShadowBindings.depthScale.size() + gShadowBindings.worldPerScreen.size()));
		} else if (!shadowUniforms.valid && !loggedInvalid) {
			loggedInvalid = true;
			Log(LogCategory::Initialization,
			    "reshade: no orthographic projection captured yet; shadow uniforms stay unset");
		}
		reshade::api::command_list *const commands = gRuntime->get_command_queue()->get_immediate_command_list();
		gRuntime->render_effects(commands, ViewHandle(renderTargetView.Get()), ViewHandle(renderTargetViewSrgb.Get()));
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
