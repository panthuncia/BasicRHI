#include "rhi_dx12.h"
#include <atlbase.h>
#include <d3d12sdklayers.h>   // ID3D12DebugDevice + D3D12_RLDO_*
#include <initguid.h>   // defines INITGUID
#include <dxgidebug.h>  // DXGI live object reporting
#include <debugapi.h>   // OutputDebugStringA

#if BASICRHI_ENABLE_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_security.h>
#endif
#if BASICRHI_ENABLE_RESHAPE
#include <Message/MessageStream.h>
#include <Backends/DX12/Layer.h>
#include <Backend/Environment.h>
#include <Backend/EnvironmentInfo.h>
#include <Backend/StartupContainer.h>
#include <Bridge/IBridge.h>
#include <Bridge/IBridgeListener.h>
#include <Bridge/Log/LogSeverity.h>
#include <Common/ComRef.h>
#include <Message/IMessageStorage.h>
#include <Schemas/Config.h>
#include <Schemas/Diagnostic.h>
#include <Schemas/Feature.h>
#include <Schemas/Instrumentation.h>
#include <Schemas/Log.h>
#include <Schemas/PipelineMetadata.h>
#include <Schemas/SGUID.h>
#include <Schemas/ShaderMetadata.h>
#include <Schemas/Features/Descriptor.h>
#include <Schemas/Features/ResourceBounds.h>
#endif
#include <string>
#include <algorithm>
#include <cctype>
#include <deque>
#include <cstring>
#include <charconv>
#include <new>

#include "rhi_dx12_casting.h"
#include "rhi_debug_internal.h"


#define VERIFY(expr) if (FAILED(expr)) { spdlog::error("Validation error!"); }

namespace rhi {
	namespace {
		static void Dx12PollReShapeMessages(Dx12Device* impl) noexcept;
		static Result Dx12RefreshReShapeFeatures(Dx12Device* impl) noexcept;
		static void Dx12EnsureReShapeFeatureList(Dx12Device* impl) noexcept;
		static bool Dx12InitializeReShapeRuntime(Dx12Device* impl, const DeviceCreateInfo& ci) noexcept;
		static void Dx12ShutdownReShapeRuntime(Dx12Device* impl) noexcept;
	}
	#if BASICRHI_ENABLE_RESHAPE
	namespace {
		struct Dx12ReShapeRuntime {
			HMODULE module{ nullptr };
			PFN_D3D12_CREATE_DEVICE_GPUOPEN createDeviceGPUOpen{ nullptr };
			::Backend::Environment environment;
			ComRef<IBridge> bridge{ nullptr };
			ComRef<IBridgeListener> bridgeTap{ nullptr };
			std::mutex capturedStreamMutex;
			std::deque<MessageStream> capturedStreams;
		};

		struct Dx12ReShapeBridgeTap final : IBridgeListener {
			explicit Dx12ReShapeBridgeTap(Dx12ReShapeRuntime* runtime) noexcept
				: runtime(runtime) {
				address = this;
			}

			void Handle(const MessageStream* streams, uint32_t count) override {
				if (!runtime || !streams || count == 0) {
					return;
				}

				std::lock_guard guard(runtime->capturedStreamMutex);
				for (uint32_t index = 0; index < count; ++index) {
					runtime->capturedStreams.push_back(streams[index]);
				}
			}

			Dx12ReShapeRuntime* runtime = nullptr;
		};

		static void Dx12BuildDefaultInstrumentationSpecialization(MessageStream& out) noexcept;

		static std::wstring Dx12GetExecutableDirectory() noexcept {
			wchar_t buffer[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
			if (!length || length >= MAX_PATH) {
				return {};
			}

			std::wstring path(buffer, length);
			const size_t separator = path.find_last_of(L"\\/");
			if (separator == std::wstring::npos) {
				return {};
			}

			path.resize(separator);
			return path;
		}

		static HMODULE Dx12LoadReShapeLayerModule() noexcept {
			const std::wstring executableDirectory = Dx12GetExecutableDirectory();
			if (executableDirectory.empty()) {
				return nullptr;
			}

			const std::wstring modulePath = executableDirectory + L"\\GRS.Backends.DX12.Layer.dll";
			return LoadLibraryW(modulePath.c_str());
		}
	}
	#endif

	// ---- DRED (Device Removed Extended Data) support ----

	static ID3D12Device* g_dredDevice = nullptr;

	static const char* BreadcrumbOpToString(D3D12_AUTO_BREADCRUMB_OP op) noexcept {
		switch (op) {
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                                      return "SetMarker";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:                                     return "BeginEvent";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                                       return "EndEvent";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:                                  return "DrawInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:                           return "DrawIndexedInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:                                return "ExecuteIndirect";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                                       return "Dispatch";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:                               return "CopyBufferRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:                              return "CopyTextureRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:                                   return "CopyResource";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTILES:                                      return "CopyTiles";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:                             return "ResolveSubresource";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:                          return "ClearRenderTargetView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:                       return "ClearUnorderedAccessView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:                          return "ClearDepthStencilView";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:                                return "ResourceBarrier";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:                                  return "ExecuteBundle";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                                        return "Present";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:                               return "ResolveQueryData";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:                                return "BeginSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:                                  return "EndSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME:                                    return "DecodeFrame";
		case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES:                                  return "ProcessFrames";
		case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT:                           return "AtomicCopyBufferUINT";
		case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64:                         return "AtomicCopyBufferUINT64";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION:                       return "ResolveSubresourceRegion";
		case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE:                           return "WriteBufferImmediate";
		case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME1:                                   return "DecodeFrame1";
		case D3D12_AUTO_BREADCRUMB_OP_SETPROTECTEDRESOURCESESSION:                    return "SetProtectedResourceSession";
		case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME2:                                   return "DecodeFrame2";
		case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES1:                                 return "ProcessFrames1";
		case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:            return "BuildRaytracingAccelerationStructure";
		case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO:return "EmitRaytracingAccelerationStructurePostbuildInfo";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE:             return "CopyRaytracingAccelerationStructure";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:                                   return "DispatchRays";
		case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND:                          return "InitializeMetaCommand";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND:                             return "ExecuteMetaCommand";
		case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION:                                 return "EstimateMotion";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP:                        return "ResolveMotionVectorHeap";
		case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1:                              return "SetPipelineState1";
		default:                                                                      return "Unknown";
		}
	}

	static const char* DredAllocationTypeToString(D3D12_DRED_ALLOCATION_TYPE type) noexcept {
		switch (type) {
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:       return "CommandQueue";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR:   return "CommandAllocator";
		case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:      return "PipelineState";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:        return "CommandList";
		case D3D12_DRED_ALLOCATION_TYPE_FENCE:               return "Fence";
		case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:     return "DescriptorHeap";
		case D3D12_DRED_ALLOCATION_TYPE_HEAP:                return "Heap";
		case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:          return "QueryHeap";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE:   return "CommandSignature";
		case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY:    return "PipelineLibrary";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER:       return "VideoDecoder";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_PROCESSOR:     return "VideoProcessor";
		case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:            return "Resource";
		case D3D12_DRED_ALLOCATION_TYPE_PASS:                return "Pass";
		case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSION:       return "CryptoSession";
		case D3D12_DRED_ALLOCATION_TYPE_CRYPTOSESSIONPOLICY: return "CryptoSessionPolicy";
		case D3D12_DRED_ALLOCATION_TYPE_PROTECTEDRESOURCESESSION: return "ProtectedResourceSession";
		case D3D12_DRED_ALLOCATION_TYPE_VIDEO_DECODER_HEAP:  return "VideoDecoderHeap";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL:        return "CommandPool";
		case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER:    return "CommandRecorder";
		case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT:        return "StateObject";
		case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND:         return "MetaCommand";
		default:                                             return "Unknown";
		}
	}
	static void LogDredData() noexcept {
		if (!g_dredDevice) return;

		HRESULT reason = g_dredDevice->GetDeviceRemovedReason();
		if (reason == S_OK) return; // device is fine

		spdlog::error("======== DEVICE REMOVED (reason 0x{:08X}) – DRED Report ========", static_cast<unsigned>(reason));

		ComPtr<ID3D12DeviceRemovedExtendedData1> pDred;
		if (FAILED(g_dredDevice->QueryInterface(IID_PPV_ARGS(&pDred)))) {
			spdlog::error("  Could not query ID3D12DeviceRemovedExtendedData1.");
			return;
		}

		// ---- Auto Breadcrumbs ----
		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbsOutput{};
		if (SUCCEEDED(pDred->GetAutoBreadcrumbsOutput1(&breadcrumbsOutput))) {
			const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbsOutput.pHeadAutoBreadcrumbNode;
			int nodeIdx = 0;
			while (node) {
				const char*    clName = node->pCommandListDebugNameA  ? node->pCommandListDebugNameA  : "<unnamed>";
				const char*    cqName = node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "<unnamed>";
				const UINT32   count  = node->BreadcrumbCount;
				const UINT32   last   = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;

				spdlog::error("  [Breadcrumb Node {}] CmdList='{}' Queue='{}' Ops={} LastCompleted={}",
					nodeIdx, clName, cqName, count, last);

				// Show the ops around the last completed breadcrumb
				if (node->pCommandHistory && count > 0) {
					UINT32 start = (last >= 3) ? last - 3 : 0;
					UINT32 end   = (last + 4 < count) ? last + 4 : count;
					for (UINT32 i = start; i < end; ++i) {
						const char* marker = (i == last) ? " <<< LAST COMPLETED" :
						                     (i == last + 1) ? " <<< LIKELY FAULTING OP" : "";
						spdlog::error("    [{:5}] {}{}", i, BreadcrumbOpToString(node->pCommandHistory[i]), marker);
					}
				}
				node = node->pNext;
				nodeIdx++;
			}
			if (nodeIdx == 0) spdlog::error("  No auto-breadcrumb nodes.");
		} else {
			spdlog::error("  GetAutoBreadcrumbsOutput1 failed.");
		}

		// ---- Page Fault ----
		D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFaultOutput{};
		if (SUCCEEDED(pDred->GetPageFaultAllocationOutput1(&pageFaultOutput))) {
			if (pageFaultOutput.PageFaultVA != 0) {
				spdlog::error("  GPU Page Fault VA: 0x{:016X}", pageFaultOutput.PageFaultVA);

				auto logAllocList = [](const char* label, const D3D12_DRED_ALLOCATION_NODE1* node) {
					if (!node) { spdlog::error("    {}: (none)", label); return; }
					spdlog::error("    {}:", label);
					while (node) {
						const char* name = node->ObjectNameA ? node->ObjectNameA : "<unnamed>";
						spdlog::error("      - '{}' ({})", name, DredAllocationTypeToString(node->AllocationType));
						node = node->pNext;
					}
				};

				logAllocList("Existing allocations matching fault VA", pageFaultOutput.pHeadExistingAllocationNode);
				logAllocList("Recently freed allocations matching fault VA", pageFaultOutput.pHeadRecentFreedAllocationNode);
			} else {
				spdlog::error("  No GPU page fault reported.");
			}
		} else {
			spdlog::error("  GetPageFaultAllocationOutput1 failed.");
		}

		spdlog::error("======== END DRED Report ========");
	}

	namespace {
		static void Dx12WaitQueueIdle(Dx12QueueState& q) noexcept
		{
			if (!q.pNativeQueue || !q.fence) return;

			const UINT64 v = ++q.value;
			(void)q.pNativeQueue->Signal(q.fence.Get(), v);

			if (q.fence->GetCompletedValue() < v) {
				const HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (!e) return; // best-effort teardown
				(void)q.fence->SetEventOnCompletion(v, e);
				(void)WaitForSingleObject(e, INFINITE);
				CloseHandle(e);
			}
		}

		static void Dx12ReportLiveObjects(ID3D12Device* device, const char* phase) noexcept
		{
			if (!device) return;

			Microsoft::WRL::ComPtr<ID3D12DebugDevice> dbgDev;
			if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dbgDev))))
			{
				if (phase)
				{
					OutputDebugStringA("\n====================================================\n");
					OutputDebugStringA(phase);
					OutputDebugStringA("\n====================================================\n");
				}

				// Disable break-on-severity
				ComPtr<ID3D12InfoQueue> iq; if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&iq)))) {
					D3D12_MESSAGE_ID blocked[] = {
						static_cast<D3D12_MESSAGE_ID>(1356),
						static_cast<D3D12_MESSAGE_ID>(1328),
						static_cast<D3D12_MESSAGE_ID>(1008)
					};
					D3D12_INFO_QUEUE_FILTER f{};
					f.DenyList.NumIDs = static_cast<UINT>(_countof(blocked));
					f.DenyList.pIDList = blocked;
					iq->AddStorageFilterEntries(&f);
					(void)iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, false);
					(void)iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
				}

				// IGNORE_INTERNAL reduces noise from runtime-owned objects.
				(void)dbgDev->ReportLiveDeviceObjects(
					D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL
				);
			}
		}

		static void Dx12LogInfoQueueMessagesSince(ID3D12Device* device, UINT64 startIndex, UINT64 maxMessages = 16) noexcept
		{
			if (!device) return;

			ComPtr<ID3D12InfoQueue> iq;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) {
				return;
			}

			const UINT64 endIndex = iq->GetNumStoredMessagesAllowedByRetrievalFilter();
			if (endIndex <= startIndex) {
				return;
			}

			UINT64 firstIndex = startIndex;
			if (endIndex - firstIndex > maxMessages) {
				firstIndex = endIndex - maxMessages;
			}

			for (UINT64 index = firstIndex; index < endIndex; ++index) {
				SIZE_T messageBytes = 0;
				if (FAILED(iq->GetMessage(index, nullptr, &messageBytes)) || messageBytes == 0) {
					continue;
				}

				std::vector<char> storage(messageBytes);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
				if (FAILED(iq->GetMessage(index, message, &messageBytes))) {
					continue;
				}

				spdlog::error(
					"D3D12 InfoQueue[{}]: severity={} category={} id={} {}",
					index,
					static_cast<uint32_t>(message->Severity),
					static_cast<uint32_t>(message->Category),
					static_cast<uint32_t>(message->ID),
					message->pDescription ? message->pDescription : "<no description>");
			}
		}

		static void DxgiReportLiveObjects(const char* phase) noexcept
		{
			Microsoft::WRL::ComPtr<IDXGIDebug1> dxgiDebug;
			if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
			{
				if (phase)
				{
					OutputDebugStringA("\n================ DXGI LIVE OBJECTS ================\n");
					OutputDebugStringA(phase);
					OutputDebugStringA("\n===================================================\n");
				}
				(void)dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_DETAIL);
			}
		}

		static Result d_createPipelineFromStream(Device* d,
			const PipelineStreamItem* items,
			uint32_t count,
			PipelinePtr& out) noexcept
		{
			auto* dimpl = static_cast<Dx12Device*>(d->impl);

			// Collect RHI subobjects
			ID3D12RootSignature* root = nullptr;
			D3D12_SHADER_BYTECODE cs{}, vs{}, ps{}, as{}, ms{};
			bool hasCS = false, hasGfx = false;

			auto rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
			auto blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
			auto depth = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
			D3D12_RT_FORMAT_ARRAY rtv{};
			rtv.NumRenderTargets = 0;
			DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
			DXGI_SAMPLE_DESC sample{ 1,0 };
			D3D12_PRIMITIVE_TOPOLOGY_TYPE primitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
			std::vector<D3D12_INPUT_ELEMENT_DESC> inputLayout;
			D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{ nullptr, 0 };

			bool hasRast = false, hasBlend = false, hasDepth = false, hasRTV = false, hasDSV = false, hasSample = false, hasInputLayout = false, hasPrimitiveTopology = false;

			for (uint32_t i = 0; i < count; i++) {
				switch (items[i].type) {
				case PsoSubobj::Layout: {
					auto& L = *static_cast<const SubobjLayout*>(items[i].data);
					auto* pl = dimpl->pipelineLayouts.get(L.layout);
					if (!pl || !pl->root) {
						spdlog::error("DX12 pipeline creation: invalid pipeline layout or missing native root signature");
						RHI_FAIL(Result::InvalidArgument);
					}
					root = pl->root.Get();
				} break;
				case PsoSubobj::Shader: {
					auto& S = *static_cast<const SubobjShader*>(items[i].data);
					D3D12_SHADER_BYTECODE bc{ S.bytecode.data, S.bytecode.size };
					switch (S.stage) {
					case ShaderStage::Compute: cs = bc; hasCS = true; break;
					case ShaderStage::Vertex: vs = bc; hasGfx = true; break;
					case ShaderStage::Pixel: ps = bc; hasGfx = true; break;
					case ShaderStage::Task: as = bc; hasGfx = true; break;
					case ShaderStage::Mesh: ms = bc; hasGfx = true; break;
					case ShaderStage::AllGraphics:
						spdlog::error("DX12 pipeline creation: invalid shader stage 'AllGraphics'");
						RHI_FAIL(Result::InvalidArgument);
						break;
					case ShaderStage::All:
						spdlog::error("DX12 pipeline creation: invalid shader stage 'All'");
						RHI_FAIL(Result::InvalidArgument);
						break;
					}
				} break;
				case PsoSubobj::Rasterizer: {
					hasRast = true;
					auto& R = *static_cast<const SubobjRaster*>(items[i].data);
					rast.FillMode = ToDX(R.rs.fill);
					rast.CullMode = ToDX(R.rs.cull);
					rast.FrontCounterClockwise = R.rs.frontCCW;
					rast.DepthBias = static_cast<int>(R.rs.depthBias);
					rast.DepthBiasClamp = R.rs.depthBiasClamp;
					rast.SlopeScaledDepthBias = R.rs.slopeScaledDepthBias;
				} break;
				case PsoSubobj::Blend: {
					hasBlend = true;
					auto& B = *static_cast<const SubobjBlend*>(items[i].data);
					blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
					blend.AlphaToCoverageEnable = B.bs.alphaToCoverage;
					blend.IndependentBlendEnable = B.bs.independentBlend;
					for (uint32_t a = 0; a < B.bs.numAttachments && a < 8; a++) {
						const auto& src = B.bs.attachments[a];
						auto& dst = blend.RenderTarget[a];
						dst.BlendEnable = src.enable;
						dst.RenderTargetWriteMask = src.writeMask;
						dst.BlendOp = ToDX(src.colorOp);
						dst.SrcBlend = ToDX(src.srcColor);
						dst.DestBlend = ToDX(src.dstColor);
						dst.BlendOpAlpha = ToDX(src.alphaOp);
						dst.SrcBlendAlpha = ToDX(src.srcAlpha);
						dst.DestBlendAlpha = ToDX(src.dstAlpha);
					}
				} break;
				case PsoSubobj::DepthStencil: {
					hasDepth = true;
					auto& D = *static_cast<const SubobjDepth*>(items[i].data);
					depth = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
					depth.DepthEnable = D.ds.depthEnable;
					depth.DepthWriteMask = D.ds.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
					depth.DepthFunc = ToDX(D.ds.depthFunc);
				} break;
				case PsoSubobj::RTVFormats: {
					hasRTV = true;
					auto& R = *static_cast<const SubobjRTVs*>(items[i].data);
					rtv.NumRenderTargets = R.rt.count;
					for (uint32_t k = 0; k < R.rt.count && k < 8; k++) rtv.RTFormats[k] = ToDxgi(R.rt.formats[k]);
				} break;
				case PsoSubobj::DSVFormat: {
					hasDSV = true;
					auto& Z = *static_cast<const SubobjDSV*>(items[i].data);
					dsv = ToDxgi(Z.dsv);
				} break;
				case PsoSubobj::Sample: {
					hasSample = true;
					auto& S = *static_cast<const SubobjSample*>(items[i].data);
					sample = { S.sd.count, S.sd.quality };
				} break;
				case PsoSubobj::InputLayout: {
					hasInputLayout = true;
					ToDx12InputLayout(static_cast<const SubobjInputLayout*>(items[i].data)->il, inputLayout);
					inputLayoutDesc = { inputLayout.data(), static_cast<uint32_t>(inputLayout.size()) };
				} break;
				case PsoSubobj::PrimitiveTopology: {
					hasPrimitiveTopology = true;
					auto& T = *static_cast<const SubobjPrimitiveTopology*>(items[i].data);
					primitiveTopologyType = ToDXTopologyType(T.pt);
				} break;
				case PsoSubobj::Flags: {
					break;
				}
				}
			}

			// Validate & decide kind
			if (hasCS && hasGfx) {
				spdlog::error("DX12 pipeline creation: cannot mix compute and graphics shaders in one PSO");
				RHI_FAIL(Result::InvalidArgument);
			}
			if (!hasCS && !hasGfx) {
				spdlog::error("DX12 pipeline creation: no shaders specified");
				RHI_FAIL(Result::InvalidArgument);
			}
			const bool isCompute = hasCS;

			PsoStreamBuilder sb;
			sb.push(SO_RootSignature{ .Value = root });

			if (hasCS) {
				sb.push(SO_CS{ .Value = cs });
			}
			else {
				if (!hasPrimitiveTopology) {
					primitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					spdlog::warn("DX12 graphics PSO created without explicit primitive topology; defaulting to TRIANGLE topology type");
				}
				sb.push(SO_PrimTopology{ .Value = primitiveTopologyType });
				if (as.pShaderBytecode) sb.push(SO_AS{ .Value = as });
				if (ms.pShaderBytecode) sb.push(SO_MS{ .Value = ms });
				if (vs.pShaderBytecode) sb.push(SO_VS{ .Value = vs });
				if (ps.pShaderBytecode) sb.push(SO_PS{ .Value = ps });

				if (hasRast) sb.push(SO_Rasterizer{ .Value = rast });
				if (hasBlend) sb.push(SO_Blend{ .Value = blend });
				if (hasDepth) sb.push(SO_DepthStencil{ .Value = depth });
				if (hasRTV) sb.push(SO_RtvFormats{ .Value = rtv });
				if (hasDSV) sb.push(SO_DsvFormat{ .Value = dsv });
				if (hasSample) sb.push(SO_SampleDesc{ .Value = sample });
				if (hasInputLayout) sb.push(SO_InputLayout{ .Value = inputLayoutDesc });
			}
			auto sd = sb.desc();

			UINT64 infoQueueStart = 0;
			ComPtr<ID3D12InfoQueue> infoQueue;
			if (SUCCEEDED(dimpl->pNativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue))) && infoQueue) {
				infoQueueStart = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
			}

			ComPtr<ID3D12PipelineState> pso;
			if (const auto hr = dimpl->pNativeDevice->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)); FAILED(hr)) {
				spdlog::error(
					"DX12 CreatePipelineState failed: hr=0x{:08X}, isCompute={}, hasCS={}, hasGfx={}, hasVS={}, hasPS={}, hasAS={}, hasMS={}, rootSet={}, topologyType={}, rtvCount={}, rtv0=0x{:X}, dsv=0x{:X}, sampleCount={}, sampleQuality={}, hasDepth={}, depthEnable={}, depthWrite={}, streamSize={}",
					static_cast<uint32_t>(hr),
					isCompute,
					hasCS,
					hasGfx,
					vs.pShaderBytecode != nullptr,
					ps.pShaderBytecode != nullptr,
					as.pShaderBytecode != nullptr,
					ms.pShaderBytecode != nullptr,
					root != nullptr,
					static_cast<uint32_t>(primitiveTopologyType),
					rtv.NumRenderTargets,
					static_cast<uint32_t>(rtv.RTFormats[0]),
					static_cast<uint32_t>(dsv),
					sample.Count,
					sample.Quality,
					hasDepth,
					depth.DepthEnable,
					depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL,
					sd.SizeInBytes);
				Dx12LogInfoQueueMessagesSince(dimpl->pNativeDevice.Get(), infoQueueStart);
				RHI_FAIL(ToRHI(hr));
			}

			auto handle = dimpl->pipelines.alloc(Dx12Pipeline(pso, isCompute, dimpl));
			Pipeline ret(handle);
			ret.vt = &g_psovt;
			ret.impl = dimpl;

			out = MakePipelinePtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyBuffer(DeviceDeletionContext* d, ResourceHandle h) noexcept
		{
			dx12_detail::Dev(d)->resources.free(h);
		}
		static void d_destroyTexture(DeviceDeletionContext* d, ResourceHandle h) noexcept
		{
			dx12_detail::Dev(d)->resources.free(h);
		}
		static void d_destroySampler(DeviceDeletionContext* d, SamplerHandle h) noexcept
		{
			dx12_detail::Dev(d)->samplers.free(h);
		}
		static void d_destroyPipeline(DeviceDeletionContext* d, PipelineHandle h) noexcept
		{
			dx12_detail::Dev(d)->pipelines.free(h);
		}

		// TOOD: Abstract out a bunch of this for use with DXR pipelines as well
		static Result d_createWorkGraph(Device* d, const WorkGraphDesc& desc, WorkGraphPtr& out) noexcept
		{
			if (!d) return Result::InvalidArgument;
			auto* dimpl = static_cast<Dx12Device*>(d->impl);
			if (!dimpl || !dimpl->pNativeDevice) return Result::InvalidArgument;
			if (!desc.programName || desc.programName[0] == '\0') return Result::InvalidArgument;
			if (desc.libraries.size == 0) return Result::InvalidArgument;

			if (desc.flags & WorkGraphFlags::WorkGraphFlagsEntrypointGraphicsNodesRasterizeInOrder) {
				// Not yet supported in d3d12.h
				spdlog::error("DX12 work graph creation: EntrypointGraphicsNodesRasterizeInOrder flag is not supported yet");
				return Result::InvalidArgument;
			}

			// Feature gate, should have already checked through RHI, but just in case
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS21 opt{};
				const HRESULT featureHr = dimpl->pNativeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &opt, sizeof(opt));
				if (FAILED(featureHr)) {
					spdlog::error(
						"DX12 work graph creation: CheckFeatureSupport(OPTIONS21) failed hr=0x{:08X}",
						static_cast<unsigned>(featureHr));
					return Result::Unsupported;
				}
				if (opt.WorkGraphsTier == D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED) {
					spdlog::error("DX12 work graph creation: WorkGraphsTier reports NOT_SUPPORTED for program '{}'", desc.programName);
					return Result::Unsupported;
				}
			}

			struct StringStore {
				std::deque<std::wstring> ws;
				LPCWSTR W(const char* s) {
					ws.emplace_back(s2ws(std::string(s ? s : "")));
					return ws.back().c_str();
				}
			};

			StringStore strings;

			CD3DX12_STATE_OBJECT_DESC soDesc(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);

			if (desc.allowStateObjectAdditions) {
				auto* cfg = soDesc.CreateSubobject<CD3DX12_STATE_OBJECT_CONFIG_SUBOBJECT>();
				cfg->SetFlags(D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS);
			}

			// DXIL libraries
			for (const ShaderLibraryDesc& lib : desc.libraries) {
				auto* libSub = soDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
				D3D12_SHADER_BYTECODE bc{ lib.dxil.data, lib.dxil.size };
				libSub->SetDXILLibrary(&bc);

				for (const ShaderExportDesc& ex : lib.exports) {
					if (!ex.name) continue;
					libSub->DefineExport(strings.W(ex.name), ex.exportToRename ? strings.W(ex.exportToRename) : nullptr);
				}
			}

			// Global root signature
			if (desc.globalRootSignature.valid()) {
				const auto* pl = dimpl->pipelineLayouts.get(desc.globalRootSignature);
				if (!pl || !pl->root) {
					spdlog::error("DX12 work graph creation: invalid global root signature handle for program '{}'", desc.programName);
					return Result::InvalidArgument;
				}
				auto* grs = soDesc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
				grs->SetRootSignature(pl->root.Get());
			}

			// Local root signature associations
			for (const LocalRootAssociation& assoc : desc.localRootAssociations) {
				const auto* pl = dimpl->pipelineLayouts.get(assoc.localRootSignature);
				if (!pl || !pl->root) {
					spdlog::error("DX12 work graph creation: invalid local root signature handle for program '{}'", desc.programName);
					return Result::InvalidArgument;
				}

				auto* lrs = soDesc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
				lrs->SetRootSignature(pl->root.Get());

				auto* assocSub = soDesc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
				assocSub->SetSubobjectToAssociate(*lrs);
				for (const char* s : assoc.exports) {
					if (!s) continue;
					assocSub->AddExport(strings.W(s));
				}
			}

			// Work graph
			auto* wg = soDesc.CreateSubobject<CD3DX12_WORK_GRAPH_SUBOBJECT>();
			LPCWSTR programNameW = strings.W(desc.programName);
			wg->SetProgramName(programNameW);
			if (desc.flags & WorkGraphFlags::WorkGraphFlagsIncludeAllAvailableNodes) {
				wg->IncludeAllAvailableNodes();
			}

			auto toNodeId = [&](const NodeIDDesc& id) {
				return D3D12_NODE_ID{ strings.W(id.name), id.arrayIndex };
				};

			auto applyOutputOverrides = [&](CD3DX12_NODE_OUTPUT_OVERRIDES& ooHelper, const Span<NodeOutputOverrideDesc>& overrides) {
				for (const NodeOutputOverrideDesc& oo : overrides) {
					ooHelper.NewOutputOverride();
					ooHelper.OutputIndex(oo.outputIndex);
					if (oo.newName.has_value()) {
						ooHelper.NewName(strings.W(oo.newName->name), oo.newName->arrayIndex);
					}
					if (oo.allowSparseNodes.has_value()) {
						ooHelper.AllowSparseNodes(*oo.allowSparseNodes ? TRUE : FALSE);
					}
					if (oo.maxRecords.has_value()) {
						ooHelper.MaxOutputRecords(*oo.maxRecords);
					}
					if (oo.maxRecordsSharedWithOutputIndex.has_value()) {
						ooHelper.MaxOutputRecordsSharedWith(*oo.maxRecordsSharedWithOutputIndex);
					}
				}
				};

			auto applyCommonOverrides = [&](auto* node, const CommonComputeNodeOverridesDesc& c) {
				if (c.localRootArgumentsTableIndex.has_value()) {
					node->LocalRootArgumentsTableIndex(static_cast<UINT>(*c.localRootArgumentsTableIndex));
				}
				if (c.isProgramEntry.has_value()) {
					node->ProgramEntry(*c.isProgramEntry ? TRUE : FALSE);
				}
				if (c.newName.has_value()) {
					node->NewName(toNodeId(*c.newName));
				}
				if (c.shareInputOf.has_value()) {
					node->ShareInputOf(toNodeId(*c.shareInputOf));
				}
				applyOutputOverrides(node->NodeOutputOverrides(), c.outputOverrides);
				};

			// Explicit nodes
			for (const NodeOverrideDesc& n : desc.explicitNodes) {
				if (!n.shaderExport || n.shaderExport[0] == '\0') continue;

				switch (n.overridesType) {
				case NodeOverridesType::BroadcastingLaunch: {
					auto* node = wg->CreateBroadcastingLaunchNodeOverrides(strings.W(n.shaderExport));
					applyCommonOverrides(node, n.broadcasting);
					if (n.broadcasting.dispatchGrid.has_value()) {
						const auto& g = *n.broadcasting.dispatchGrid;
						node->DispatchGrid(g.x, g.y, g.z);
					}
					if (n.broadcasting.maxDispatchGrid.has_value()) {
						const auto& g = *n.broadcasting.maxDispatchGrid;
						node->MaxDispatchGrid(g.x, g.y, g.z);
					}
				} break;

				case NodeOverridesType::CoalescingLaunch: {
					auto* node = wg->CreateCoalescingLaunchNodeOverrides(strings.W(n.shaderExport));
					applyCommonOverrides(node, n.coalescing);
				} break;

				case NodeOverridesType::ThreadLaunch: {
					auto* node = wg->CreateThreadLaunchNodeOverrides(strings.W(n.shaderExport));
					applyCommonOverrides(node, n.thread);
				} break;

				case NodeOverridesType::CommonCompute: {
					auto* node = wg->CreateCommonComputeNodeOverrides(strings.W(n.shaderExport));
					applyCommonOverrides(node, n.common);
				} break;

				case NodeOverridesType::None:
				default:
					wg->CreateShaderNode(strings.W(n.shaderExport));
					break;
				}
			}

			// Entry points
			for (const NodeIDDesc& e : desc.entrypoints) {
				wg->AddEntrypoint(toNodeId(e));
			}

			ComPtr<ID3D12StateObject> stateObject;
			if (const auto hr = dimpl->pNativeDevice->CreateStateObject(soDesc, IID_PPV_ARGS(&stateObject)); FAILED(hr)) {
				spdlog::error(
					"DX12 work graph creation: CreateStateObject failed for program '{}' hr=0x{:08X}",
					desc.programName,
					static_cast<unsigned>(hr));
				RHI_FAIL(ToRHI(hr));
			}

			ComPtr<ID3D12StateObjectProperties1> props1;
			if (const auto hr = stateObject.As(&props1); FAILED(hr) || !props1) {
				spdlog::error(
					"DX12 work graph creation: QueryInterface(ID3D12StateObjectProperties1) failed for program '{}' hr=0x{:08X} props1={}",
					desc.programName,
					static_cast<unsigned>(hr),
					static_cast<bool>(props1));
				RHI_FAIL(Result::Failed);
			}
			ComPtr<ID3D12WorkGraphProperties> wgProps;
			if (const auto hr = stateObject.As(&wgProps); FAILED(hr) || !wgProps) {
				spdlog::error(
					"DX12 work graph creation: QueryInterface(ID3D12WorkGraphProperties) failed for program '{}' hr=0x{:08X} wgProps={}",
					desc.programName,
					static_cast<unsigned>(hr),
					static_cast<bool>(wgProps));
				RHI_FAIL(Result::Failed);
			}

			const D3D12_PROGRAM_IDENTIFIER programId = props1->GetProgramIdentifier(programNameW);
			const UINT wgIndex = wgProps->GetWorkGraphIndex(programNameW);
			const UINT numEntries = wgProps->GetNumEntrypoints(wgIndex);
			if (numEntries == 0) {
				spdlog::error("DX12 work graph creation: no entrypoints found for program '{}'", desc.programName);
				RHI_FAIL(Result::InvalidArgument);
			}
			D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS mem{};
			wgProps->GetWorkGraphMemoryRequirements(wgIndex, &mem);

			auto handle = dimpl->workGraphs.alloc(Dx12WorkGraph(stateObject, props1, wgProps, programId, wgIndex, mem, dimpl));
			WorkGraph ret(handle);
			ret.vt = &g_wgvt;
			ret.impl = dimpl;
			out = MakeWorkGraphPtr(d, ret, dimpl->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyWorkGraph(DeviceDeletionContext* d, WorkGraphHandle h) noexcept
		{
			dx12_detail::Dev(d)->workGraphs.free(h);
		}

		static void d_destroyCommandList(DeviceDeletionContext* d, CommandList* p) noexcept {
			if (!d || !p || !p->IsValid()) {
				BreakIfDebugging();
				return;
			}
			auto* impl = dx12_detail::Dev(d);
			impl->commandLists.free(p->GetHandle());
			p->Reset();
		}

		static Queue d_getQueue(Device* d, QueueKind qk) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			QueueHandle h = (qk == QueueKind::Graphics ? impl->gfxHandle : qk == QueueKind::Compute ? impl->compHandle : impl->copyHandle);
			Queue out{ qk, h }; out.vt = &g_qvt;
			out.impl = impl;
			return out;
		}

		static Result d_createQueue(Device* d, QueueKind qk, const char* name, Queue& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);

			D3D12_COMMAND_LIST_TYPE type{};
			switch (qk) {
			case QueueKind::Graphics: type = D3D12_COMMAND_LIST_TYPE_DIRECT;  break;
			case QueueKind::Compute:  type = D3D12_COMMAND_LIST_TYPE_COMPUTE; break;
			case QueueKind::Copy:     type = D3D12_COMMAND_LIST_TYPE_COPY;    break;
			default: RHI_FAIL(Result::InvalidArgument);
			}

			Dx12QueueState qs{};
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = type;

			ComPtr<ID3D12CommandQueue> qProxy;
			HRESULT hr = impl->pSLProxyDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&qProxy));
			if (FAILED(hr)) { RHI_FAIL(ToRHI(hr)); }
			qs.pSLProxyQueue = qProxy;

			// Streamline: extract native queue
			if (impl->steamlineInitialized)
			{
			#if BASICRHI_ENABLE_STREAMLINE
				ID3D12CommandQueue* qNative = nullptr;
				if (SL_FAILED(res, slGetNativeInterface(qs.pSLProxyQueue.Get(), (void**)&qNative)))
					qs.pNativeQueue = qs.pSLProxyQueue;
				else
					qs.pNativeQueue.Attach(qNative);
			#else
				qs.pNativeQueue = qs.pSLProxyQueue;
			#endif
			}
			else
			{
				qs.pNativeQueue = qs.pSLProxyQueue;
			}

			qs.dev = impl;
			hr = impl->pNativeDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&qs.fence));
			if (FAILED(hr)) { RHI_FAIL(ToRHI(hr)); }
			qs.value = 0;

			if (name) {
				const int wLen = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
				if (wLen > 0) {
					std::vector<wchar_t> wname(wLen);
					MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), wLen);
					if (qs.pNativeQueue)  qs.pNativeQueue->SetName(wname.data());
					if (qs.pSLProxyQueue && qs.pSLProxyQueue.Get() != qs.pNativeQueue.Get())
						qs.pSLProxyQueue->SetName(wname.data());
				}
			}

			QueueHandle h = impl->queues.alloc(qs);
			out = Queue{ qk, h };
			out.vt = &g_qvt;
			out.impl = impl;
			return Result::Ok;
		}

		static void d_destroyQueue(DeviceDeletionContext* ctx, QueueHandle h) noexcept {
			auto* impl = dx12_detail::Dev(ctx);
			if (!impl) return;
			auto* qs = impl->queues.get(h);
			if (!qs) return;
			// Drain the queue before destroying it
			Dx12WaitQueueIdle(*qs);
			impl->queues.free(h);
		}

		static Result d_waitIdle(Device* d) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			for (auto& slot : impl->queues.slots) {
				if (slot.alive) Dx12WaitQueueIdle(slot.obj);
			}
			return Result::Ok;
		}
		static void   d_flushDeletionQueue(Device*) noexcept {}

		// Swapchain create/destroy
		static Result d_createSwapchain(Device* d, void* hwnd, const uint32_t w, const uint32_t h, const Format fmt, const uint32_t bufferCount, const bool allowTearing, SwapchainPtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.BufferCount = bufferCount;
			desc.Width = w;
			desc.Height = h;
			desc.Format = ToDxgi(fmt);
			desc.SampleDesc = { 1,0 };
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
			auto flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
			if (allowTearing) {
				flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
			}
			desc.Flags = flags;

			ComPtr<IDXGISwapChain1> sc1;
			auto* gfxState = impl->queues.get(impl->gfxHandle);
			if (const auto hr = impl->pSLProxyFactory->CreateSwapChainForHwnd(gfxState->pSLProxyQueue.Get(), static_cast<HWND>(hwnd), &desc, nullptr, nullptr, &sc1); FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}
			ComPtr<IDXGISwapChain3> proxySc3;
			if (const auto hr = sc1.As(&proxySc3); FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}

			ComPtr<IDXGISwapChain3> nativeSc3;
			if (impl->steamlineInitialized) {
				IDXGISwapChain3* nativeSc3Raw = nullptr;
				slGetNativeInterface(proxySc3.Get(), reinterpret_cast<void**>(&nativeSc3Raw));
				nativeSc3.Attach(nativeSc3Raw);
			}
			else {
				nativeSc3 = proxySc3;
			}
			if (!nativeSc3) {
				RHI_FAIL(Result::InvalidNativePointer);
			}

			std::vector<ComPtr<ID3D12Resource>> imgs(bufferCount);
			std::vector<ResourceHandle> imgHandles(bufferCount);

			for (UINT i = 0; i < bufferCount; i++) {
				auto hr = nativeSc3->GetBuffer(i, IID_PPV_ARGS(&imgs[i]));
				if (FAILED(hr)) {
					RHI_FAIL(ToRHI(hr));
				}
				// Register as a TextureHandle
				Dx12Resource t(imgs[i], desc.Format, w, h, 1, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, impl);
				imgHandles[i] = impl->resources.alloc(t);

			}

			auto scWrap = Dx12Swapchain(
				nativeSc3, proxySc3, desc.Format, w, h, bufferCount,
				desc.Flags,
				imgs, imgHandles,
				impl
			);

			auto scHandle = impl->swapchains.alloc(scWrap);

			Swapchain ret{ scHandle };
			ret.impl = impl;
			ret.vt = &g_scvt;
			out = MakeSwapchainPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroySwapchain(DeviceDeletionContext*, Swapchain* sc) noexcept {
			auto* s = dx12_detail::SC(sc);
			if (!s) { sc->impl = nullptr; sc->vt = nullptr; return; }

			// Ensure GPU is idle before ripping out backbuffers.
			if (s->dev && s->dev->self.vt && s->dev->self.vt->deviceWaitIdle) {
				(void)s->dev->self.vt->deviceWaitIdle(&s->dev->self);
			}

			if (s->dev)
			{
				// Release references held by the resource registry and free the handles.
				for (auto h : s->imageHandles)
				{
					if (h.generation != 0) // invalid guard
					{
						if (auto* r = s->dev->resources.get(h))
							r->res.Reset(); // drop ID3D12Resource ref so DXGI can actually destroy
						s->dev->resources.free(h);
					}
				}
				s->dev->swapchains.free(sc->GetHandle());
			}

			sc->impl = nullptr;
			sc->vt = nullptr;
		}

		static void d_destroyDevice(Device* d) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			impl->Shutdown();
			d->vt = nullptr;
		}

		static D3D12_SHADER_VISIBILITY ToDx12Vis(ShaderStage s) {
			switch (s) {
			case ShaderStage::Vertex: return D3D12_SHADER_VISIBILITY_VERTEX;
			case ShaderStage::Pixel:  return D3D12_SHADER_VISIBILITY_PIXEL;
			case ShaderStage::Mesh:  return D3D12_SHADER_VISIBILITY_MESH;
			case ShaderStage::Task:  return D3D12_SHADER_VISIBILITY_AMPLIFICATION;
			case ShaderStage::Compute: return D3D12_SHADER_VISIBILITY_ALL;
			default:                  return D3D12_SHADER_VISIBILITY_ALL;
			}
		}

		static bool Dx12UsesEmulatedRootConstants(const PushConstantRangeDesc& pc) noexcept {
			return pc.type == PushConstantRangeType::EmulatedRootConstants;
		}

		static size_t Dx12AlignUpSize(size_t value, size_t alignment) noexcept {
			return (value + (alignment - 1)) & ~(alignment - 1);
		}

		static bool Dx12EnsureRootCbvScratchPage(Dx12Device* impl, Dx12CommandList& list, size_t minSize) noexcept {
			constexpr size_t kDefaultPageSize = 64 * 1024;
			const size_t capacity = (std::max)(Dx12AlignUpSize(minSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), kDefaultPageSize);

			D3D12_HEAP_PROPERTIES hp{};
			hp.Type = D3D12_HEAP_TYPE_UPLOAD;
			hp.CreationNodeMask = 1;
			hp.VisibleNodeMask = 1;

			const D3D12_RESOURCE_DESC1 desc = MakeBufferDesc1(capacity, D3D12_RESOURCE_FLAG_NONE);

			Microsoft::WRL::ComPtr<ID3D12Resource> resource;
			const HRESULT hr = impl->pNativeDevice->CreateCommittedResource3(
				&hp,
				D3D12_HEAP_FLAG_NONE,
				&desc,
				D3D12_BARRIER_LAYOUT_UNDEFINED,
				nullptr,
				nullptr,
				0,
				nullptr,
				IID_PPV_ARGS(&resource));
			if (FAILED(hr) || !resource) {
				spdlog::error(
					"DX12 root CBV scratch allocation failed: hr=0x{:08X}, capacity={}",
					static_cast<uint32_t>(hr),
					capacity);
				return false;
			}

			void* mapped = nullptr;
			if (FAILED(resource->Map(0, nullptr, &mapped)) || !mapped) {
				spdlog::error("DX12 root CBV scratch map failed for capacity={}", capacity);
				return false;
			}

			Dx12CommandList::RootCbvScratchPage page{};
			page.gpuBase = resource->GetGPUVirtualAddress();
			page.capacity = capacity;
			page.cursor = 0;
			page.mapped = static_cast<uint8_t*>(mapped);
			page.resource = std::move(resource);
			list.rootCbvScratchPages.push_back(std::move(page));
			return true;
		}

		static bool Dx12AllocateRootCbvScratch(Dx12CommandList* list, size_t size, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddress, void*& cpuAddress) noexcept {
			if (!list || !list->dev || size == 0) {
				return false;
			}

			for (auto& page : list->rootCbvScratchPages) {
				const size_t alignedCursor = Dx12AlignUpSize(page.cursor, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				if (alignedCursor + size > page.capacity) {
					continue;
				}

				gpuAddress = page.gpuBase + alignedCursor;
				cpuAddress = page.mapped + alignedCursor;
				page.cursor = alignedCursor + size;
				return true;
			}

			if (!Dx12EnsureRootCbvScratchPage(list->dev, *list, size)) {
				return false;
			}

			auto& page = list->rootCbvScratchPages.back();
			gpuAddress = page.gpuBase;
			cpuAddress = page.mapped;
			page.cursor = size;
			return true;
		}

		static Dx12CommandList::RootCbvShadowState* Dx12GetRootCbvShadowState(
			Dx12CommandList* list,
			const Dx12PipelineLayout::RootConstParam& rc) noexcept {
			for (auto& state : list->rootCbvShadowStates) {
				if (state.set == rc.set && state.binding == rc.binding && state.rootIndex == rc.rootIndex) {
					if (state.values.size() != rc.num32) {
						state.values.assign(rc.num32, 0u);
					}
					return &state;
				}
			}

			Dx12CommandList::RootCbvShadowState state{};
			state.set = rc.set;
			state.binding = rc.binding;
			state.rootIndex = rc.rootIndex;
			state.values.assign(rc.num32, 0u);
			list->rootCbvShadowStates.push_back(std::move(state));
			return &list->rootCbvShadowStates.back();
		}

		static Result d_createPipelineLayout(Device* d, const PipelineLayoutDesc& ld, PipelineLayoutPtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);

			// Root parameters: push constants only (bindless tables omitted for brevity)
			std::vector<D3D12_ROOT_PARAMETER1> params;
			params.reserve(ld.pushConstants.size);
			std::vector<D3D12_ROOT_PARAMETER> paramsV10;
			paramsV10.reserve(ld.pushConstants.size);
			for (uint32_t i = 0; i < ld.pushConstants.size; ++i) {
				const auto& pc = ld.pushConstants.data[i];
				const bool useRootCbv = Dx12UsesEmulatedRootConstants(pc);
				D3D12_ROOT_PARAMETER1 p{};
				p.ParameterType = useRootCbv ? D3D12_ROOT_PARAMETER_TYPE_CBV : D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				if (useRootCbv) {
					p.Descriptor.ShaderRegister = pc.binding;
					p.Descriptor.RegisterSpace = pc.set;
					p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
				}
				else {
					p.Constants.Num32BitValues = pc.num32BitValues;
					p.Constants.ShaderRegister = pc.binding;   // binding -> ShaderRegister
					p.Constants.RegisterSpace = pc.set;       // set     -> RegisterSpace
				}
				p.ShaderVisibility = ToDx12Vis(pc.visibility);
				params.push_back(p);

				D3D12_ROOT_PARAMETER pV10{};
				pV10.ParameterType = useRootCbv ? D3D12_ROOT_PARAMETER_TYPE_CBV : D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				if (useRootCbv) {
					pV10.Descriptor.ShaderRegister = pc.binding;
					pV10.Descriptor.RegisterSpace = pc.set;
				}
				else {
					pV10.Constants.Num32BitValues = pc.num32BitValues;
					pV10.Constants.ShaderRegister = pc.binding;
					pV10.Constants.RegisterSpace = pc.set;
				}
				pV10.ShaderVisibility = ToDx12Vis(pc.visibility);
				paramsV10.push_back(pV10);
			}

			// Static samplers
			std::vector<D3D12_STATIC_SAMPLER_DESC> ssmps;
			ssmps.reserve(ld.staticSamplers.size);
			for (uint32_t i = 0; i < ld.staticSamplers.size; ++i) {
				const auto& ss = ld.staticSamplers.data[i];
				D3D12_STATIC_SAMPLER_DESC s{};
				s.Filter = BuildDxFilter(ss.sampler);
				s.AddressU = ToDX(ss.sampler.addressU);
				s.AddressV = ToDX(ss.sampler.addressV);
				s.AddressW = ToDX(ss.sampler.addressW);
				s.MipLODBias = ss.sampler.mipLodBias;
				s.MaxAnisotropy = ss.sampler.maxAnisotropy;
				s.ComparisonFunc = ss.sampler.compareEnable ? ToDX(ss.sampler.compareOp) : D3D12_COMPARISON_FUNC_NEVER;
				s.MinLOD = EffectiveDxMinLod(ss.sampler);
				s.MaxLOD = EffectiveDxMaxLod(ss.sampler);
				s.ShaderRegister = ss.binding; // binding -> ShaderRegister
				s.RegisterSpace = ss.set;     // set -> RegisterSpace
				s.ShaderVisibility = ToDx12Vis(ss.visibility);
				s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
				s.MaxAnisotropy = (ss.sampler.maxAnisotropy > 1) ? (std::min<uint32_t>(ss.sampler.maxAnisotropy, 16u)) : 1u;
				ssmps.push_back(s);
				// (arrayCount>1: add multiple entries or extend StaticSamplerDesc to carry per-binding arrays)
			}

			// Root signature flags
			D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
			rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			rs.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
			rs.Desc_1_1.pParameters = params.data();
			rs.Desc_1_1.NumStaticSamplers = static_cast<UINT>(ssmps.size());
			rs.Desc_1_1.pStaticSamplers = ssmps.data();
			rs.Desc_1_1.Flags =
				D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
				D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

			if (ld.flags & PipelineLayoutFlags::PF_AllowInputAssembler) {
				rs.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			}


			Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
			HRESULT hr = D3DX12SerializeVersionedRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &err);
			if (FAILED(hr)) {
				const char* errorText = (err && err->GetBufferPointer())
					? static_cast<const char*>(err->GetBufferPointer())
					: "<no serializer error>";
				spdlog::error(
					"DX12 CreatePipelineLayout serialize failed: hr=0x{:08X}, flags=0x{:X}, pushConstantCount={}, staticSamplerCount={}, error={} ",
					static_cast<uint32_t>(hr),
					static_cast<uint32_t>(rs.Desc_1_1.Flags),
					static_cast<uint32_t>(ld.pushConstants.size),
					static_cast<uint32_t>(ld.staticSamplers.size),
					errorText);
				RHI_FAIL(ToRHI(hr));
			}

			UINT64 infoQueueStart = 0;
			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (SUCCEEDED(impl->pNativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue))) && infoQueue) {
				infoQueueStart = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
			}
			Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
			hr = impl->pNativeDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
				IID_PPV_ARGS(&root));
			if (FAILED(hr)) {
				auto probeRootSignature = [&](const char* label,
					const D3D12_ROOT_PARAMETER* probeParams,
					UINT probeParamCount,
					const D3D12_STATIC_SAMPLER_DESC* probeSamplers,
					UINT probeSamplerCount,
					D3D12_ROOT_SIGNATURE_FLAGS probeFlags) {
					D3D12_ROOT_SIGNATURE_DESC probeDesc{};
					probeDesc.NumParameters = probeParamCount;
					probeDesc.pParameters = probeParams;
					probeDesc.NumStaticSamplers = probeSamplerCount;
					probeDesc.pStaticSamplers = probeSamplers;
					probeDesc.Flags = probeFlags;

					Microsoft::WRL::ComPtr<ID3DBlob> probeBlob, probeErr;
					const HRESULT serializeHr = D3D12SerializeRootSignature(
						&probeDesc,
						D3D_ROOT_SIGNATURE_VERSION_1,
						&probeBlob,
						&probeErr);
					HRESULT createHr = E_FAIL;
					if (SUCCEEDED(serializeHr) && probeBlob) {
						Microsoft::WRL::ComPtr<ID3D12RootSignature> probeRoot;
						createHr = impl->pNativeDevice->CreateRootSignature(
							0,
							probeBlob->GetBufferPointer(),
							probeBlob->GetBufferSize(),
							IID_PPV_ARGS(&probeRoot));
					}

					const char* probeErrorText = (probeErr && probeErr->GetBufferPointer())
						? static_cast<const char*>(probeErr->GetBufferPointer())
						: "<no probe serializer error>";
					spdlog::error(
						"DX12 CreatePipelineLayout probe [{}]: serializeHr=0x{:08X}, createHr=0x{:08X}, paramCount={}, samplerCount={}, flags=0x{:X}, serializerError={}",
						label,
						static_cast<uint32_t>(serializeHr),
						static_cast<uint32_t>(createHr),
						probeParamCount,
						probeSamplerCount,
						static_cast<uint32_t>(probeFlags),
						probeErrorText);
				};

				probeRootSignature("empty", nullptr, 0, nullptr, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE);
				probeRootSignature("samplers-only", nullptr, 0, ssmps.data(), static_cast<UINT>(ssmps.size()), D3D12_ROOT_SIGNATURE_FLAG_NONE);
				probeRootSignature("full-params-no-samplers", paramsV10.data(), static_cast<UINT>(paramsV10.size()), nullptr, 0, rs.Desc_1_1.Flags);
				probeRootSignature("reduced-params-with-samplers", paramsV10.data(), static_cast<UINT>((std::min<size_t>)(5, paramsV10.size())), ssmps.data(), static_cast<UINT>(ssmps.size()), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
				probeRootSignature("descriptor-params-only", paramsV10.size() >= 7 ? &paramsV10[5] : nullptr, paramsV10.size() >= 7 ? 2u : 0u, ssmps.data(), static_cast<UINT>(ssmps.size()), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

				D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSigFeature{};
				rootSigFeature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
				HRESULT rootSigFeatureHr = impl->pNativeDevice->CheckFeatureSupport(
					D3D12_FEATURE_ROOT_SIGNATURE,
					&rootSigFeature,
					sizeof(rootSigFeature));

				D3D12_FEATURE_DATA_D3D12_OPTIONS options0{};
				HRESULT optionsHr = impl->pNativeDevice->CheckFeatureSupport(
					D3D12_FEATURE_D3D12_OPTIONS,
					&options0,
					sizeof(options0));

				D3D12_FEATURE_DATA_SHADER_MODEL shaderModelFeature{};
				shaderModelFeature.HighestShaderModel = D3D_SHADER_MODEL_6_8;
				(void)impl->pNativeDevice->CheckFeatureSupport(
					D3D12_FEATURE_SHADER_MODEL,
					&shaderModelFeature,
					sizeof(shaderModelFeature));
				spdlog::error(
					"DX12 CreatePipelineLayout capabilities: highestShaderModel=0x{:X}, rootSignatureFeatureHr=0x{:08X}, highestRootSignatureVersion=0x{:X}, optionsHr=0x{:08X}, resourceBindingTier={}, resourceHeapTier={}",
					static_cast<uint32_t>(shaderModelFeature.HighestShaderModel),
					static_cast<uint32_t>(rootSigFeatureHr),
					static_cast<uint32_t>(rootSigFeature.HighestVersion),
					static_cast<uint32_t>(optionsHr),
					static_cast<uint32_t>(options0.ResourceBindingTier),
					static_cast<uint32_t>(options0.ResourceHeapTier));

				constexpr D3D12_ROOT_SIGNATURE_FLAGS kDirectIndexingFlags =
					D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
					D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
				D3D12_VERSIONED_ROOT_SIGNATURE_DESC fallbackRs = rs;
				fallbackRs.Desc_1_1.Flags &= ~kDirectIndexingFlags;

				Microsoft::WRL::ComPtr<ID3DBlob> fallbackBlob, fallbackErr;
				const HRESULT fallbackSerializeHr = D3DX12SerializeVersionedRootSignature(
					&fallbackRs,
					D3D_ROOT_SIGNATURE_VERSION_1_1,
					&fallbackBlob,
					&fallbackErr);
				HRESULT fallbackCreateHr = E_FAIL;
				if (SUCCEEDED(fallbackSerializeHr) && fallbackBlob) {
					Microsoft::WRL::ComPtr<ID3D12RootSignature> fallbackRoot;
					fallbackCreateHr = impl->pNativeDevice->CreateRootSignature(
						0,
						fallbackBlob->GetBufferPointer(),
						fallbackBlob->GetBufferSize(),
						IID_PPV_ARGS(&fallbackRoot));
				}

				const char* fallbackErrorText = (fallbackErr && fallbackErr->GetBufferPointer())
					? static_cast<const char*>(fallbackErr->GetBufferPointer())
					: "<no fallback serializer error>";
				spdlog::error(
					"DX12 CreatePipelineLayout fallback without direct heap indexing: serializeHr=0x{:08X}, createHr=0x{:08X}, fallbackFlags=0x{:X}, serializerError={}",
					static_cast<uint32_t>(fallbackSerializeHr),
					static_cast<uint32_t>(fallbackCreateHr),
					static_cast<uint32_t>(fallbackRs.Desc_1_1.Flags),
					fallbackErrorText);
				if (SUCCEEDED(fallbackCreateHr)) {
					spdlog::error(
						"DX12 CreatePipelineLayout probe: root signature succeeds only after removing direct heap indexing flags. This renderer requires ResourceDescriptorHeap[] and SamplerDescriptorHeap[] bindless access, so the current device/proxy is not compatible with the existing root signature model.");
				}

				D3D12_ROOT_SIGNATURE_DESC rsV10{};
				rsV10.NumParameters = static_cast<UINT>(paramsV10.size());
				rsV10.pParameters = paramsV10.data();
				rsV10.NumStaticSamplers = static_cast<UINT>(ssmps.size());
				rsV10.pStaticSamplers = ssmps.data();
				rsV10.Flags = rs.Desc_1_1.Flags;

				Microsoft::WRL::ComPtr<ID3DBlob> legacyBlob, legacyErr;
				const HRESULT legacySerializeHr = D3D12SerializeRootSignature(
					&rsV10,
					D3D_ROOT_SIGNATURE_VERSION_1,
					&legacyBlob,
					&legacyErr);
				HRESULT legacyCreateHr = E_FAIL;
				Microsoft::WRL::ComPtr<ID3D12RootSignature> legacyRoot;
				if (SUCCEEDED(legacySerializeHr) && legacyBlob) {
					legacyCreateHr = impl->pNativeDevice->CreateRootSignature(
						0,
						legacyBlob->GetBufferPointer(),
						legacyBlob->GetBufferSize(),
						IID_PPV_ARGS(&legacyRoot));
				}

				const char* legacyErrorText = (legacyErr && legacyErr->GetBufferPointer())
					? static_cast<const char*>(legacyErr->GetBufferPointer())
					: "<no legacy serializer error>";
				spdlog::error(
					"DX12 CreatePipelineLayout legacy v1.0 fallback: serializeHr=0x{:08X}, createHr=0x{:08X}, flags=0x{:X}, serializerError={}",
					static_cast<uint32_t>(legacySerializeHr),
					static_cast<uint32_t>(legacyCreateHr),
					static_cast<uint32_t>(rsV10.Flags),
					legacyErrorText);
				if (SUCCEEDED(legacyCreateHr) && legacyRoot) {
					spdlog::warn("DX12 CreatePipelineLayout: using legacy root signature v1.0 fallback after v1.1 CreateRootSignature failed");
					root = legacyRoot;
					hr = S_OK;
				}

				if (FAILED(hr)) {

				spdlog::error(
					"DX12 CreatePipelineLayout CreateRootSignature failed: hr=0x{:08X}, flags=0x{:X}, pushConstantCount={}, staticSamplerCount={}, blobSize={}",
					static_cast<uint32_t>(hr),
					static_cast<uint32_t>(rs.Desc_1_1.Flags),
					static_cast<uint32_t>(ld.pushConstants.size),
					static_cast<uint32_t>(ld.staticSamplers.size),
					blob ? blob->GetBufferSize() : 0);
				Dx12LogInfoQueueMessagesSince(impl->pNativeDevice.Get(), infoQueueStart);
				RHI_FAIL(ToRHI(hr));
				}
			}

			Dx12PipelineLayout L(ld, impl);
			if (ld.pushConstants.size && ld.pushConstants.data) {
				L.pcs.assign(ld.pushConstants.data, ld.pushConstants.data + ld.pushConstants.size);
				// Build rcParams in the same order as params:
				L.rcParams.reserve(ld.pushConstants.size);
				for (uint32_t i = 0; i < ld.pushConstants.size; ++i) {
					const auto& pc = ld.pushConstants.data[i];
					L.rcParams.push_back(Dx12PipelineLayout::RootConstParam{
						pc.set,
						pc.binding,
						pc.num32BitValues,
						/*rootIndex=*/i,
						pc.type
						});
				}
			}
			if (ld.staticSamplers.size && ld.staticSamplers.data) {
				L.staticSamplers.assign(ld.staticSamplers.data, ld.staticSamplers.data + ld.staticSamplers.size);
			}
			L.root = root;
			auto handle = impl->pipelineLayouts.alloc(L);
			PipelineLayout ret(handle);
			ret.vt = &g_plvt;
			ret.impl = impl;
			out = MakePipelineLayoutPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void CopyUtf8FromWide(const wchar_t* src, char* dst, size_t dstCap) {
			if (!dst || dstCap == 0) return;
			dst[0] = '\0';
			if (!src) return;
			int written = WideCharToMultiByte(
				CP_UTF8,
				0, src,
				-1, dst,
				static_cast<int>(dstCap),
				nullptr,
				nullptr);
			if (written <= 0) dst[0] = '\0';
			dst[dstCap - 1] = '\0';
		}

		inline std::vector<D3D_SHADER_MODEL> shaderModels = {
			D3D_SHADER_MODEL_6_8,
			D3D_SHADER_MODEL_6_7,
			D3D_SHADER_MODEL_6_6,
			D3D_SHADER_MODEL_6_5,
			D3D_SHADER_MODEL_6_4,
			D3D_SHADER_MODEL_6_3,
			D3D_SHADER_MODEL_6_2,
			D3D_SHADER_MODEL_6_1,
			D3D_SHADER_MODEL_6_0,
		};

		inline static D3D_SHADER_MODEL getHighestShaderModel(ID3D12Device* dev)
		{
			if (!dev) return D3D_SHADER_MODEL_6_0;

			for (const auto& model : shaderModels) {
				D3D12_FEATURE_DATA_SHADER_MODEL sm{};
				sm.HighestShaderModel = model;
				if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
					return model;
				}
			}
			BreakIfDebugging(); // Nothing under 6_0 is supported
			return D3D_SHADER_MODEL_6_0;
		}

		template<class T>
		concept HasPerPrimitiveVrs = requires(T t) { t.PerPrimitiveShadingRateSupportedWithViewportIndexing; };

		template<class T>
		concept HasAdditionalVrsRates = requires(T t) { t.AdditionalShadingRatesSupported; };

		static Result d_queryFeatureInfo(const Device* d, FeatureInfoHeader* chain) noexcept
		{
			if (!d || !chain) return Result::InvalidArgument;

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice) return Result::Failed;

			ID3D12Device* dev = impl->pNativeDevice.Get();

			D3D12_FEATURE_DATA_D3D12_OPTIONS   opt0{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS1  opt1{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS3  opt3{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS5  opt5{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS6  opt6{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS7  opt7{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS9  opt9{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS11 opt11{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS12 opt12{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS14 opt14{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS16 opt16{};
			D3D12_FEATURE_DATA_D3D12_OPTIONS21 opt21{};
			D3D12_FEATURE_DATA_TIGHT_ALIGNMENT ta{};

			// Note: if a CheckFeatureSupport fails, the struct stays zeroed => "unsupported".
			(void)dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt0, sizeof(opt0));
			(void)dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1));
			(void)dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &opt3, sizeof(opt3));
			(void)dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
			auto hasOptions6 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opt6, sizeof(opt6)));
			auto hasOptions7 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)));
			auto hasOptions9 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &opt9, sizeof(opt9)));
			auto hasOptions11 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &opt11, sizeof(opt11)));
			auto hasOptions12 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opt12, sizeof(opt12)));
			auto hasOptions14 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &opt14, sizeof(opt14)));
			auto hasOptions16 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &opt16, sizeof(opt16)));
			auto hasOptions21 = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &opt21, sizeof(opt21)));
			auto hasTightAlignment = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT, &ta, sizeof(ta)));

			D3D12_FEATURE_DATA_SHADER_MODEL sm{};
			sm.HighestShaderModel = getHighestShaderModel(dev);

			// Architecture (UMA, etc.)
			D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
			arch.NodeIndex = 0;
			(void)dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch));

			bool gpuUploadHeapSupported = false;
			if (hasOptions16) {
				gpuUploadHeapSupported = opt16.GPUUploadHeapSupported;
			}

			bool tightAlignmentSupported = false;
			if (hasTightAlignment) {
				tightAlignmentSupported = (ta.SupportTier >= D3D12_TIGHT_ALIGNMENT_TIER_1);
			}

			const bool createNotZeroedSupported = hasOptions7;

			const bool hasMeshShaders =
				(opt7.MeshShaderTier == D3D12_MESH_SHADER_TIER_1);

			const bool hasRayTracingPipeline =
				(opt5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0) ||
				(opt5.RaytracingTier == D3D12_RAYTRACING_TIER_1_1);

			const bool hasRayTracing11 =
				(opt5.RaytracingTier == D3D12_RAYTRACING_TIER_1_1);

			const bool hasVrsPerDraw =
				(opt6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_1) ||
				(opt6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_2);

			const bool hasVrsAttachment =
				(opt6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_2);

			const bool unifiedResourceHeaps =
				(opt0.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_2);

			const bool unboundedDescriptorTables =
				(opt0.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3);

			// Optional DX12 Options11-derived semantics
			bool derivativesInMeshAndTask = false;
			bool atomicInt64GroupShared = false;
			bool atomicInt64Typed = false;
			bool atomicInt64OnDescriptorHeapResources = false;
			if (hasOptions9) {
				derivativesInMeshAndTask = opt9.DerivativesInMeshAndAmplificationShadersSupported;
				atomicInt64GroupShared = opt9.AtomicInt64OnGroupSharedSupported;
				atomicInt64Typed = opt9.AtomicInt64OnTypedResourceSupported;
			}
			if (hasOptions11) {
				atomicInt64OnDescriptorHeapResources = opt11.AtomicInt64OnDescriptorHeapResourceSupported;
			}

			//Walk requested chain and fill
			for (FeatureInfoHeader* h = chain; h; h = h->pNext) {
				if (h->structSize < sizeof(FeatureInfoHeader))
					return Result::InvalidArgument;

				switch (h->sType) {
				case FeatureInfoStructType::AdapterInfo: {
					if (h->structSize < sizeof(AdapterFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<AdapterFeatureInfo*>(h);

					DXGI_ADAPTER_DESC3 desc{};
					if (impl->adapter && SUCCEEDED(impl->adapter->GetDesc3(&desc))) {
						CopyUtf8FromWide(desc.Description, out->name, sizeof(out->name));
						out->vendorId = desc.VendorId;
						out->deviceId = desc.DeviceId;
						out->dedicatedVideoMemory = desc.DedicatedVideoMemory;
						out->dedicatedSystemMemory = desc.DedicatedSystemMemory;
						out->sharedSystemMemory = desc.SharedSystemMemory;
					}
					else {
						// Leave defaults if adapter unavailable.
						out->name[0] = '\0';
						out->vendorId = out->deviceId = 0;
						out->dedicatedVideoMemory = out->dedicatedSystemMemory = out->sharedSystemMemory = 0;
					}
				} break;

				case FeatureInfoStructType::Architecture: {
					if (h->structSize < sizeof(ArchitectureFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<ArchitectureFeatureInfo*>(h);
					out->uma = arch.UMA;
					out->cacheCoherentUMA = arch.CacheCoherentUMA;
					out->isolatedMMU = arch.IsolatedMMU;
					out->tileBasedRenderer = arch.TileBasedRenderer;
				} break;

				case FeatureInfoStructType::Features: {
					if (h->structSize < sizeof(ShaderFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<ShaderFeatureInfo*>(h);

					out->maxShaderModel = ToRHI(sm.HighestShaderModel);

					// Tier-derived semantics:
					out->unifiedResourceHeaps = unifiedResourceHeaps;
					out->unboundedDescriptorTables = unboundedDescriptorTables;

					// "Actual shader capability" bits:
					out->waveOps = opt1.WaveOps;
					out->int64ShaderOps = opt1.Int64ShaderOps;
					out->barycentrics = opt3.BarycentricsSupported;

					// Options11-derived bits (or false if not available):
					out->derivativesInMeshAndTaskShaders = derivativesInMeshAndTask;
					out->atomicInt64OnGroupShared = atomicInt64GroupShared;
					out->atomicInt64OnTypedResource = atomicInt64Typed;
					out->atomicInt64OnDescriptorHeapResources = atomicInt64OnDescriptorHeapResources;
				} break;

				case FeatureInfoStructType::MeshShaders: {
					if (h->structSize < sizeof(MeshShaderFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<MeshShaderFeatureInfo*>(h);

					// DX12 exposes this as a single tier: if present, both mesh+amplification exist.
					out->meshShader = hasMeshShaders;
					out->taskShader = hasMeshShaders;

					// Derivatives support is independent (Options11). Only meaningful if mesh shaders exist.
					out->derivatives = hasMeshShaders && derivativesInMeshAndTask;
				} break;

				case FeatureInfoStructType::RayTracing: {
					if (h->structSize < sizeof(RayTracingFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<RayTracingFeatureInfo*>(h);

					// DXR tier -> semantic bits.
					out->pipeline = hasRayTracingPipeline;

					// DXR 1.1 implies inline raytracing
					out->rayQuery = hasRayTracing11;

					// Also a heuristic: "indirect trace" is not a single clean DX12 bit in core options;
					out->indirect = hasRayTracing11;
				} break;

				case FeatureInfoStructType::ShadingRate: {
					if (h->structSize < sizeof(ShadingRateFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<ShadingRateFeatureInfo*>(h);

					out->perDrawRate = hasVrsPerDraw;
					out->attachmentRate = hasVrsAttachment;

					// Only meaningful if attachmentRate.
					out->tileSize = hasVrsAttachment ? opt6.ShadingRateImageTileSize : 0;

					if constexpr (HasAdditionalVrsRates<decltype(opt6)>) {
						out->additionalRates = (opt6.AdditionalShadingRatesSupported != 0);
					}
					else {
						out->additionalRates = false;
					}

					if constexpr (HasPerPrimitiveVrs<decltype(opt6)>) {
						out->perPrimitiveRate = (opt6.PerPrimitiveShadingRateSupportedWithViewportIndexing != 0);
					}
					else {
						out->perPrimitiveRate = false;
					}
				} break;

				case FeatureInfoStructType::EnhancedBarriers: {
					if (h->structSize < sizeof(EnhancedBarriersFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<EnhancedBarriersFeatureInfo*>(h);

					if (hasOptions12) {
						out->enhancedBarriers = (opt12.EnhancedBarriersSupported != 0);
						out->relaxedFormatCasting = (opt12.RelaxedFormatCastingSupported != 0);
					}
					else {
						out->enhancedBarriers = false;
						out->relaxedFormatCasting = false;
					}
				} break;
				case FeatureInfoStructType::ResourceAllocation: {
					if (h->structSize < sizeof(ResourceAllocationFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<ResourceAllocationFeatureInfo*>(h);

					out->gpuUploadHeapSupported = gpuUploadHeapSupported;
					out->tightAlignmentSupported = tightAlignmentSupported;
					out->createNotZeroedHeapSupported = createNotZeroedSupported;
				} break;
				case FeatureInfoStructType::WorkGraphs: {
					if (h->structSize < sizeof(WorkGraphFeatureInfo)) return Result::InvalidArgument;
					auto* out = reinterpret_cast<WorkGraphFeatureInfo*>(h);
					if (hasOptions21) {
						out->computeNodes = (opt21.WorkGraphsTier != D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED);
						out->meshNodes = false; // DX12 does not expose mesh node support yet
					}
					else {
						out->computeNodes = false;
						out->meshNodes = false;
					}
					
				} break;
				default:
					// Unknown sType: ignore
					break;
				}
			}

			return Result::Ok;
		}

		static Result d_queryVideoMemoryInfo(
			const Device* d,
			uint32_t nodeIndex,
			MemorySegmentGroup segmentGroup,
			VideoMemoryInfo& out) noexcept
		{
			if (!d) return Result::InvalidArgument;

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->adapter) return Result::Failed;

			ComPtr<IDXGIAdapter3> a3;
			HRESULT hr = impl->adapter.As(&a3);
			if (FAILED(hr) || !a3) return Result::Unsupported;

			DXGI_MEMORY_SEGMENT_GROUP dxGroup = DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
			switch (segmentGroup) {
			case MemorySegmentGroup::Local:    dxGroup = DXGI_MEMORY_SEGMENT_GROUP_LOCAL; break;
			case MemorySegmentGroup::NonLocal: dxGroup = DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL; break;
			default: return Result::InvalidArgument;
			}

			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			hr = a3->QueryVideoMemoryInfo(nodeIndex, dxGroup, &info);

			if (FAILED(hr)) {
				return ToRHI(hr);
			}

			out.budgetBytes = info.Budget;
			out.currentUsageBytes = info.CurrentUsage;
			out.availableForReservationBytes = info.AvailableForReservation;
			out.currentReservationBytes = info.CurrentReservation;
			return Result::Ok;
		}

		static void d_destroyPipelineLayout(DeviceDeletionContext* d, PipelineLayoutHandle h) noexcept {
			dx12_detail::Dev(d)->pipelineLayouts.free(h);
		}

		static bool FillDx12Arg(const IndirectArg& a, D3D12_INDIRECT_ARGUMENT_DESC& out) {
			switch (a.kind) {
			case IndirectArgKind::Constant:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
				out.Constant.RootParameterIndex = a.u.rootConstants.rootIndex;
				out.Constant.DestOffsetIn32BitValues = a.u.rootConstants.destOffset32;
				out.Constant.Num32BitValuesToSet = a.u.rootConstants.num32;
				return true;
			case IndirectArgKind::DispatchMesh:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
				return true;
			case IndirectArgKind::Dispatch:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
				return true;
			case IndirectArgKind::Draw:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
				return true;
			case IndirectArgKind::DrawIndexed:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
				return true;
			case IndirectArgKind::VertexBuffer:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
				out.VertexBuffer.Slot = a.u.vertexBuffer.slot; return true;
			case IndirectArgKind::IndexBuffer:
				out.Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
				return true;
			default: return false;
			}
		}

		static Result d_createCommandSignature(Device* d,
			const CommandSignatureDesc& cd,
			const PipelineLayoutHandle layout, CommandSignaturePtr& out) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);

			std::vector<D3D12_INDIRECT_ARGUMENT_DESC> dxArgs(cd.args.size);
			bool hasRoot = false;
			for (uint32_t i = 0; i < cd.args.size; ++i) {
				if (!FillDx12Arg(cd.args.data[i], dxArgs[i])) {
					RHI_FAIL(Result::InvalidArgument);
				}
				hasRoot |= (cd.args.data[i].kind == IndirectArgKind::Constant);
			}

			ID3D12RootSignature* rs = nullptr;
			if (hasRoot) {
				auto* L = impl->pipelineLayouts.get(layout);
				if (!L || !L->root) {
					RHI_FAIL(Result::InvalidArgument);
				}
				for (uint32_t i = 0; i < cd.args.size; ++i) {
					if (cd.args.data[i].kind != IndirectArgKind::Constant) {
						continue;
					}

					const uint32_t rootIndex = cd.args.data[i].u.rootConstants.rootIndex;
					if (rootIndex >= L->rcParams.size()) {
						spdlog::error(
							"DX12 CreateCommandSignature: root constant arg {} targets invalid root parameter slot {}",
							i,
							rootIndex);
						RHI_FAIL(Result::InvalidArgument);
					}

					if (L->rcParams[rootIndex].type != PushConstantRangeType::RootConstants32) {
						spdlog::error(
							"DX12 CreateCommandSignature: root constant arg {} targets emulated root-constant slot {} (set={}, binding={})",
							i,
							rootIndex,
							L->rcParams[rootIndex].set,
							L->rcParams[rootIndex].binding);
						RHI_FAIL(Result::InvalidArgument);
					}
				}
				rs = L->root.Get();
			}

			D3D12_COMMAND_SIGNATURE_DESC desc{};
			desc.pArgumentDescs = dxArgs.data();
			desc.NumArgumentDescs = static_cast<UINT>(dxArgs.size());
			desc.ByteStride = cd.byteStride;

			Microsoft::WRL::ComPtr<ID3D12CommandSignature> cs;
			if (const auto hr = impl->pNativeDevice->CreateCommandSignature(&desc, rs, IID_PPV_ARGS(&cs)); FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}
			const Dx12CommandSignature s(cs, cd.byteStride, impl);
			const auto handle = impl->commandSignatures.alloc(s);
			CommandSignature ret(handle);
			ret.vt = &g_csvt;
			ret.impl = impl;
			out = MakeCommandSignaturePtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyCommandSignature(DeviceDeletionContext* d, CommandSignatureHandle h) noexcept {
			dx12_detail::Dev(d)->commandSignatures.free(h);
		}

		static Result d_createDescriptorHeap(Device* d, const DescriptorHeapDesc& hd, DescriptorHeapPtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);

			D3D12_DESCRIPTOR_HEAP_DESC desc{};
			desc.Type = ToDX(hd.type);
			desc.NumDescriptors = hd.capacity;
			desc.Flags = hd.shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
			if (const auto hr = impl->pNativeDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}

			const auto descriptorSize = impl->pNativeDevice->GetDescriptorHandleIncrementSize(desc.Type);
			const Dx12DescriptorHeap H(heap, desc.Type, descriptorSize, (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0, impl);

			const auto handle = impl->descHeaps.alloc(H);
			DescriptorHeap ret(handle);
			ret.vt = &g_dhvt;
			ret.impl = impl;
			out = MakeDescriptorHeapPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}
		static void d_destroyDescriptorHeap(DeviceDeletionContext* d, DescriptorHeapHandle h) noexcept {
			dx12_detail::Dev(d)->descHeaps.free(h);
		}
		static bool DxGetDstCpu(Dx12Device* impl, DescriptorSlot s, D3D12_CPU_DESCRIPTOR_HANDLE& out, D3D12_DESCRIPTOR_HEAP_TYPE expect) {
			auto* H = impl->descHeaps.get(s.heap);
			if (!H || H->type != expect) return false;
			out = H->cpuStart;
			out.ptr += SIZE_T(s.index) * H->inc;
			return true;
		}

		static bool DxGetDstGpu(Dx12Device* impl, DescriptorSlot s,
			D3D12_GPU_DESCRIPTOR_HANDLE& out,
			D3D12_DESCRIPTOR_HEAP_TYPE expect) {
			auto* H = impl->descHeaps.get(s.heap);
			if (!H || H->type != expect || !H->shaderVisible) return false;
			out = H->gpuStart;
			out.ptr += static_cast<UINT64>(s.index) * H->inc;
			return true;
		}

		static Result d_createShaderResourceView(Device* d, DescriptorSlot s, const ResourceHandle& resource, const SrvDesc& dv) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);

			D3D12_CPU_DESCRIPTOR_HANDLE dst{};
			if (!DxGetDstCpu(impl, s, dst, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Shader4ComponentMapping = (dv.componentMapping == 0)
				? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
				: dv.componentMapping;

			switch (dv.dimension) {
			case SrvDim::Buffer: {
				auto* B = impl->resources.get(resource);
				if (!B || !B->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}

				desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				switch (dv.buffer.kind) {
				case BufferViewKind::Raw:
					desc.Format = DXGI_FORMAT_R32_TYPELESS;
					desc.Buffer.FirstElement = static_cast<UINT>(dv.buffer.firstElement); // in 32-bit units
					desc.Buffer.NumElements = dv.buffer.numElements;
					desc.Buffer.StructureByteStride = 0;
					desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
					break;
				case BufferViewKind::Structured:
					desc.Format = DXGI_FORMAT_UNKNOWN;
					desc.Buffer.FirstElement = static_cast<UINT>(dv.buffer.firstElement);
					desc.Buffer.NumElements = dv.buffer.numElements;
					desc.Buffer.StructureByteStride = dv.buffer.structureByteStride;
					desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
					break;
				case BufferViewKind::Typed:
					desc.Format = ToDxgi(dv.formatOverride);
					desc.Buffer.FirstElement = static_cast<UINT>(dv.buffer.firstElement);
					desc.Buffer.NumElements = dv.buffer.numElements;
					desc.Buffer.StructureByteStride = 0;
					desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
					break;
				}
				impl->pNativeDevice->CreateShaderResourceView(B->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture1D: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
				desc.Texture1D.MostDetailedMip = dv.tex1D.mostDetailedMip;
				desc.Texture1D.MipLevels = dv.tex1D.mipLevels;
				desc.Texture1D.ResourceMinLODClamp = dv.tex1D.minLodClamp;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture1DArray: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
				desc.Texture1DArray.MostDetailedMip = dv.tex1DArray.mostDetailedMip;
				desc.Texture1DArray.MipLevels = dv.tex1DArray.mipLevels;
				desc.Texture1DArray.FirstArraySlice = dv.tex1DArray.firstArraySlice;
				desc.Texture1DArray.ArraySize = dv.tex1DArray.arraySize;
				desc.Texture1DArray.ResourceMinLODClamp = dv.tex1DArray.minLodClamp;
				auto* R = T->res.Get();
				impl->pNativeDevice->CreateShaderResourceView(R, &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture2D: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MostDetailedMip = dv.tex2D.mostDetailedMip;
				desc.Texture2D.MipLevels = dv.tex2D.mipLevels;
				desc.Texture2D.PlaneSlice = dv.tex2D.planeSlice;
				desc.Texture2D.ResourceMinLODClamp = dv.tex2D.minLodClamp;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture2DArray: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MostDetailedMip = dv.tex2DArray.mostDetailedMip;
				desc.Texture2DArray.MipLevels = dv.tex2DArray.mipLevels;
				desc.Texture2DArray.FirstArraySlice = dv.tex2DArray.firstArraySlice;
				desc.Texture2DArray.ArraySize = dv.tex2DArray.arraySize;
				desc.Texture2DArray.PlaneSlice = dv.tex2DArray.planeSlice;
				desc.Texture2DArray.ResourceMinLODClamp = dv.tex2DArray.minLodClamp;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture2DMS: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture2DMSArray: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
				desc.Texture2DMSArray.FirstArraySlice = dv.tex2DMSArray.firstArraySlice;
				desc.Texture2DMSArray.ArraySize = dv.tex2DMSArray.arraySize;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::Texture3D: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				desc.Texture3D.MostDetailedMip = dv.tex3D.mostDetailedMip;
				desc.Texture3D.MipLevels = dv.tex3D.mipLevels;
				desc.Texture3D.ResourceMinLODClamp = dv.tex3D.minLodClamp;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::TextureCube: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
				desc.TextureCube.MostDetailedMip = dv.cube.mostDetailedMip;
				desc.TextureCube.MipLevels = dv.cube.mipLevels;
				desc.TextureCube.ResourceMinLODClamp = dv.cube.minLodClamp;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::TextureCubeArray: {
				auto* T = impl->resources.get(resource); if (!T || !T->res) return Result::InvalidArgument;
				desc.Format = (dv.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dv.formatOverride);
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
				desc.TextureCubeArray.MostDetailedMip = dv.cubeArray.mostDetailedMip;
				desc.TextureCubeArray.MipLevels = dv.cubeArray.mipLevels;
				desc.TextureCubeArray.First2DArrayFace = dv.cubeArray.first2DArrayFace;
				desc.TextureCubeArray.NumCubes = dv.cubeArray.numCubes;
				desc.TextureCubeArray.ResourceMinLODClamp = dv.cubeArray.minLodClamp;
				impl->pNativeDevice->CreateShaderResourceView(T->res.Get(), &desc, dst);
				return Result::Ok;
			}

			case SrvDim::AccelerationStruct: {
				// AS is stored in a buffer with ResourceFlags::RaytracingAccelerationStructure
				auto* B = impl->resources.get(resource); if (!B || !B->res) return Result::InvalidArgument;
				desc.Format = DXGI_FORMAT_UNKNOWN;
				desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
				desc.RaytracingAccelerationStructure.Location = B->res->GetGPUVirtualAddress();
				impl->pNativeDevice->CreateShaderResourceView(B->res.Get(), &desc, dst);
				return Result::Ok;
			}

			default: break;
			}
			BreakIfDebugging();
			return Result::InvalidArgument;
		}

		static Result d_createUnorderedAccessView(Device* d, DescriptorSlot s, const ResourceHandle& resource, const UavDesc& dv) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				BreakIfDebugging();
				return Result::Failed;
			};

			D3D12_CPU_DESCRIPTOR_HANDLE dst{};
			if (!DxGetDstCpu(impl, s, dst, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			ID3D12Resource* pResource = nullptr;
			ID3D12Resource* pCounterResource = nullptr; // optional, for structured append/consume

			switch (dv.dimension)
			{
				// ========================= Buffer UAV =========================
			case UavDim::Buffer:
			{
				auto* B = impl->resources.get(resource);
				if (!B || !B->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}

				pResource = B->res.Get();
				desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
				desc.Buffer.FirstElement = (UINT)dv.buffer.firstElement;
				desc.Buffer.NumElements = dv.buffer.numElements;
				desc.Buffer.CounterOffsetInBytes = (UINT)dv.buffer.counterOffsetInBytes;

				switch (dv.buffer.kind)
				{
				case BufferViewKind::Raw:
					desc.Format = DXGI_FORMAT_R32_TYPELESS;
					desc.Buffer.StructureByteStride = 0;
					desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
					break;

				case BufferViewKind::Structured:
					desc.Format = DXGI_FORMAT_UNKNOWN;
					desc.Buffer.StructureByteStride = dv.buffer.structureByteStride;
					desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
					// If caller provided a counter offset, assume the counter is in the same buffer.
					if (dv.buffer.counterOffsetInBytes != 0)
						pCounterResource = pResource;
					break;

				case BufferViewKind::Typed:
					desc.Format = ToDxgi(dv.formatOverride);
					desc.Buffer.StructureByteStride = 0;
					desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
					break;
				}

				impl->pNativeDevice->CreateUnorderedAccessView(pResource, pCounterResource, &desc, dst);
				return Result::Ok;
			}

			// ========================= Texture UAVs =========================
			case UavDim::Texture1D:
			{
				auto* T = impl->resources.get(resource);
				if (!T || !T->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}
				pResource = T->res.Get();
				if (dv.formatOverride != Format::Unknown) {
					desc.Format = ToDxgi(dv.formatOverride);
				}
				else {
					desc.Format = T->fmt;
				}
				desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
				desc.Texture1D.MipSlice = dv.texture1D.mipSlice;

				impl->pNativeDevice->CreateUnorderedAccessView(pResource, nullptr, &desc, dst);
				return Result::Ok;
			}

			case UavDim::Texture1DArray:
			{
				auto* T = impl->resources.get(resource);
				if (!T || !T->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}
				pResource = T->res.Get();

				if (dv.formatOverride != Format::Unknown) {
					desc.Format = ToDxgi(dv.formatOverride);
				}
				else {
					desc.Format = T->fmt;
				}
				desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
				desc.Texture1DArray.MipSlice = dv.texture1DArray.mipSlice;
				desc.Texture1DArray.FirstArraySlice = dv.texture1DArray.firstArraySlice;
				desc.Texture1DArray.ArraySize = dv.texture1DArray.arraySize;

				impl->pNativeDevice->CreateUnorderedAccessView(pResource, nullptr, &desc, dst);
				return Result::Ok;
			}

			case UavDim::Texture2D:
			{
				auto* T = impl->resources.get(resource);
				if (!T || !T->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}
				pResource = T->res.Get();

				if (dv.formatOverride != Format::Unknown) {
					desc.Format = ToDxgi(dv.formatOverride);
				}
				else {
					desc.Format = T->fmt;
				}			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = dv.texture2D.mipSlice;
				desc.Texture2D.PlaneSlice = dv.texture2D.planeSlice;

				impl->pNativeDevice->CreateUnorderedAccessView(pResource, nullptr, &desc, dst);
				return Result::Ok;
			}

			case UavDim::Texture2DArray:
			{
				auto* T = impl->resources.get(resource);
				if (!T || !T->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}
				pResource = T->res.Get();

				if (dv.formatOverride != Format::Unknown) {
					desc.Format = ToDxgi(dv.formatOverride);
				}
				else {
					desc.Format = T->fmt;
				}			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MipSlice = dv.texture2DArray.mipSlice;
				desc.Texture2DArray.FirstArraySlice = dv.texture2DArray.firstArraySlice;
				desc.Texture2DArray.ArraySize = dv.texture2DArray.arraySize;
				desc.Texture2DArray.PlaneSlice = dv.texture2DArray.planeSlice;

				impl->pNativeDevice->CreateUnorderedAccessView(pResource, nullptr, &desc, dst);
				return Result::Ok;
			}

			case UavDim::Texture3D:
			{
				auto* T = impl->resources.get(resource);
				if (!T || !T->res) {
					BreakIfDebugging();
					return Result::InvalidArgument;
				}
				pResource = T->res.Get();

				if (dv.formatOverride != Format::Unknown) {
					desc.Format = ToDxgi(dv.formatOverride);
				}
				else {
					desc.Format = T->fmt;
				}			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
				desc.Texture3D.MipSlice = dv.texture3D.mipSlice;
				desc.Texture3D.FirstWSlice = dv.texture3D.firstWSlice;
				desc.Texture3D.WSize = (dv.texture3D.wSize == 0) ? UINT(-1) : dv.texture3D.wSize;

				impl->pNativeDevice->CreateUnorderedAccessView(pResource, nullptr, &desc, dst);
				return Result::Ok;
			}

			case UavDim::Texture2DMS:
			case UavDim::Texture2DMSArray:
				// UAVs for MSAA textures are not supported by D3D12
				BreakIfDebugging();
				return Result::Unsupported;
			}
			BreakIfDebugging();
			return Result::InvalidArgument;
		}

		static Result d_createConstantBufferView(Device* d, DescriptorSlot s, const ResourceHandle& bh, const CbvDesc& dv) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			D3D12_CPU_DESCRIPTOR_HANDLE dst{};
			if (!DxGetDstCpu(impl, s, dst, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) return Result::InvalidArgument;
			auto* B = impl->resources.get(bh); if (!B) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
			desc.BufferLocation = B->res->GetGPUVirtualAddress() + dv.byteOffset;
			desc.SizeInBytes = (UINT)((dv.byteSize + 255) & ~255u);
			impl->pNativeDevice->CreateConstantBufferView(&desc, dst);
			return Result::Ok;
		}

		static Result d_createSampler(Device* d, DescriptorSlot s, const SamplerDesc& sd) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);
			D3D12_CPU_DESCRIPTOR_HANDLE dst{};
			if (!DxGetDstCpu(impl, s, dst, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			D3D12_SAMPLER_DESC desc{};
			desc.Filter = BuildDxFilter(sd);
			desc.AddressU = ToDX(sd.addressU);
			desc.AddressV = ToDX(sd.addressV);
			desc.AddressW = ToDX(sd.addressW);

			// DX12 ignores unnormalizedCoordinates (always normalized)

			// Clamp anisotropy to device limit (DX12 spec says 1->16)
			desc.MaxAnisotropy = (sd.maxAnisotropy > 1) ? (std::min<uint32_t>(sd.maxAnisotropy, 16u)) : 1u;

			desc.MipLODBias = sd.mipLodBias;
			desc.MinLOD = EffectiveDxMinLod(sd);
			desc.MaxLOD = EffectiveDxMaxLod(sd);

			desc.ComparisonFunc = sd.compareEnable ? ToDX(sd.compareOp) : D3D12_COMPARISON_FUNC_NEVER;

			FillDxBorderColor(sd, desc.BorderColor);

			impl->pNativeDevice->CreateSampler(&desc, dst);
			return Result::Ok;
		}

		static Result d_createRenderTargetView(Device* d, DescriptorSlot s, const ResourceHandle& texture, const RtvDesc& rd) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				BreakIfDebugging();
				return Result::Failed;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE dst{};
			if (!DxGetDstCpu(impl, s, dst, D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			// For texture RTVs we expect a texture resource
			auto* T = impl->resources.get(texture);
			if (!T && rd.dimension != RtvDim::Buffer) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}
			if (rd.dimension != RtvDim::Buffer && (!T || !T->res)) {
				spdlog::critical(
					"DX12 createRTV: null resource for handle=({}, {}) heap=({}, {}) fmt={} dim={} tex={}x{}",
					texture.index,
					texture.generation,
					s.heap.index,
					s.index,
					T ? static_cast<unsigned>(T->fmt) : 0u,
					T ? static_cast<unsigned>(T->dim) : 0u,
					T ? T->tex.w : 0u,
					T ? T->tex.h : 0u);
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			D3D12_RENDER_TARGET_VIEW_DESC r{};
			ID3D12Resource* pRes = nullptr;

			switch (rd.dimension)
			{
			case RtvDim::Texture1D:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
				r.Texture1D.MipSlice = rd.range.baseMip;
				break;
			}

			case RtvDim::Texture1DArray:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
				r.Texture1DArray.MipSlice = rd.range.baseMip;
				r.Texture1DArray.FirstArraySlice = rd.range.baseLayer;
				r.Texture1DArray.ArraySize = rd.range.layerCount;
				break;
			}

			case RtvDim::Texture2D:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				r.Texture2D.MipSlice = rd.range.baseMip;
				r.Texture2D.PlaneSlice = 0; // no plane in desc -> default to 0
				break;
			}

			case RtvDim::Texture2DArray:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
				r.Texture2DArray.MipSlice = rd.range.baseMip;
				r.Texture2DArray.FirstArraySlice = rd.range.baseLayer;
				r.Texture2DArray.ArraySize = rd.range.layerCount;
				r.Texture2DArray.PlaneSlice = 0;
				break;
			}

			case RtvDim::Texture2DMS:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
				break;
			}

			case RtvDim::Texture2DMSArray:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
				r.Texture2DMSArray.FirstArraySlice = rd.range.baseLayer;
				r.Texture2DMSArray.ArraySize = rd.range.layerCount;
				break;
			}

			case RtvDim::Texture3D:
			{
				pRes = T->res.Get();
				r.Format = (rd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(rd.formatOverride);
				r.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
				r.Texture3D.MipSlice = rd.range.baseMip;
				// Reuse range.baseLayer/layerCount to address Z-slices of the 3D subresource.
				r.Texture3D.FirstWSlice = rd.range.baseLayer;
				r.Texture3D.WSize = (rd.range.layerCount == 0) ? UINT(-1) : rd.range.layerCount;
				break;
			}

			case RtvDim::Buffer:
			{
				// TODO: What is this?
				BreakIfDebugging();
				return Result::Unsupported;
			}

			default:
				BreakIfDebugging();
				return Result::Unsupported;
			}
			impl->pNativeDevice->CreateRenderTargetView(pRes, &r, dst);
			return Result::Ok;
		}

		static Result d_createDepthStencilView(Device* d, DescriptorSlot s, const ResourceHandle& texture, const DsvDesc& dd) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				BreakIfDebugging();
				return Result::Failed;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE dst{};
			if (!DxGetDstCpu(impl, s, dst, D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			auto* T = impl->resources.get(texture);
			if (!T) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}

			D3D12_DEPTH_STENCIL_VIEW_DESC z{};
			z.Format = (dd.formatOverride == Format::Unknown) ? T->fmt : ToDxgi(dd.formatOverride);
			z.Flags = (dd.readOnlyDepth ? D3D12_DSV_FLAG_READ_ONLY_DEPTH : (D3D12_DSV_FLAGS)0) |
				(dd.readOnlyStencil ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : (D3D12_DSV_FLAGS)0);

			switch (dd.dimension)
			{
			case DsvDim::Texture1D:
				z.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
				z.Texture1D.MipSlice = dd.range.baseMip;
				break;

			case DsvDim::Texture1DArray:
				z.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
				z.Texture1DArray.MipSlice = dd.range.baseMip;
				z.Texture1DArray.FirstArraySlice = dd.range.baseLayer;
				z.Texture1DArray.ArraySize = dd.range.layerCount;
				break;

			case DsvDim::Texture2D:
				z.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				z.Texture2D.MipSlice = dd.range.baseMip;
				break;

			case DsvDim::Texture2DArray:
				z.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
				z.Texture2DArray.MipSlice = dd.range.baseMip;
				z.Texture2DArray.FirstArraySlice = dd.range.baseLayer;
				z.Texture2DArray.ArraySize = dd.range.layerCount;
				break;

			case DsvDim::Texture2DMS:
				z.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
				break;

			case DsvDim::Texture2DMSArray:
				z.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
				z.Texture2DMSArray.FirstArraySlice = dd.range.baseLayer;
				z.Texture2DMSArray.ArraySize = dd.range.layerCount;
				break;

			default:
				BreakIfDebugging();
				return Result::Unsupported;
			}

			impl->pNativeDevice->CreateDepthStencilView(T->res.Get(), &z, dst);
			return Result::Ok;
		}

		static Result d_createCommandAllocator(Device* d, QueueKind q, CommandAllocatorPtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			ID3D12Device* createDevice = impl->steamlineInitialized ? impl->pSLProxyDevice.Get() : impl->pNativeDevice.Get();
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> a;
			if (const auto hr = createDevice->CreateCommandAllocator(ToDX(q), IID_PPV_ARGS(&a)); FAILED(hr)) {
				spdlog::error("DX12 create command allocator failed queue={} hr=0x{:08X}", static_cast<int>(q), static_cast<unsigned>(hr));
				BreakIfDebugging();
				return ToRHI(hr);
			}

			Dx12Allocator A(a, ToDX(q), impl);
			const auto h = impl->allocators.alloc(A);

			CommandAllocator ret{ h };
			ret.impl = impl;
			ret.vt = &g_calvt;
			out = MakeCommandAllocatorPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyCommandAllocator(DeviceDeletionContext* d, CommandAllocator* ca) noexcept {
			dx12_detail::Dev(d)->allocators.free(ca->GetHandle());
		}

		static Result d_createCommandList(Device* d, QueueKind q, CommandAllocator ca, CommandListPtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			ID3D12Device* createDevice = impl->steamlineInitialized ? impl->pSLProxyDevice.Get() : impl->pNativeDevice.Get();
			const auto* A = dx12_detail::Alloc(&ca);
			if (!A) {
				RHI_FAIL(Result::InvalidArgument);
			}

			ComPtr<ID3D12GraphicsCommandList> cl0; // Needs at least version 10 for work graphs
			if (const auto hr = createDevice->CreateCommandList(0, A->type, A->alloc.Get(), nullptr, IID_PPV_ARGS(&cl0)); FAILED(hr)) {
				spdlog::error("DX12 create command list CreateCommandList failed queue={} hr=0x{:08X}", static_cast<int>(q), static_cast<unsigned>(hr));
				RHI_FAIL(ToRHI(hr));
			}

			// Attempt upcast to ID3D12GraphicsCommandList10
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10> cl;
			if (const auto hr = cl0.As(&cl); FAILED(hr)) {
				spdlog::error("DX12 create command list As(ID3D12GraphicsCommandList10) failed queue={} hr=0x{:08X}", static_cast<int>(q), static_cast<unsigned>(hr));
				RHI_FAIL(ToRHI(hr));
			}
			
			const Dx12CommandList rec(cl, A->alloc, A->type, impl);
			Dx12CommandList recWithScratch = rec;
			if (!Dx12EnsureRootCbvScratchPage(impl, recWithScratch, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)) {
				spdlog::error("DX12 create command list EnsureRootCbvScratchPage failed queue={}", static_cast<int>(q));
				RHI_FAIL(Result::Failed);
			}
			const auto h = impl->commandLists.alloc(recWithScratch);

			CommandList ret{ h };
			ret.impl = impl;
			ret.vt = &g_clvt;
			out = MakeCommandListPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createCommittedBuffer(Device* d, const ResourceDesc& bd, ResourcePtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice || bd.buffer.sizeBytes == 0) {
				RHI_FAIL(Result::InvalidArgument);
			}

			D3D12_HEAP_PROPERTIES hp{};
			hp.Type = ToDX(bd.heapType);
			hp.CreationNodeMask = 1;
			hp.VisibleNodeMask = 1;

			const auto hf = ToDX(bd.heapFlags);

			const D3D12_RESOURCE_FLAGS flags = ToDX(bd.resourceFlags);
			const D3D12_RESOURCE_DESC1  desc = MakeBufferDesc1(bd.buffer.sizeBytes, flags);

			Microsoft::WRL::ComPtr<ID3D12Resource> res;
			// Buffers must use UNDEFINED layout per spec
			constexpr D3D12_BARRIER_LAYOUT initialLayout = D3D12_BARRIER_LAYOUT_UNDEFINED;

			std::vector<DXGI_FORMAT> castableFormats;
			for (const auto& fmt : bd.castableFormats) {
				castableFormats.push_back(ToDxgi(fmt));
			}

			const HRESULT hr = impl->pNativeDevice->CreateCommittedResource3(
				&hp,
				hf,
				&desc,
				initialLayout,
				/*pOptimizedClearValue*/ nullptr,        // buffers: must be null
				/*pProtectedSession*/   nullptr,
				/*NumCastableFormats*/   static_cast<uint32_t>(castableFormats.size()),
				/*pCastableFormats*/     castableFormats.data(),
				IID_PPV_ARGS(&res));
			if (FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}

			if (bd.debugName) {
				res->SetName(std::wstring(bd.debugName, bd.debugName + ::strlen(bd.debugName)).c_str());
			}

			Dx12Resource B(std::move(res), bd.buffer.sizeBytes, impl);
			const auto handle = impl->resources.alloc(B);

			Resource ret{ handle, false };
			ret.impl = impl;
			ret.vt = &g_buf_rvt;
			out = MakeBufferPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createCommittedTexture(Device* d, const ResourceDesc& td, ResourcePtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice || td.texture.width == 0 || td.texture.height == 0 || td.texture.format == Format::Unknown) {
				RHI_FAIL(Result::InvalidArgument);
			}

			D3D12_HEAP_PROPERTIES hp{};
			hp.Type = ToDX(td.heapType);
			hp.CreationNodeMask = 1;
			hp.VisibleNodeMask = 1;

			const auto hf = ToDX(td.heapFlags);

			const D3D12_RESOURCE_DESC1 desc = MakeTexDesc1(td);

			D3D12_CLEAR_VALUE* pClear = nullptr;
			D3D12_CLEAR_VALUE clear;
			if (td.texture.optimizedClear) {
				clear = ToDX(*td.texture.optimizedClear);
				pClear = &clear;
			}
			// Textures can specify InitialLayout (enhanced barriers)
			const D3D12_BARRIER_LAYOUT initialLayout = ToDX(td.texture.initialLayout);

			std::vector<DXGI_FORMAT> castableFormats;
			for (const auto& fmt : td.castableFormats) {
				castableFormats.push_back(ToDxgi(fmt));
			}

			Microsoft::WRL::ComPtr<ID3D12Resource> res;
			HRESULT hr = impl->pNativeDevice->CreateCommittedResource3(
				&hp,
				hf,
				&desc,
				initialLayout,
				pClear,
				/*pProtectedSession*/ nullptr,
				/*NumCastableFormats*/ static_cast<uint32_t>(castableFormats.size()),
				/*pCastableFormats*/   castableFormats.data(),
				IID_PPV_ARGS(&res));
			if (FAILED(hr)) {
				spdlog::error("Failed to create committed texture: {0}", hr);
				RHI_FAIL(ToRHI(hr));
			};

			if (td.debugName) res->SetName(std::wstring(td.debugName, td.debugName + ::strlen(td.debugName)).c_str());

			const auto arraySize = td.type == ResourceType::Texture3D ? 1 : td.texture.depthOrLayers;
			const auto depth = td.type == ResourceType::Texture3D ? td.texture.depthOrLayers : 1;
			Dx12Resource T(std::move(res), desc.Format, td.texture.width, td.texture.height,
				td.texture.mipLevels, arraySize, (td.type == ResourceType::Texture3D)
				? D3D12_RESOURCE_DIMENSION_TEXTURE3D
				: (td.type == ResourceType::Texture2D ? D3D12_RESOURCE_DIMENSION_TEXTURE2D
					: D3D12_RESOURCE_DIMENSION_TEXTURE1D), depth, impl);

			auto handle = impl->resources.alloc(T);

			Resource ret{ handle, true };
			ret.impl = impl;
			ret.vt = &g_tex_rvt;

			out = MakeTexturePtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createCommittedResource(Device* d, const ResourceDesc& td, ResourcePtr& out) noexcept {
			switch (td.type) {
			case ResourceType::Buffer:  return d_createCommittedBuffer(d, td, out);
			case ResourceType::Texture3D:
			case ResourceType::Texture2D:
			case ResourceType::Texture1D:
				return d_createCommittedTexture(d, td, out);
			case ResourceType::Unknown: {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}
			}
			BreakIfDebugging();
			return Result::Unexpected;
		}

		static uint32_t d_getDescriptorHandleIncrementSize(Device* d, DescriptorHeapType type) noexcept {
			const auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice) return 0;
			const D3D12_DESCRIPTOR_HEAP_TYPE t = ToDX(type);
			return (uint32_t)impl->pNativeDevice->GetDescriptorHandleIncrementSize(t);
		}

		static Result d_createTimeline(Device* d, uint64_t initial, const char* dbg, TimelinePtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			Microsoft::WRL::ComPtr<ID3D12Fence> f;
			if (const auto hr = impl->pNativeDevice->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)); FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}
			if (dbg) { std::wstring w(dbg, dbg + ::strlen(dbg)); f->SetName(w.c_str()); }
			const Dx12Timeline T(f, impl);
			const auto h = impl->timelines.alloc(T);
			Timeline ret{ h };
			ret.impl = impl;
			ret.vt = &g_tlvt;
			out = MakeTimelinePtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}


		static void d_destroyTimeline(DeviceDeletionContext* d, TimelineHandle t) noexcept {
			dx12_detail::Dev(d)->timelines.free(t);
		}

		static Result d_createHeap(const Device* d, const HeapDesc& hd, HeapPtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice || hd.sizeBytes == 0) {
				RHI_FAIL(Result::InvalidArgument);
			}
			D3D12_HEAP_PROPERTIES props{};
			props.Type = ToDX(hd.memory);
			props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			props.CreationNodeMask = 1;
			props.VisibleNodeMask = 1;

			D3D12_HEAP_DESC desc{};
			desc.SizeInBytes = hd.sizeBytes;
			desc.Alignment = (hd.alignment ? hd.alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
			desc.Properties = props;
			desc.Flags = ToDX(hd.flags);

			Microsoft::WRL::ComPtr<ID3D12Heap> heap;
			if (const auto hr = impl->pNativeDevice->CreateHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr)) {
				return ToRHI(hr);
			}

#ifdef _WIN32
			if (hd.debugName) { const std::wstring w(hd.debugName, hd.debugName + ::strlen(hd.debugName)); heap->SetName(w.c_str()); }
#endif

			const Dx12Heap H(heap, hd.sizeBytes, impl);
			const auto h = impl->heaps.alloc(H);
			Heap ret{ h };
			ret.impl = impl;
			ret.vt = &g_hevt;
			out = MakeHeapPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyHeap(DeviceDeletionContext* d, HeapHandle h) noexcept {
			dx12_detail::Dev(d)->heaps.free(h);
		}

		static void d_setNameBuffer(Device* d, ResourceHandle b, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* B = impl->resources.get(b)) {
				std::wstring w(n, n + ::strlen(n));
				B->res->SetName(w.c_str());
			}
		}

		static void d_setNameTexture(Device* d, ResourceHandle t, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* T = impl->resources.get(t)) {
				std::wstring w(n, n + ::strlen(n));
				T->res->SetName(w.c_str());
			}
		}

		static void d_setNameSampler(Device* d, SamplerHandle s, const char* n) noexcept {
			return; // TODO?
		}

		static void d_setNamePipelineLayout(Device* d, PipelineLayoutHandle p, const char* n) noexcept {
			return; // TODO?
		}

		static void d_setNamePipeline(Device* d, PipelineHandle p, const char* n) noexcept {
			if (!n) return;
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* P = impl->pipelines.get(p)) {
				std::wstring w(n, n + ::strlen(n));
				P->pso->SetName(w.c_str());
			}
		}

		static void d_setNameCommandSignature(Device* d, CommandSignatureHandle cs, const char* n) noexcept {
			if (!n) return;
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* CS = impl->commandSignatures.get(cs)) {
				std::wstring w(n, n + ::strlen(n));
				CS->sig->SetName(w.c_str());
			}
		}

		static void d_setNameDescriptorHeap(Device* d, DescriptorHeapHandle dh, const char* n) noexcept {
			if (!n) return;
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* DH = impl->descHeaps.get(dh)) {
				std::wstring w(n, n + ::strlen(n));
				DH->heap->SetName(w.c_str());
			}
		}

		static void d_setNameTimeline(Device* d, TimelineHandle t, const char* n) noexcept {
			if (!n) return;
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* TL = impl->timelines.get(t)) {
				std::wstring w(n, n + ::strlen(n));
				TL->fence->SetName(w.c_str());
			}
		}

		static void d_setNameHeap(Device* d, HeapHandle h, const char* n) noexcept {
			if (!n) return;
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (auto* H = impl->heaps.get(h)) {
				std::wstring w(n, n + ::strlen(n));
				H->heap->SetName(w.c_str());
			}
		}

		static Result d_createPlacedTexture(Device* d, HeapHandle hh, uint64_t offset, const ResourceDesc& td, ResourcePtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}
			const auto* H = impl->heaps.get(hh); 
			if (!H || !H->heap) {
				RHI_FAIL(Result::InvalidArgument);
			}
			if (td.texture.width == 0 || td.texture.height == 0 || td.texture.format == Format::Unknown) {
				RHI_FAIL(Result::InvalidArgument);
			}
			const D3D12_RESOURCE_DESC1 desc = MakeTexDesc1(td);
			D3D12_CLEAR_VALUE* pClear = nullptr;
			D3D12_CLEAR_VALUE clear;
			if (td.texture.optimizedClear) {
				clear = ToDX(*td.texture.optimizedClear);
				pClear = &clear;
			}
			// Textures can specify InitialLayout (enhanced barriers)
			const D3D12_BARRIER_LAYOUT initialLayout = ToDX(td.texture.initialLayout);

			std::vector<DXGI_FORMAT> castableFormats;
			for (const auto& fmt : td.castableFormats) {
				castableFormats.push_back(ToDxgi(fmt));
			}

			Microsoft::WRL::ComPtr<ID3D12Resource> res;
			const HRESULT hr = impl->pNativeDevice->CreatePlacedResource2(
				H->heap.Get(),
				offset,
				&desc,
				initialLayout,
				pClear,
				/*numCastableFormats*/ static_cast<uint32_t>(castableFormats.size()),
				/*pProtectedSession*/ castableFormats.data(),
				IID_PPV_ARGS(&res));
			if (FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}
			if (td.debugName) res->SetName(std::wstring(td.debugName, td.debugName + ::strlen(td.debugName)).c_str());
			Dx12Resource T(std::move(res), desc.Format, td.texture.width, td.texture.height,
				td.texture.mipLevels, (td.type == ResourceType::Texture3D) ? 1 : td.texture.depthOrLayers,
				(td.type == ResourceType::Texture3D) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
				: (td.type == ResourceType::Texture2D ? D3D12_RESOURCE_DIMENSION_TEXTURE2D
					: D3D12_RESOURCE_DIMENSION_TEXTURE1D),
				(td.type == ResourceType::Texture3D) ? td.texture.depthOrLayers : 1,
				impl);

			const auto handle = impl->resources.alloc(T);
			Resource ret(handle, true);
			ret.impl = impl;
			ret.vt = &g_tex_rvt;
			out = MakeTexturePtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createPlacedBuffer(Device* d, const HeapHandle hh, const uint64_t offset, const ResourceDesc& bd, ResourcePtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}
			const auto* H = impl->heaps.get(hh); if (!H || !H->heap) {
				RHI_FAIL(Result::InvalidArgument);
			}
			if (bd.buffer.sizeBytes == 0) {
				return Result::InvalidArgument;
			}
			const D3D12_RESOURCE_FLAGS flags = ToDX(bd.resourceFlags);
			const D3D12_RESOURCE_DESC1  desc = MakeBufferDesc1(bd.buffer.sizeBytes, flags);
			// Buffers must use UNDEFINED layout per spec
			const D3D12_BARRIER_LAYOUT initialLayout = D3D12_BARRIER_LAYOUT_UNDEFINED;

			std::vector<DXGI_FORMAT> castableFormats;
			for (const auto& fmt : bd.castableFormats) {
				castableFormats.push_back(ToDxgi(fmt));
			}

			Microsoft::WRL::ComPtr<ID3D12Resource> res;
			HRESULT hr = impl->pNativeDevice->CreatePlacedResource2(
				H->heap.Get(),
				offset,
				&desc,
				initialLayout,
				/*pOptimizedClearValue*/ nullptr,        // buffers: must be null
				/*numCastableFormats*/   static_cast<uint32_t>(castableFormats.size()),
				/*pProtectedSession*/   castableFormats.data(),
				IID_PPV_ARGS(&res));
			if (FAILED(hr)) {
				spdlog::info("?");
				//RHI_FAIL(ToRHI(hr));
			}
			if (bd.debugName) res->SetName(std::wstring(bd.debugName, bd.debugName + ::strlen(bd.debugName)).c_str());
			const Dx12Resource B(std::move(res), bd.buffer.sizeBytes, impl);
			const auto handle = impl->resources.alloc(B);
			Resource ret{ handle, false };
			ret.impl = impl;
			ret.vt = &g_buf_rvt;
			out = MakeBufferPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static Result d_createPlacedResource(Device* d, const HeapHandle hh, const uint64_t offset, const ResourceDesc& rd, ResourcePtr& out) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}
			auto* H = impl->heaps.get(hh); if (!H || !H->heap) {
				RHI_FAIL(Result::InvalidArgument);
			}
			switch (rd.type) {
			case ResourceType::Buffer:  return d_createPlacedBuffer(d, hh, offset, rd, out);
			case ResourceType::Texture3D:
			case ResourceType::Texture2D:
			case ResourceType::Texture1D:
				return d_createPlacedTexture(d, hh, offset, rd, out);
			case ResourceType::Unknown: {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}
			}
			BreakIfDebugging();
			return Result::Unexpected;
		}

		static Result d_createQueryPool(Device* d, const QueryPoolDesc& qd, QueryPoolPtr& out) noexcept {
			auto* dimpl = static_cast<Dx12Device*>(d->impl);
			if (!dimpl || qd.count == 0) {
				RHI_FAIL(Result::InvalidArgument);
			}
			D3D12_QUERY_HEAP_DESC desc{};
			desc.Count = qd.count;

			D3D12_QUERY_HEAP_TYPE type;
			bool usePSO1 = false;

			switch (qd.type) {
			case QueryType::Timestamp:
				desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
				type = desc.Type; break;

			case QueryType::Occlusion:
				desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
				type = desc.Type; break;

			case QueryType::PipelineStatistics: {
				// If mesh/task bits requested and supported -> use *_STATISTICS1
				bool needMesh = (qd.statsMask & (PS_TaskInvocations | PS_MeshInvocations | PS_MeshPrimitives)) != 0;

				D3D12_FEATURE_DATA_D3D12_OPTIONS9 opts9{};
				bool haveOpt9 = SUCCEEDED(dimpl->pNativeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &opts9, sizeof(opts9)));
				bool canMeshStats = haveOpt9 && !!opts9.MeshShaderPipelineStatsSupported;

				if (needMesh && !canMeshStats && qd.requireAllStats) {
					RHI_FAIL(Result::InvalidArgument);
				}

				desc.Type = needMesh && canMeshStats
					? D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1
					: D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
				type = desc.Type;
				usePSO1 = (desc.Type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1);
			} break;
			}

			Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
			if (const auto hr = dimpl->pNativeDevice->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr)) {
				RHI_FAIL(ToRHI(hr));
			}
			Dx12QueryPool qp(heap, type, qd.count, dimpl);
			qp.usePSO1 = usePSO1;

			const auto handle = dimpl->queryPools.alloc(qp);
			QueryPool ret{ handle };
			ret.impl = dimpl;
			ret.vt = &g_qpvt;

			out = MakeQueryPoolPtr(d, ret, static_cast<Dx12Device*>(d->impl)->selfWeak.lock());
			return Result::Ok;
		}

		static void d_destroyQueryPool(DeviceDeletionContext* d, QueryPoolHandle h) noexcept {
			dx12_detail::Dev(d)->queryPools.free(h);
		}

		static TimestampCalibration d_getTimestampCalibration(Device* d, QueueKind q) noexcept {
			auto* impl = static_cast<Dx12Device*>(d->impl);
			QueueHandle h = (q == QueueKind::Graphics) ? impl->gfxHandle : (q == QueueKind::Compute ? impl->compHandle : impl->copyHandle);
			auto* s = impl->queues.get(h);
			UINT64 freq = 0;
			if (s && s->pNativeQueue) s->pNativeQueue->GetTimestampFrequency(&freq);
			return { freq };
		}

		static CopyableFootprintsInfo d_getCopyableFootprints(
			Device* d,
			const FootprintRangeDesc& in,
			CopyableFootprint* out,
			uint32_t outCap) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !out || outCap == 0) {
				BreakIfDebugging();
				return {};
			}

			auto* T = impl->resources.get(in.texture);
			if (!T || !T->res) {
				BreakIfDebugging();
				return {};
			}
			const D3D12_RESOURCE_DESC desc = T->res->GetDesc();

			// Resource-wide properties
			const UINT mipLevels = desc.MipLevels;
			const UINT arrayLayers = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 1u : desc.DepthOrArraySize;

			// Plane count per DXGI format
			const UINT resPlaneCount = D3D12GetFormatPlaneCount(impl->pNativeDevice.Get(), desc.Format);

			// Clamp input range to resource
			const UINT firstMip = std::min<UINT>(in.firstMip, mipLevels ? mipLevels - 1u : 0u);
			const UINT mipCount = std::min<UINT>(in.mipCount, mipLevels - firstMip);
			const UINT firstArray = std::min<UINT>(in.firstArraySlice, arrayLayers ? arrayLayers - 1u : 0u);
			const UINT arrayCount = std::min<UINT>(in.arraySize, arrayLayers - firstArray);
			const UINT firstPlane = std::min<UINT>(in.firstPlane, resPlaneCount ? resPlaneCount - 1u : 0u);
			const UINT planeCount = std::min<UINT>(in.planeCount, resPlaneCount - firstPlane);

			if (mipCount == 0 || arrayCount == 0 || planeCount == 0) {
				BreakIfDebugging();
				return {};
			}

			const UINT totalSubs = mipCount * arrayCount * planeCount;
			if (outCap < totalSubs) {
				BreakIfDebugging();
				return {}; // TODO: partial?
			}
			// D3D12 subresource layout: Mip + Array*NumMips + Plane*NumMips*ArraySize
			const UINT firstSubresource =
				firstMip + firstArray * mipLevels + firstPlane * mipLevels * arrayLayers;

			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placed(totalSubs);
			std::vector<UINT>  numRows(totalSubs);
			std::vector<UINT64> rowSizes(totalSubs);
			UINT64 totalBytes = 0;

			impl->pNativeDevice->GetCopyableFootprints(
				&desc,
				firstSubresource,
				totalSubs,
				in.baseOffset,
				placed.data(),
				numRows.data(),
				rowSizes.data(),
				&totalBytes);

			// Pack back into RHI-friendly structure
			for (UINT i = 0; i < totalSubs; ++i) {
				const auto& f = placed[i].Footprint;
				out[i].offset = placed[i].Offset;
				out[i].rowPitch = f.RowPitch;     // bytes
				out[i].height = f.Height;       // texel rows used for the copy
				out[i].width = f.Width;        // texels
				out[i].depth = f.Depth;        // slices for 3D (else 1)
			}

			return { totalSubs, totalBytes };
		}

		static Result d_getResourceAllocationInfo(
			const Device* d,
			const ResourceDesc* resources,
			uint32_t resourceCount,
			ResourceAllocationInfo* outInfos) noexcept
		{
			// Validate inputs
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !resources || resourceCount == 0 || !outInfos) {
				RHI_FAIL(Result::InvalidArgument);
			}
			// TODO: Should we store this elsewhere to avoid reallocating every call?
			std::vector<D3D12_RESOURCE_DESC1> descs;
			descs.resize(resourceCount);
			for (size_t i = 0; i < resourceCount; ++i) {
				switch (resources[i].type) {
				case ResourceType::Buffer:
					descs[i] = MakeBufferDesc1(resources[i].buffer.sizeBytes, ToDX(resources[i].resourceFlags));
					break;
				case ResourceType::Texture1D:
				case ResourceType::Texture2D:
				case ResourceType::Texture3D:
					descs[i] = MakeTexDesc1(resources[i]);
					break;
				default:
					RHI_FAIL(Result::InvalidArgument);
				}
			}
			// Out array
			std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> dxInfos;
			dxInfos.resize(resourceCount);
			impl->pNativeDevice->GetResourceAllocationInfo2(0, resourceCount, descs.data(), dxInfos.data());
			// Pack back
			for (size_t i = 0; i < resourceCount; ++i) {
				outInfos[i].offset = dxInfos[i].Offset;
				outInfos[i].alignment = dxInfos[i].Alignment;
				outInfos[i].sizeInBytes = dxInfos[i].SizeInBytes;
			}
			return Result::Ok;
		}

		static Result d_setResidencyPriority(
			const Device* d,
			const Span<PageableRef> objects,
			ResidencyPriority priority) noexcept
		{
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (objects.size == 0) return Result::Ok;

			std::vector<ID3D12Pageable*> pageables;
			pageables.reserve(objects.size);

			std::vector<D3D12_RESIDENCY_PRIORITY> priorities;
			priorities.resize(objects.size, ToDX(priority));

			for (uint32_t i = 0; i < objects.size; ++i)
			{
				const PageableRef& o = objects.data[i];
				ID3D12Pageable* native = nullptr;

				switch (o.kind)
				{
				case PageableKind::Resource:
				{
					auto* R = impl->resources.get(o.resource);
					if (!R || !R->res) {
						RHI_FAIL(Result::InvalidArgument);
					}
					native = R->res.Get(); // ID3D12Resource* -> ID3D12Pageable*
				} break;

				case PageableKind::Heap:
				{
					auto* H = impl->heaps.get(o.heap);
					if (!H || !H->heap) {
						RHI_FAIL(Result::InvalidArgument);
					}
					native = H->heap.Get(); // ID3D12Heap* -> ID3D12Pageable*
				} break;

				case PageableKind::DescriptorHeap:
				{
					auto* DH = impl->descHeaps.get(o.descHeap);
					if (!DH || !DH->heap) {
						RHI_FAIL(Result::InvalidArgument);
					}
					native = DH->heap.Get(); // ID3D12DescriptorHeap* -> ID3D12Pageable*
				} break;

				case PageableKind::QueryPool:
				{
					auto* QH = impl->queryPools.get(o.queryPool);
					if (!QH || !QH->heap) {
						RHI_FAIL(Result::InvalidArgument);
					}
					native = QH->heap.Get(); // ID3D12QueryHeap* -> ID3D12Pageable*
				} break;

				case PageableKind::Pipeline:
				{
					auto* P = impl->pipelines.get(o.pipeline);
					if (!P || !P->pso) {
						RHI_FAIL(Result::InvalidArgument);
					}
					native = P->pso.Get(); // ID3D12PipelineState* -> ID3D12Pageable*
				} break;

				default:
					RHI_FAIL(Result::InvalidArgument);
				}

				// Defensive (should never happen if the above is correct)
				if (!native) {
					RHI_FAIL(Result::InvalidArgument);
				}

				pageables.push_back(native);
			}

			const HRESULT hr = impl->pNativeDevice->SetResidencyPriority(
				static_cast<UINT>(pageables.size()),
				pageables.data(),
				priorities.data());

			if (FAILED(hr)) return ToRHI(hr);
			return Result::Ok;
		}

		// ---------------- Queue vtable funcs ----------------
		static Result q_submit(Queue* q, Span<CommandList> lists, const SubmitDesc& s) noexcept {
			auto* qs = dx12_detail::QState(q);
			auto* dev = qs->dev;
			if (!dev) {
				RHI_FAIL(Result::InvalidArgument);
			}

			// Pre-waits
			for (auto& w : s.waits) {
				auto* TL = dev->timelines.get(w.t); if (!TL) {
					RHI_FAIL(Result::InvalidArgument);
				}
				if (FAILED(qs->pNativeQueue->Wait(TL->fence.Get(), w.value))) {
					RHI_FAIL(Result::InvalidArgument);
				}
			}

			// Execute command lists
			std::vector<ID3D12CommandList*> native; native.reserve(lists.size);
			for (auto& L : lists) {
				auto* w = dx12_detail::CL(&L);
				native.push_back(w->cl.Get());
			}
			if (!native.empty()) qs->pNativeQueue->ExecuteCommandLists(static_cast<uint32_t>(native.size()), native.data());

			// Post-signals
			for (auto& sgn : s.signals) {
				auto* TL = dev->timelines.get(sgn.t); if (!TL) {
					RHI_FAIL(Result::InvalidArgument);
				}
				if (sgn.value == 0) {
					spdlog::error(
						"q_submit post-signal: attempted to signal timeline(index={}, gen={}) "
						"with value 0 via SubmitDesc. Break to inspect callstack.",
						sgn.t.index, sgn.t.generation);
					BreakIfDebugging();
					continue;
				}
#if BUILD_TYPE == BUILD_DEBUG
				{
					auto last = qs->lastSignaledValue.find(sgn.t);
					if (last != qs->lastSignaledValue.end() && sgn.value <= last->second) {
						spdlog::error(
							"q_submit post-signal monotonicity violation: timeline(index={}, gen={}) "
							"attempted value={} but lastSignaled={}",
							sgn.t.index, sgn.t.generation,
							sgn.value, last->second);
						BreakIfDebugging();
						continue;
					}
					qs->lastSignaledValue[sgn.t] = sgn.value;
				}
#endif
				if (const auto hr = qs->pNativeQueue->Signal(TL->fence.Get(), sgn.value); FAILED(hr)) {
					RHI_FAIL(ToRHI(hr));
				}
			}
			return Result::Ok;
		}

		static void q_checkDebugMessages(Queue* q) noexcept {
			auto* qs = dx12_detail::QState(q);
			if (!qs || !qs->dev) {
				return;
			}
			Dx12PollReShapeMessages(qs->dev);
			LogDredData();
		}

		static void Dx12CopyDebugString(char* dst, size_t dstSize, const char* src) noexcept {
			if (!dst || dstSize == 0) {
				return;
			}

			dst[0] = '\0';
			if (!src) {
				return;
			}

			const size_t srcLen = std::strlen(src);
			const size_t copyLen = (std::min)(srcLen, dstSize - 1);
			std::memcpy(dst, src, copyLen);
			dst[copyLen] = '\0';
		}

		static bool Dx12TryParseIssueUid(const char* message, const char* prefix, uint64_t& uid) noexcept {
			if (!message || !prefix) {
				return false;
			}

			const size_t prefixLength = std::strlen(prefix);
			if (std::strncmp(message, prefix, prefixLength) != 0) {
				return false;
			}

			const char* first = message + prefixLength;
			const char* last = first;
			while (*last >= '0' && *last <= '9') {
				++last;
			}

			if (first == last) {
				return false;
			}

			uid = 0;
			const auto result = std::from_chars(first, last, uid);
			return result.ec == std::errc{};
		}

		static std::string Dx12NormalizeShaderPath(std::string path) {
			std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
				return value == '\\'
					? '/'
					: static_cast<char>(std::tolower(value));
			});
			return path;
		}

		static bool Dx12IsPrimaryShaderSourcePath(const std::string& path) {
			if (path.empty()) {
				return false;
			}

			const std::string normalized = Dx12NormalizeShaderPath(path);
			const auto extensionIndex = normalized.find_last_of('.');
			const std::string extension = extensionIndex == std::string::npos
				? std::string{}
				: normalized.substr(extensionIndex);
			if (normalized.ends_with(".dxil.txt")
				|| normalized.ends_with(".module.txt")
				|| normalized.find("gpu-reshape/") != std::string::npos
				|| normalized.find("/source/backends/dx12/") != std::string::npos
				|| normalized.find("/source/backends/vulkan/") != std::string::npos
				|| normalized.find("/modules/") != std::string::npos
				|| normalized.find("/generated/") != std::string::npos
				|| normalized.find("/unknown/") != std::string::npos) {
				return false;
			}

			return extension == ".hlsl"
				|| extension == ".hlsli"
				|| extension == ".fx"
				|| extension == ".fxh"
				|| extension == ".slang"
				|| extension == ".glsl"
				|| extension == ".comp"
				|| extension == ".vert"
				|| extension == ".frag";
		}

		static std::string Dx12NormalizeIssueMessageForDeduplication(std::string_view input) {
			std::string normalized(input);
			constexpr std::string_view kExecutionToken = "execution ";
			size_t searchOffset = 0;
			while ((searchOffset = normalized.find(kExecutionToken, searchOffset)) != std::string::npos) {
				size_t digitOffset = searchOffset + kExecutionToken.size();
				size_t digitEnd = digitOffset;
				while (digitEnd < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[digitEnd])) != 0) {
					++digitEnd;
				}
				if (digitEnd > digitOffset) {
					normalized.replace(digitOffset, digitEnd - digitOffset, "#");
					searchOffset = digitOffset + 1;
				} else {
					searchOffset = digitOffset;
				}
			}
			return normalized;
		}

		static void Dx12AppendRollingExecutionUid(std::vector<uint32_t>& rollingExecutionUids, uint32_t rollingExecutionUid) {
			if (!rollingExecutionUid) {
				return;
			}

			if (std::find(rollingExecutionUids.begin(), rollingExecutionUids.end(), rollingExecutionUid) == rollingExecutionUids.end()) {
				rollingExecutionUids.push_back(rollingExecutionUid);
			}
		}

		static std::string Dx12BuildExecutionDetailRetentionKey(const Dx12InstrumentationExecutionDetail& detail) {
			std::string key;
			key.reserve(256);
			key += std::to_string(static_cast<uint32_t>(detail.severity));
			key += '|';
			key += std::to_string(static_cast<uint32_t>(detail.kind));
			key += '|';
			key += std::to_string(detail.shaderUid);
			key += '|';
			key += std::to_string(detail.pipelineUid);
			key += '|';
			key += std::to_string(detail.sguid);
			key += '|';
			key += Dx12NormalizeIssueMessageForDeduplication(detail.message);
			return key;
		}

		template <typename TTraceback>
		static Dx12InstrumentationTracebackDetail Dx12BuildTracebackDetail(const TTraceback* traceback) {
			Dx12InstrumentationTracebackDetail detail{};
			if (!traceback) {
				return detail;
			}

			detail.valid = true;
			detail.executionFlag = traceback->executionFlag;
			detail.rollingExecutionUid = traceback->rollingExecutionUID;
			detail.pipelineUid = traceback->pipelineUid;
			std::copy(std::begin(traceback->markerHashes32), std::end(traceback->markerHashes32), detail.markerHashes32.begin());
			detail.queueUid = traceback->queueUid;
			detail.kernelLaunch = { traceback->kernelLaunchX, traceback->kernelLaunchY, traceback->kernelLaunchZ };
			detail.thread = { traceback->threadX, traceback->threadY, traceback->threadZ };
			return detail;
		}

		static void Dx12AppendExecutionDetailUnlocked(Dx12DebugInstrumentationSession& session, Dx12InstrumentationExecutionDetail detail) {
			constexpr size_t kMaxExecutionDetails = 2048;
			constexpr size_t kMaxArchivedExecutionDetailsPerIssue = 16;
			if (!detail.detailId) {
				detail.detailId = session.nextExecutionDetailId++;
			}

			const std::string retentionKey = Dx12BuildExecutionDetailRetentionKey(detail);
			auto& archivedDetails = session.archivedExecutionDetailsByKey[retentionKey];
			if (archivedDetails.size() >= kMaxArchivedExecutionDetailsPerIssue) {
				archivedDetails.pop_front();
			}
			archivedDetails.push_back(detail);

			if (session.executionDetails.size() >= kMaxExecutionDetails) {
				session.executionDetails.pop_front();
			}
			session.executionDetails.push_back(std::move(detail));
		}

		static bool Dx12RetainExecutionDetailUnlocked(Dx12DebugInstrumentationSession& session, uint64_t detailId) {
			if (!detailId) {
				return false;
			}

			if (session.retainedExecutionDetails.contains(detailId)) {
				return true;
			}

			auto retainIfMatch = [&](const Dx12InstrumentationExecutionDetail& detail) {
				if (detail.detailId != detailId) {
					return false;
				}
				session.retainedExecutionDetails.emplace(detailId, detail);
				return true;
			};

			for (auto it = session.executionDetails.rbegin(); it != session.executionDetails.rend(); ++it) {
				if (retainIfMatch(*it)) {
					return true;
				}
			}

			for (const auto& [key, details] : session.archivedExecutionDetailsByKey) {
				(void)key;
				for (auto it = details.rbegin(); it != details.rend(); ++it) {
					if (retainIfMatch(*it)) {
						return true;
					}
				}
			}

			return false;
		}

		static uint64_t Dx12ResolveExecutionDetailShaderUidUnlocked(
			const Dx12DebugInstrumentationSession& session,
			const Dx12InstrumentationExecutionDetail& detail) noexcept {
			if (detail.shaderUid != 0) {
				return detail.shaderUid;
			}

			if (detail.sguid == 0) {
				return 0;
			}

			const auto mappingIt = session.shaderSourceMappings.find(detail.sguid);
			if (mappingIt != session.shaderSourceMappings.end() && mappingIt->second.resolved) {
				return mappingIt->second.shaderGuid;
			}

			return 0;
		}

		static std::string Dx12ResolveExecutionDetailPipelineLabelUnlocked(
			const Dx12DebugInstrumentationSession& session,
			uint64_t pipelineUid) {
			if (pipelineUid == 0) {
				return "Unknown pipeline";
			}

			const auto pipelineNameIt = session.pipelineNames.find(pipelineUid);
			if (pipelineNameIt != session.pipelineNames.end() && !pipelineNameIt->second.empty()) {
				return pipelineNameIt->second;
			}

			char label[64] = {};
			std::snprintf(label, sizeof(label), "Pipeline %llu", static_cast<unsigned long long>(pipelineUid));
			return label;
		}

		static bool Dx12ExecutionDetailHasPathUnlocked(
			const Dx12DebugInstrumentationSession& session,
			const Dx12InstrumentationExecutionDetail& detail,
			std::string_view expectedPath) {
			if (expectedPath.empty()) {
				return true;
			}

			const std::string expected(expectedPath);
			const uint64_t resolvedShaderUid = Dx12ResolveExecutionDetailShaderUidUnlocked(session, detail);
			const auto metadataIt = resolvedShaderUid != 0
				? session.shaderMetadata.find(resolvedShaderUid)
				: session.shaderMetadata.end();

			if (detail.sguid != 0) {
				const auto mappingIt = session.shaderSourceMappings.find(detail.sguid);
				if (mappingIt != session.shaderSourceMappings.end() && mappingIt->second.resolved && metadataIt != session.shaderMetadata.end()) {
					const auto exactPathIt = metadataIt->second.filePathsByUid.find(mappingIt->second.fileUid);
					if (exactPathIt != metadataIt->second.filePathsByUid.end() && exactPathIt->second == expected) {
						return true;
					}
				}
			}

			if (metadataIt == session.shaderMetadata.end()) {
				return false;
			}

			return std::find(
				metadataIt->second.filePaths.begin(),
				metadataIt->second.filePaths.end(),
				expected) != metadataIt->second.filePaths.end();
		}

		static bool Dx12IssueMatchesExecutionDetailUnlocked(
			const Dx12DebugInstrumentationSession& session,
			const DebugInstrumentationIssue& issue,
			const Dx12InstrumentationExecutionDetail& detail) {
			if (issue.severity != detail.severity) {
				return false;
			}

			if (Dx12NormalizeIssueMessageForDeduplication(issue.message) != Dx12NormalizeIssueMessageForDeduplication(detail.message)) {
				return false;
			}

			if (issue.type == DebugInstrumentationIssueType::Pipeline) {
				return detail.pipelineUid != 0 && issue.objectUid == detail.pipelineUid;
			}

			if (issue.type != DebugInstrumentationIssueType::ShaderFile) {
				return false;
			}

			const uint64_t resolvedShaderUid = Dx12ResolveExecutionDetailShaderUidUnlocked(session, detail);
			const uint64_t expectedObjectUid = resolvedShaderUid != 0 ? resolvedShaderUid : detail.sguid;
			if (expectedObjectUid == 0 || issue.objectUid != expectedObjectUid) {
				return false;
			}

			if (issue.parentPipelineUid != 0 && detail.pipelineUid != 0 && issue.parentPipelineUid != detail.pipelineUid) {
				return false;
			}

			if (issue.path[0] != '\0' && !Dx12ExecutionDetailHasPathUnlocked(session, detail, issue.path)) {
				return false;
			}

			return true;
		}

		static debug::InstrumentationExecutionDetailKind Dx12MapExecutionDetailKind(Dx12InstrumentationExecutionKind kind) noexcept {
			switch (kind) {
			case Dx12InstrumentationExecutionKind::DescriptorMismatch:
				return debug::InstrumentationExecutionDetailKind::DescriptorMismatch;
			case Dx12InstrumentationExecutionKind::ResourceIndexOutOfBounds:
				return debug::InstrumentationExecutionDetailKind::ResourceIndexOutOfBounds;
			default:
				return debug::InstrumentationExecutionDetailKind::Unknown;
			}
		}

		static debug::InstrumentationExecutionDetailSnapshot Dx12BuildExecutionDetailSnapshotUnlocked(
			const Dx12DebugInstrumentationSession& session,
			const Dx12InstrumentationExecutionDetail& detail) {
			debug::InstrumentationExecutionDetailSnapshot snapshot{};
			snapshot.detailId = detail.detailId;
			snapshot.pipelineUid = detail.pipelineUid;
			snapshot.shaderUid = Dx12ResolveExecutionDetailShaderUidUnlocked(session, detail);
			snapshot.sguid = detail.sguid;
			snapshot.severity = detail.severity;
			snapshot.kind = Dx12MapExecutionDetailKind(detail.kind);
			snapshot.pipelineLabel = Dx12ResolveExecutionDetailPipelineLabelUnlocked(session, detail.pipelineUid);
			snapshot.message = detail.message;

			snapshot.traceback.valid = detail.traceback.valid;
			snapshot.traceback.executionFlag = detail.traceback.executionFlag;
			snapshot.traceback.rollingExecutionUid = detail.traceback.rollingExecutionUid;
			snapshot.traceback.queueUid = detail.traceback.queueUid;
			snapshot.traceback.markerHashes32 = detail.traceback.markerHashes32;
			snapshot.traceback.kernelLaunch = detail.traceback.kernelLaunch;
			snapshot.traceback.thread = detail.traceback.thread;
			if (detail.traceback.valid) {
				const auto stackIt = session.executionStacks.find(detail.traceback.rollingExecutionUid);
				if (stackIt != session.executionStacks.end()) {
					snapshot.traceback.hostStackTrace = stackIt->second;
				}
			}

			snapshot.descriptorMismatch.hasDetail = detail.descriptorMismatch.hasDetail;
			snapshot.descriptorMismatch.token = detail.descriptorMismatch.token;
			snapshot.descriptorMismatch.compileType = detail.descriptorMismatch.compileType;
			snapshot.descriptorMismatch.runtimeType = detail.descriptorMismatch.runtimeType;
			snapshot.descriptorMismatch.isUndefined = detail.descriptorMismatch.isUndefined;
			snapshot.descriptorMismatch.isOutOfBounds = detail.descriptorMismatch.isOutOfBounds;
			snapshot.descriptorMismatch.isTableNotBound = detail.descriptorMismatch.isTableNotBound;

			snapshot.resourceBounds.hasDetail = detail.resourceBounds.hasDetail;
			snapshot.resourceBounds.token = detail.resourceBounds.token;
			snapshot.resourceBounds.coordinate = detail.resourceBounds.coordinate;
			snapshot.resourceBounds.isTexture = detail.resourceBounds.isTexture;
			snapshot.resourceBounds.isWrite = detail.resourceBounds.isWrite;

			if (detail.sguid != 0) {
				const auto mappingIt = session.shaderSourceMappings.find(detail.sguid);
				if (mappingIt != session.shaderSourceMappings.end() && mappingIt->second.resolved) {
					snapshot.sourceLine = mappingIt->second.line;
					snapshot.sourceColumn = mappingIt->second.column;
					snapshot.sourceText = mappingIt->second.sourceLine;

					const uint64_t shaderUid = snapshot.shaderUid;
					const auto metadataIt = shaderUid != 0
						? session.shaderMetadata.find(shaderUid)
						: session.shaderMetadata.end();
					if (metadataIt != session.shaderMetadata.end()) {
						const auto exactPathIt = metadataIt->second.filePathsByUid.find(mappingIt->second.fileUid);
						if (exactPathIt != metadataIt->second.filePathsByUid.end()) {
							snapshot.sourcePath = exactPathIt->second;
						}
					}
				}
			}

			if (snapshot.sourcePath.empty() && detail.sguid != 0) {
				char buffer[64] = {};
				std::snprintf(buffer, sizeof(buffer), "SGUID %llu", static_cast<unsigned long long>(detail.sguid));
				snapshot.sourcePath = buffer;
			}

			return snapshot;
		}

		static std::vector<debug::InstrumentationExecutionDetailSnapshot> Dx12CollectExecutionDetailSnapshotsUnlocked(
			const Dx12DebugInstrumentationSession& session,
			const DebugInstrumentationIssue& issue) {
			std::vector<debug::InstrumentationExecutionDetailSnapshot> snapshots;
			std::unordered_set<uint64_t> seenDetailIds;

			auto tryAppend = [&](const Dx12InstrumentationExecutionDetail& detail) {
				if (!Dx12IssueMatchesExecutionDetailUnlocked(session, issue, detail)) {
					return;
				}

				if (!seenDetailIds.insert(detail.detailId).second) {
					return;
				}

				snapshots.push_back(Dx12BuildExecutionDetailSnapshotUnlocked(session, detail));
			};

			for (const auto& [detailId, detail] : session.retainedExecutionDetails) {
				(void)detailId;
				tryAppend(detail);
			}

			for (const auto& [key, details] : session.archivedExecutionDetailsByKey) {
				(void)key;
				for (const Dx12InstrumentationExecutionDetail& detail : details) {
					tryAppend(detail);
				}
			}

			for (const Dx12InstrumentationExecutionDetail& detail : session.executionDetails) {
				tryAppend(detail);
			}

			std::sort(
				snapshots.begin(),
				snapshots.end(),
				[](const debug::InstrumentationExecutionDetailSnapshot& lhs, const debug::InstrumentationExecutionDetailSnapshot& rhs) {
					return lhs.detailId > rhs.detailId;
				});

			return snapshots;
		}

		static bool Dx12HasPendingPipelineIssueUnlocked(const Dx12DebugInstrumentationSession& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t pipelineUid,
			const char* message) {
			const std::string normalizedMessage = Dx12NormalizeIssueMessageForDeduplication(message ? message : "");
			for (const Dx12PendingInstrumentationPipelineIssue& issue : session.pendingPipelineIssues) {
				if (issue.severity == severity
					&& issue.pipelineUid == pipelineUid
					&& Dx12NormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
					return true;
				}
			}
			return false;
		}

		static bool Dx12HasPendingShaderIssueUnlocked(const Dx12DebugInstrumentationSession& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t shaderUid,
			uint64_t pipelineUid,
			uint64_t sguid,
			const char* message) {
			const std::string normalizedMessage = Dx12NormalizeIssueMessageForDeduplication(message ? message : "");
			for (const Dx12PendingInstrumentationShaderIssue& issue : session.pendingShaderIssues) {
				if (issue.severity == severity
					&& issue.shaderUid == shaderUid
					&& issue.pipelineUid == pipelineUid
					&& issue.sguid == sguid
					&& Dx12NormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
					return true;
				}
			}
			return false;
		}

		static void Dx12QueuePendingPipelineIssueUnlocked(Dx12DebugInstrumentationSession& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t pipelineUid,
			const char* message,
			uint32_t rollingExecutionUid = 0) {
			if (!pipelineUid) {
				return;
			}

			if (!Dx12HasPendingPipelineIssueUnlocked(session, severity, pipelineUid, message)) {
				session.pendingPipelineIssues.push_back(Dx12PendingInstrumentationPipelineIssue{
					.severity = severity,
					.pipelineUid = pipelineUid,
					.message = message ? message : ""
				});
				Dx12AppendRollingExecutionUid(session.pendingPipelineIssues.back().rollingExecutionUids, rollingExecutionUid);
				session.issuesDirty = true;
			} else {
				const std::string normalizedMessage = Dx12NormalizeIssueMessageForDeduplication(message ? message : "");
				for (Dx12PendingInstrumentationPipelineIssue& issue : session.pendingPipelineIssues) {
					if (issue.severity == severity
						&& issue.pipelineUid == pipelineUid
						&& Dx12NormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
						Dx12AppendRollingExecutionUid(issue.rollingExecutionUids, rollingExecutionUid);
						break;
					}
				}
			}

			if (!session.pipelineNames.contains(pipelineUid) && !session.requestedPipelineNames.contains(pipelineUid)) {
				session.pendingPipelineNameRequests.insert(pipelineUid);
			}
		}

		static void Dx12QueuePendingShaderIssueUnlocked(Dx12DebugInstrumentationSession& session,
			DebugInstrumentationDiagnosticSeverity severity,
			uint64_t shaderUid,
			uint64_t pipelineUid,
			uint64_t sguid,
			const char* message,
			uint32_t rollingExecutionUid = 0) {
			if (!shaderUid && !sguid) {
				return;
			}

			if (!Dx12HasPendingShaderIssueUnlocked(session, severity, shaderUid, pipelineUid, sguid, message)) {
				session.pendingShaderIssues.push_back(Dx12PendingInstrumentationShaderIssue{
					.severity = severity,
					.shaderUid = shaderUid,
					.pipelineUid = pipelineUid,
					.sguid = sguid,
					.message = message ? message : ""
				});
				Dx12AppendRollingExecutionUid(session.pendingShaderIssues.back().rollingExecutionUids, rollingExecutionUid);
				session.issuesDirty = true;
			} else {
				const std::string normalizedMessage = Dx12NormalizeIssueMessageForDeduplication(message ? message : "");
				for (Dx12PendingInstrumentationShaderIssue& issue : session.pendingShaderIssues) {
					if (issue.severity == severity
						&& issue.shaderUid == shaderUid
						&& issue.pipelineUid == pipelineUid
						&& issue.sguid == sguid
						&& Dx12NormalizeIssueMessageForDeduplication(issue.message) == normalizedMessage) {
						Dx12AppendRollingExecutionUid(issue.rollingExecutionUids, rollingExecutionUid);
						break;
					}
				}
			}

			if (shaderUid) {
				Dx12ShaderIssueMetadata& metadata = session.shaderMetadata[shaderUid];
				if (!metadata.requested) {
					session.pendingShaderCodeRequests.insert(shaderUid);
				}
			}
		}

		static void Dx12MarkIssueFromMessageUnlocked(Dx12DebugInstrumentationSession& session,
			DebugInstrumentationDiagnosticSeverity severity,
			const char* message) {
			if (severity == DebugInstrumentationDiagnosticSeverity::Info || !message || !message[0]) {
				return;
			}

			uint64_t uid = 0;
			if (Dx12TryParseIssueUid(message, "Pipeline ", uid)) {
				Dx12QueuePendingPipelineIssueUnlocked(session, severity, uid, message);
				return;
			}

			if (Dx12TryParseIssueUid(message, "Shader ", uid)) {
				Dx12QueuePendingShaderIssueUnlocked(session, severity, uid, 0, 0, message);
			}
		}

		static void Dx12RebuildInstrumentationIssuesUnlocked(Dx12DebugInstrumentationSession& session) {
			if (!session.issuesDirty) {
				return;
			}

			session.issues.clear();
			std::unordered_set<std::string> keys;

			auto tryAppendIssue = [&](const DebugInstrumentationIssue& issue) {
				std::string key;
				key.reserve(512);
				key += std::to_string(static_cast<uint32_t>(issue.severity));
				key += '|';
				key += std::to_string(static_cast<uint32_t>(issue.type));
				key += '|';
				key += std::to_string(issue.objectUid);
				key += '|';
				key += issue.label;
				key += '|';
				key += issue.path;
				key += '|';
				key += Dx12NormalizeIssueMessageForDeduplication(issue.message);
				if (keys.insert(key).second) {
					session.issues.push_back(issue);
				}
			};

			for (const Dx12PendingInstrumentationPipelineIssue& pendingIssue : session.pendingPipelineIssues) {
				DebugInstrumentationIssue issue{};
				issue.severity = pendingIssue.severity;
				issue.type = DebugInstrumentationIssueType::Pipeline;
				issue.objectUid = pendingIssue.pipelineUid;
				const auto pipelineNameIt = session.pipelineNames.find(pendingIssue.pipelineUid);
				if (pipelineNameIt != session.pipelineNames.end() && !pipelineNameIt->second.empty()) {
					char label[128] = {};
					std::snprintf(
						label,
						sizeof(label),
						"%s (UID %llu)",
						pipelineNameIt->second.c_str(),
						static_cast<unsigned long long>(pendingIssue.pipelineUid));
					Dx12CopyDebugString(issue.label, sizeof(issue.label), label);
				} else {
					char label[64] = {};
					std::snprintf(label, sizeof(label), "Pipeline %llu", static_cast<unsigned long long>(pendingIssue.pipelineUid));
					Dx12CopyDebugString(issue.label, sizeof(issue.label), label);
				}
				Dx12CopyDebugString(issue.message, sizeof(issue.message), pendingIssue.message.c_str());
				tryAppendIssue(issue);
			}

			for (const Dx12PendingInstrumentationShaderIssue& pendingIssue : session.pendingShaderIssues) {
				uint64_t resolvedShaderUid = pendingIssue.shaderUid;
				if (!resolvedShaderUid && pendingIssue.sguid != 0) {
					const auto mappingIt = session.shaderSourceMappings.find(pendingIssue.sguid);
					if (mappingIt != session.shaderSourceMappings.end() && mappingIt->second.resolved && mappingIt->second.shaderGuid != 0) {
						resolvedShaderUid = mappingIt->second.shaderGuid;
						Dx12ShaderIssueMetadata& metadata = session.shaderMetadata[resolvedShaderUid];
						if (!metadata.requested) {
							session.pendingShaderCodeRequests.insert(resolvedShaderUid);
						}
					}
				}

				const auto metadataIt = resolvedShaderUid != 0
					? session.shaderMetadata.find(resolvedShaderUid)
					: session.shaderMetadata.end();
				std::vector<std::string> primaryFilePaths;
				bool hasInternalFilePaths = false;
				bool nativeNoSourceShader = false;
				const Dx12ShaderSourceMappingMetadata* resolvedMapping = nullptr;
				if (pendingIssue.sguid != 0) {
					const auto mappingIt = session.shaderSourceMappings.find(pendingIssue.sguid);
					if (mappingIt != session.shaderSourceMappings.end() && mappingIt->second.resolved) {
						resolvedMapping = &mappingIt->second;
					}
				}
				if (metadataIt != session.shaderMetadata.end()) {
					nativeNoSourceShader = metadataIt->second.nativeBinary && !metadataIt->second.hasDebugFiles;
					if (resolvedMapping && resolvedMapping->fileUid != UINT32_MAX) {
						auto exactPathIt = metadataIt->second.filePathsByUid.find(resolvedMapping->fileUid);
						if (exactPathIt != metadataIt->second.filePathsByUid.end() && Dx12IsPrimaryShaderSourcePath(exactPathIt->second)) {
							primaryFilePaths.push_back(exactPathIt->second);
						}
					}
					for (const std::string& filePath : metadataIt->second.filePaths) {
						if (std::find(primaryFilePaths.begin(), primaryFilePaths.end(), filePath) != primaryFilePaths.end()) {
							continue;
						}
						if (Dx12IsPrimaryShaderSourcePath(filePath)) {
							primaryFilePaths.push_back(filePath);
						} else {
							hasInternalFilePaths = true;
						}
					}
				}
				const bool hasSourceMapping = pendingIssue.sguid != 0
					&& session.shaderSourceMappings.contains(pendingIssue.sguid)
					&& session.shaderSourceMappings.find(pendingIssue.sguid)->second.resolved;

				if (primaryFilePaths.empty()) {
					if (nativeNoSourceShader && !hasSourceMapping && pendingIssue.sguid == 0) {
						continue;
					}

					if (hasInternalFilePaths && !hasSourceMapping && pendingIssue.sguid == 0) {
						continue;
					}

					DebugInstrumentationIssue issue{};
					issue.severity = pendingIssue.severity;
					issue.type = DebugInstrumentationIssueType::ShaderFile;
					issue.objectUid = resolvedShaderUid != 0 ? resolvedShaderUid : pendingIssue.sguid;
					issue.parentPipelineUid = pendingIssue.pipelineUid;
					char label[64] = {};
					if (resolvedShaderUid != 0) {
						std::snprintf(label, sizeof(label), "Shader %llu", static_cast<unsigned long long>(resolvedShaderUid));
					} else {
						std::snprintf(label, sizeof(label), "Shader SGUID %llu", static_cast<unsigned long long>(pendingIssue.sguid));
					}
					Dx12CopyDebugString(issue.label, sizeof(issue.label), label);
					Dx12CopyDebugString(issue.message, sizeof(issue.message), pendingIssue.message.c_str());
					tryAppendIssue(issue);
					continue;
				}

				for (const std::string& filePath : primaryFilePaths) {
					DebugInstrumentationIssue issue{};
					issue.severity = pendingIssue.severity;
					issue.type = DebugInstrumentationIssueType::ShaderFile;
					issue.objectUid = resolvedShaderUid;
					issue.parentPipelineUid = pendingIssue.pipelineUid;
					char label[64] = {};
					std::snprintf(label, sizeof(label), "Shader %llu", static_cast<unsigned long long>(resolvedShaderUid));
					Dx12CopyDebugString(issue.label, sizeof(issue.label), label);
					Dx12CopyDebugString(issue.path, sizeof(issue.path), filePath.c_str());
					Dx12CopyDebugString(issue.message, sizeof(issue.message), pendingIssue.message.c_str());
					tryAppendIssue(issue);
				}
			}

			session.issuesDirty = false;
		}

		static void Dx12AppendInstrumentationDiagnostic(
			Dx12Device* impl,
			DebugInstrumentationDiagnosticSeverity severity,
			const char* message,
			bool mirrorToSpdlog = true) noexcept {
			if (!impl) {
				return;
			}

			const char* safeMessage = message ? message : "";
			if (mirrorToSpdlog) {
				switch (severity) {
				case DebugInstrumentationDiagnosticSeverity::Info:
					spdlog::info("GPU-Reshape: {}", safeMessage);
					break;
				case DebugInstrumentationDiagnosticSeverity::Warning:
					spdlog::warn("GPU-Reshape: {}", safeMessage);
					break;
				case DebugInstrumentationDiagnosticSeverity::Error:
				default:
					spdlog::error("GPU-Reshape: {}", safeMessage);
					break;
			}
			}

			std::lock_guard guard(impl->debugInstrumentation.mutex);
			auto& diagnostics = impl->debugInstrumentation.diagnostics;

			const std::string normalizedMessage = Dx12NormalizeIssueMessageForDeduplication(safeMessage);
			for (const DebugInstrumentationDiagnostic& existing : diagnostics) {
				if (existing.severity == severity && Dx12NormalizeIssueMessageForDeduplication(existing.message) == normalizedMessage) {
					Dx12MarkIssueFromMessageUnlocked(impl->debugInstrumentation, severity, safeMessage);
					return;
				}
			}

			if (diagnostics.size() >= 32) {
				diagnostics.pop_front();
			}

			DebugInstrumentationDiagnostic diagnostic{};
			diagnostic.severity = severity;
			Dx12CopyDebugString(diagnostic.message, sizeof(diagnostic.message), message);
			diagnostics.push_back(diagnostic);
			Dx12MarkIssueFromMessageUnlocked(impl->debugInstrumentation, severity, safeMessage);
		}

		#if BASICRHI_ENABLE_RESHAPE
		static Dx12ReShapeRuntime* Dx12GetReShapeRuntime(Dx12Device* impl) noexcept {
			return impl ? static_cast<Dx12ReShapeRuntime*>(impl->debugInstrumentation.runtime) : nullptr;
		}

		static Result Dx12CommitReShapeMessages(Dx12Device* impl, MessageStream& stream) noexcept;
		static Result Dx12RefreshReShapeFeatures(Dx12Device* impl) noexcept;

		static DebugInstrumentationDiagnosticSeverity Dx12MapReShapeSeverity(uint32_t severity) noexcept {
			switch (static_cast<LogSeverity>(severity)) {
			case LogSeverity::Info:
				return DebugInstrumentationDiagnosticSeverity::Info;
			case LogSeverity::Warning:
				return DebugInstrumentationDiagnosticSeverity::Warning;
			case LogSeverity::Error:
			default:
				return DebugInstrumentationDiagnosticSeverity::Error;
			}
		}

		static uint64_t Dx12FindFeatureBitByNameUnlocked(const Dx12DebugInstrumentationSession& session, const char* featureName) noexcept {
			if (!featureName || !featureName[0]) {
				return 0;
			}

			for (const DebugInstrumentationFeature& feature : session.features) {
				if (std::strcmp(feature.name, featureName) == 0) {
					return feature.featureBit;
				}
			}

			return 0;
		}

		static uint64_t Dx12BuildDefaultInstrumentationFeatureMaskUnlocked(const Dx12DebugInstrumentationSession& session) noexcept {
			static constexpr const char* kDefaultFeatureNames[] = {
				"Descriptor",
				"Resource Bounds",
				"Initialization",
				"Concurrency",
			};

			uint64_t featureMask = 0;
			for (const char* featureName : kDefaultFeatureNames) {
				featureMask |= Dx12FindFeatureBitByNameUnlocked(session, featureName);
			}

			return featureMask;
		}

		static void Dx12BuildDefaultInstrumentationSpecialization(MessageStream& out) noexcept {
			auto* config = MessageStreamView<>(out).Add<SetInstrumentationConfigMessage>();
			config->validationCoverage = 0;
			config->safeGuard = 0;
			config->detail = 1;
			config->traceback = 1;
		}

		template <typename TMessage>
		static uint32_t Dx12GetChunkMask(const TMessage& message) noexcept {
			return *reinterpret_cast<const uint32_t*>(&message) >> (32u - static_cast<uint32_t>(TMessage::Chunk::Count));
		}

		static void Dx12QueueShaderSourceMappingRequestUnlocked(Dx12DebugInstrumentationSession& session, uint64_t sguid) noexcept {
			if (!sguid) {
				return;
			}

			auto& mapping = session.shaderSourceMappings[sguid];
			if (mapping.requested || session.requestedShaderSourceMappings.count(sguid)) {
				return;
			}

			session.pendingShaderSourceMappingRequests.insert(sguid);
		}

		static std::string Dx12FormatShaderSourceMappingSuffix(const Dx12DebugInstrumentationSession& session, uint64_t sguid) {
			if (!sguid) {
				return {};
			}

			auto it = session.shaderSourceMappings.find(sguid);
			if (it == session.shaderSourceMappings.end() || !it->second.resolved) {
				char buffer[64] = {};
				std::snprintf(buffer, sizeof(buffer), " [SGUID %llu]", static_cast<unsigned long long>(sguid));
				return std::string(buffer);
			}

			const Dx12ShaderSourceMappingMetadata& mapping = it->second;
			std::string suffix;
			if (mapping.fileUid != UINT32_MAX) {
				char location[96] = {};
				std::snprintf(
					location,
					sizeof(location),
					" [line %u, col %u]",
					mapping.line + 1,
					mapping.column + 1);
				suffix = location;
			}
			if (!mapping.sourceLine.empty()) {
				if (!suffix.empty()) {
					suffix += " ";
				}
				suffix += mapping.sourceLine;
			}
			return suffix;
		}

		static std::string Dx12FormatTracebackSuffix(const Dx12DebugInstrumentationSession& session, uint32_t rollingExecutionUid, uint32_t pipelineUid) {
			if (!rollingExecutionUid && !pipelineUid) {
				return {};
			}

			char header[96] = {};
			std::snprintf(
				header,
				sizeof(header),
				" [pipeline %u, execution %u]",
				pipelineUid,
				rollingExecutionUid);

			std::string suffix(header);
			auto stackIt = session.executionStacks.find(rollingExecutionUid);
			if (stackIt != session.executionStacks.end() && !stackIt->second.empty()) {
				suffix += "\nHost Stack Trace:\n";
				suffix += stackIt->second;
			}
			return suffix;
		}

		static std::string Dx12FormatDescriptorMismatchMessage(
			const Dx12DebugInstrumentationSession& session,
			const DescriptorMismatchMessage& message,
			const DescriptorMismatchMessage::TracebackChunk* traceback) {
			static constexpr const char* kTypeNames[] = { "Texture", "Buffer", "CBuffer", "Sampler" };

			std::string text;
			if (message.isUndefined) {
				text = "Descriptor is undefined";
			} else if (message.isOutOfBounds) {
				text = "Descriptor indexing out of bounds";
			} else if (message.isTableNotBound) {
				text = "Descriptor table not bound";
			} else {
				text = "Descriptor mismatch detected";
			}

			text += ", shader expected ";
			text += kTypeNames[message.compileType & 0x3u];
			if (!message.isUndefined && !message.isOutOfBounds && !message.isTableNotBound) {
				text += " but received ";
				text += kTypeNames[message.runtimeType & 0x3u];
			}

			text += Dx12FormatShaderSourceMappingSuffix(session, message.sguid);
			if (traceback) {
				text += Dx12FormatTracebackSuffix(session, traceback->rollingExecutionUID, traceback->pipelineUid);
			}

			return text;
		}

		static std::string Dx12FormatResourceBoundsMessage(
			const Dx12DebugInstrumentationSession& session,
			const ResourceIndexOutOfBoundsMessage& message,
			const ResourceIndexOutOfBoundsMessage::TracebackChunk* traceback) {
			std::string text = message.isTexture ? "Texture" : "Buffer";
			text += message.isWrite ? " write out of bounds" : " read out of bounds";
			text += Dx12FormatShaderSourceMappingSuffix(session, message.sguid);
			if (traceback) {
				text += Dx12FormatTracebackSuffix(session, traceback->rollingExecutionUID, traceback->pipelineUid);
			}
			return text;
		}

		static void Dx12AppendDescriptorMismatchExecutionDetailUnlocked(
			Dx12DebugInstrumentationSession& session,
			uint64_t shaderUid,
			const DescriptorMismatchMessage& message,
			const DescriptorMismatchMessage::DetailChunk* detailChunk,
			const DescriptorMismatchMessage::TracebackChunk* traceback,
			const char* formattedMessage) {
			Dx12InstrumentationExecutionDetail detail{};
			detail.severity = DebugInstrumentationDiagnosticSeverity::Error;
			detail.kind = Dx12InstrumentationExecutionKind::DescriptorMismatch;
			detail.shaderUid = shaderUid;
			detail.pipelineUid = traceback ? traceback->pipelineUid : 0;
			detail.sguid = message.sguid;
			detail.message = formattedMessage ? formattedMessage : "";
			detail.traceback = Dx12BuildTracebackDetail(traceback);
			detail.descriptorMismatch.hasDetail = detailChunk != nullptr;
			detail.descriptorMismatch.token = detailChunk ? detailChunk->token : 0;
			detail.descriptorMismatch.compileType = message.compileType;
			detail.descriptorMismatch.runtimeType = message.runtimeType;
			detail.descriptorMismatch.isUndefined = message.isUndefined != 0;
			detail.descriptorMismatch.isOutOfBounds = message.isOutOfBounds != 0;
			detail.descriptorMismatch.isTableNotBound = message.isTableNotBound != 0;
			Dx12AppendExecutionDetailUnlocked(session, std::move(detail));
		}

		static void Dx12AppendResourceBoundsExecutionDetailUnlocked(
			Dx12DebugInstrumentationSession& session,
			uint64_t shaderUid,
			const ResourceIndexOutOfBoundsMessage& message,
			const ResourceIndexOutOfBoundsMessage::DetailChunk* detailChunk,
			const ResourceIndexOutOfBoundsMessage::TracebackChunk* traceback,
			const char* formattedMessage) {
			Dx12InstrumentationExecutionDetail detail{};
			detail.severity = DebugInstrumentationDiagnosticSeverity::Warning;
			detail.kind = Dx12InstrumentationExecutionKind::ResourceIndexOutOfBounds;
			detail.shaderUid = shaderUid;
			detail.pipelineUid = traceback ? traceback->pipelineUid : 0;
			detail.sguid = message.sguid;
			detail.message = formattedMessage ? formattedMessage : "";
			detail.traceback = Dx12BuildTracebackDetail(traceback);
			detail.resourceBounds.hasDetail = detailChunk != nullptr;
			detail.resourceBounds.token = detailChunk ? detailChunk->token : 0;
			detail.resourceBounds.coordinate = detailChunk
				? std::array<uint32_t, 3>{ detailChunk->coordinate[0], detailChunk->coordinate[1], detailChunk->coordinate[2] }
				: std::array<uint32_t, 3>{ 0, 0, 0 };
			detail.resourceBounds.isTexture = message.isTexture != 0;
			detail.resourceBounds.isWrite = message.isWrite != 0;
			Dx12AppendExecutionDetailUnlocked(session, std::move(detail));
		}

		static void Dx12SetReShapeFeatureList(Dx12Device* impl, const FeatureListMessage& message) noexcept {
			MessageStream descriptorStream;
			message.descriptors.Transfer(descriptorStream);

			std::vector<DebugInstrumentationFeature> features;
			for (auto it = MessageStreamView(descriptorStream).GetIterator(); it; ++it) {
				if (!it.Is(FeatureDescriptorMessage::kID)) {
					continue;
				}

				auto* descriptor = it.Get<FeatureDescriptorMessage>();
				const std::string featureName{ descriptor->name.View() };
				const std::string featureDescription{ descriptor->description.View() };
				DebugInstrumentationFeature feature{};
				feature.featureBit = descriptor->featureBit;
				Dx12CopyDebugString(feature.name, sizeof(feature.name), featureName.c_str());
				Dx12CopyDebugString(feature.description, sizeof(feature.description), featureDescription.c_str());
				features.push_back(feature);
			}

			uint32_t featureCount = 0;
			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				impl->debugInstrumentation.features = std::move(features);
				impl->debugInstrumentation.featureQueryCompleted = true;
				impl->debugInstrumentation.capabilities.featureCount = static_cast<uint32_t>(impl->debugInstrumentation.features.size());
				featureCount = impl->debugInstrumentation.capabilities.featureCount;
			}

			char infoMessage[128] = {};
			sprintf_s(infoMessage,
				sizeof(infoMessage),
				"Feature discovery completed with %u backend feature(s).",
				featureCount);
			Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Info, infoMessage);
		}

		static void Dx12ProcessReShapeMessageStream(Dx12Device* impl, MessageStream& stream, MessageStream& requestStream) noexcept {
			const MessageSchema schema = stream.GetSchema();

			if (stream.Is<LogMessage>()) {
				ConstMessageStreamView<LogMessage> view(stream);
				for (auto it = view.GetIterator(); it; ++it) {
					Dx12AppendInstrumentationDiagnostic(impl, Dx12MapReShapeSeverity(it->severity), it->message.View().data());
				}
				return;
			}

			if (schema == OrderedMessageSchema::GetSchema()) {
				ConstMessageStreamView<> view(stream);
				for (auto it = view.GetIterator(); it; ++it) {
					switch (it.GetID()) {
					case FeatureListMessage::kID:
						Dx12SetReShapeFeatureList(impl, *it.Get<FeatureListMessage>());
						break;
					case InstrumentationDiagnosticMessage::kID: {
						auto* message = it.Get<InstrumentationDiagnosticMessage>();
						if (!message) {
							break;
						}

						char summary[160] = {};
						if (message->failedShaders != 0 || message->failedPipelines != 0) {
							std::snprintf(
								summary,
								sizeof(summary),
								"Instrumentation failed for %u shader(s) and %u pipeline(s).",
								message->failedShaders,
								message->failedPipelines);
							Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, summary);
						}

						std::snprintf(
							summary,
							sizeof(summary),
							"Instrumented %u shader(s) (%u ms) and %u pipeline(s) (%u ms), total %u ms.",
							message->passedShaders,
							message->millisecondsShaders,
							message->passedPipelines,
							message->millisecondsPipelines,
							message->millisecondsTotal);
						Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Info, summary);

						MessageStream diagnosticStream;
						message->messages.Transfer(diagnosticStream);
						ConstMessageStreamView<CompilationDiagnosticMessage> diagnosticsView(diagnosticStream);
						for (auto diagIt = diagnosticsView.GetIterator(); diagIt; ++diagIt) {
							Dx12AppendInstrumentationDiagnostic(
								impl,
								DebugInstrumentationDiagnosticSeverity::Error,
								diagIt->content.View().data());
						}
						break;
					}
					case PipelineNameMessage::kID: {
						auto* message = it.Get<PipelineNameMessage>();
						if (!message) {
							break;
						}

						std::lock_guard guard(impl->debugInstrumentation.mutex);
						impl->debugInstrumentation.pipelineNames[message->pipelineUID] = std::string(message->name.View());
						impl->debugInstrumentation.issuesDirty = true;
						break;
					}
					case ShaderCodeMessage::kID: {
						auto* message = it.Get<ShaderCodeMessage>();
						if (!message) {
							break;
						}

						std::lock_guard guard(impl->debugInstrumentation.mutex);
						Dx12ShaderIssueMetadata& metadata = impl->debugInstrumentation.shaderMetadata[message->shaderUID];
						metadata.requested = true;
						metadata.resolved = message->found != 0;
						metadata.nativeBinary = message->native != 0;
						metadata.hasDebugFiles = message->fileCount != 0;
						impl->debugInstrumentation.issuesDirty = true;
						break;
					}
					case ShaderCodeFileMessage::kID: {
						auto* message = it.Get<ShaderCodeFileMessage>();
						if (!message) {
							break;
						}

						const std::string filename(message->filename.View());
						if (filename.empty()) {
							break;
						}

						std::lock_guard guard(impl->debugInstrumentation.mutex);
						Dx12ShaderIssueMetadata& metadata = impl->debugInstrumentation.shaderMetadata[message->shaderUID];
						metadata.requested = true;
						metadata.resolved = true;
						metadata.hasDebugFiles = true;
						metadata.filePathsByUid[message->fileUID] = filename;
						auto& filePaths = metadata.filePaths;
						if (std::find(filePaths.begin(), filePaths.end(), filename) == filePaths.end()) {
							filePaths.push_back(filename);
							impl->debugInstrumentation.issuesDirty = true;
						}
						break;
					}
					case SetGlobalInstrumentationMessage::kID: {
						std::lock_guard guard(impl->debugInstrumentation.mutex);
						impl->debugInstrumentation.state.globalFeatureMask = it.Get<SetGlobalInstrumentationMessage>()->featureBitSet;
						break;
					}
					default:
						break;
					}
				}
				return;
			}

			if (stream.Is<ShaderSourceMappingMessage>()) {
				ConstMessageStreamView<ShaderSourceMappingMessage> view(stream);
				for (auto it = view.GetIterator(); it; ++it) {
					std::lock_guard guard(impl->debugInstrumentation.mutex);
					Dx12ShaderSourceMappingMetadata& mapping = impl->debugInstrumentation.shaderSourceMappings[it->sguid];
					mapping.requested = true;
					mapping.resolved = it->shaderGUID != 0 || !it->contents.Empty();
					mapping.shaderGuid = it->shaderGUID;
					mapping.fileUid = it->fileUID;
					mapping.line = it->line;
					mapping.column = it->column;
					mapping.sourceLine.assign(it->contents.View());
					impl->debugInstrumentation.issuesDirty = true;
				}
				return;
			}

			if (stream.Is<ExecutionStackTraceMessage>()) {
				ConstMessageStreamView<ExecutionStackTraceMessage> view(stream);
				for (auto it = view.GetIterator(); it; ++it) {
					if (!it->rollingExecutionUID) {
						continue;
					}

					std::lock_guard guard(impl->debugInstrumentation.mutex);
					impl->debugInstrumentation.executionStacks[it->rollingExecutionUID] = std::string(it->stackTrace.View());
				}
				return;
			}

			if (stream.Is<DescriptorMismatchMessage>()) {
				ConstMessageStreamView<DescriptorMismatchMessage> view(stream);
				for (auto it = view.GetIterator(); it; ++it) {
					const DescriptorMismatchMessage* message = it.Get();
					const uint32_t chunkMask = Dx12GetChunkMask(*message);
					const uint8_t* chunkData = reinterpret_cast<const uint8_t*>(message) + sizeof(DescriptorMismatchMessage);
					const DescriptorMismatchMessage::DetailChunk* detailChunk = nullptr;
					const DescriptorMismatchMessage::TracebackChunk* traceback = nullptr;

					if (chunkMask & static_cast<uint32_t>(DescriptorMismatchMessage::Chunk::Detail)) {
						detailChunk = reinterpret_cast<const DescriptorMismatchMessage::DetailChunk*>(chunkData);
						chunkData += sizeof(DescriptorMismatchMessage::DetailChunk);
					}
					if (chunkMask & static_cast<uint32_t>(DescriptorMismatchMessage::Chunk::Traceback)) {
						traceback = reinterpret_cast<const DescriptorMismatchMessage::TracebackChunk*>(chunkData);
					}

					std::string formatted;
					{
						std::lock_guard guard(impl->debugInstrumentation.mutex);
						Dx12QueueShaderSourceMappingRequestUnlocked(impl->debugInstrumentation, message->sguid);
						uint64_t shaderUid = 0;
						auto mappingIt = impl->debugInstrumentation.shaderSourceMappings.find(message->sguid);
						if (mappingIt != impl->debugInstrumentation.shaderSourceMappings.end() && mappingIt->second.resolved) {
							shaderUid = mappingIt->second.shaderGuid;
						}
						formatted = Dx12FormatDescriptorMismatchMessage(impl->debugInstrumentation, *message, traceback);
						Dx12AppendDescriptorMismatchExecutionDetailUnlocked(
							impl->debugInstrumentation,
							shaderUid,
							*message,
							detailChunk,
							traceback,
							formatted.c_str());
						Dx12QueuePendingShaderIssueUnlocked(
							impl->debugInstrumentation,
							DebugInstrumentationDiagnosticSeverity::Error,
							shaderUid,
							traceback ? traceback->pipelineUid : 0,
							message->sguid,
							formatted.c_str(),
							traceback ? traceback->rollingExecutionUID : 0);
						if (traceback && traceback->pipelineUid != 0) {
							Dx12QueuePendingPipelineIssueUnlocked(
								impl->debugInstrumentation,
								DebugInstrumentationDiagnosticSeverity::Error,
								traceback->pipelineUid,
								formatted.c_str(),
								traceback->rollingExecutionUID);
						}
					}

					Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, formatted.c_str(), false);
				}
				return;
			}

			if (stream.Is<ResourceIndexOutOfBoundsMessage>()) {
				ConstMessageStreamView<ResourceIndexOutOfBoundsMessage> view(stream);
				for (auto it = view.GetIterator(); it; ++it) {
					const ResourceIndexOutOfBoundsMessage* message = it.Get();
					const uint32_t chunkMask = Dx12GetChunkMask(*message);
					const uint8_t* chunkData = reinterpret_cast<const uint8_t*>(message) + sizeof(ResourceIndexOutOfBoundsMessage);
					const ResourceIndexOutOfBoundsMessage::DetailChunk* detailChunk = nullptr;
					const ResourceIndexOutOfBoundsMessage::TracebackChunk* traceback = nullptr;

					if (chunkMask & static_cast<uint32_t>(ResourceIndexOutOfBoundsMessage::Chunk::Detail)) {
						detailChunk = reinterpret_cast<const ResourceIndexOutOfBoundsMessage::DetailChunk*>(chunkData);
						chunkData += sizeof(ResourceIndexOutOfBoundsMessage::DetailChunk);
					}
					if (chunkMask & static_cast<uint32_t>(ResourceIndexOutOfBoundsMessage::Chunk::Traceback)) {
						traceback = reinterpret_cast<const ResourceIndexOutOfBoundsMessage::TracebackChunk*>(chunkData);
					}

					std::string formatted;
					{
						std::lock_guard guard(impl->debugInstrumentation.mutex);
						Dx12QueueShaderSourceMappingRequestUnlocked(impl->debugInstrumentation, message->sguid);
						uint64_t shaderUid = 0;
						auto mappingIt = impl->debugInstrumentation.shaderSourceMappings.find(message->sguid);
						if (mappingIt != impl->debugInstrumentation.shaderSourceMappings.end() && mappingIt->second.resolved) {
							shaderUid = mappingIt->second.shaderGuid;
						}
						formatted = Dx12FormatResourceBoundsMessage(impl->debugInstrumentation, *message, traceback);
						Dx12AppendResourceBoundsExecutionDetailUnlocked(
							impl->debugInstrumentation,
							shaderUid,
							*message,
							detailChunk,
							traceback,
							formatted.c_str());
						Dx12QueuePendingShaderIssueUnlocked(
							impl->debugInstrumentation,
							DebugInstrumentationDiagnosticSeverity::Warning,
							shaderUid,
							traceback ? traceback->pipelineUid : 0,
							message->sguid,
							formatted.c_str(),
							traceback ? traceback->rollingExecutionUID : 0);
						if (traceback && traceback->pipelineUid != 0) {
							Dx12QueuePendingPipelineIssueUnlocked(
								impl->debugInstrumentation,
								DebugInstrumentationDiagnosticSeverity::Warning,
								traceback->pipelineUid,
								formatted.c_str(),
								traceback->rollingExecutionUID);
						}
					}

					Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Warning, formatted.c_str(), false);
				}
			}
		}

		static void Dx12EnsureReShapeFeatureList(Dx12Device* impl) noexcept {
			auto* runtime = Dx12GetReShapeRuntime(impl);
			if (!impl || !runtime || !runtime->bridge) {
				return;
			}

			bool shouldRefresh = false;
			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				if (!impl->debugInstrumentation.state.active) {
					return;
				}

				shouldRefresh = !impl->debugInstrumentation.featureQueryCompleted
					|| (impl->debugInstrumentation.features.empty() && impl->debugInstrumentation.featureQueryAttempts < 3);
			}

			if (shouldRefresh) {
				(void)Dx12RefreshReShapeFeatures(impl);
			}
		}

		static void Dx12PollReShapeMessages(Dx12Device* impl) noexcept {
			auto* runtime = Dx12GetReShapeRuntime(impl);
			if (!runtime || !runtime->bridge) {
				return;
			}
			MessageStream requestStream;

			std::deque<MessageStream> capturedStreams;
			{
				std::lock_guard guard(runtime->capturedStreamMutex);
				capturedStreams.swap(runtime->capturedStreams);
			}

			for (MessageStream& stream : capturedStreams) {
				Dx12ProcessReShapeMessageStream(impl, stream, requestStream);
			}

			IMessageStorage* storage = runtime->bridge->GetOutput();
			if (storage) {
				uint32_t streamCount = 0;
				storage->ConsumeStreams(&streamCount, nullptr);
				if (streamCount != 0) {
					std::vector<MessageStream> streams(streamCount);
					storage->ConsumeStreams(&streamCount, streams.data());

					for (uint32_t streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
						MessageStream& stream = streams[streamIndex];
						Dx12ProcessReShapeMessageStream(impl, stream, requestStream);
						storage->Free(stream);
					}
				}
			}

			bool queuedDefaultFeatureMask = false;
			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				if (impl->debugInstrumentation.state.active
					&& impl->debugInstrumentation.featureQueryCompleted
					&& !impl->debugInstrumentation.defaultDescriptorMaskApplied
					&& !impl->debugInstrumentation.explicitGlobalFeatureMaskConfigured
					&& impl->debugInstrumentation.state.globalFeatureMask == 0) {
					const uint64_t defaultFeatureMask = Dx12BuildDefaultInstrumentationFeatureMaskUnlocked(impl->debugInstrumentation);
					if (defaultFeatureMask != 0) {
						MessageStream specializationStream;
						Dx12BuildDefaultInstrumentationSpecialization(specializationStream);
						auto* message = MessageStreamView<>(requestStream).Add<SetGlobalInstrumentationMessage>(SetGlobalInstrumentationMessage::AllocationInfo{
							.specializationByteSize = specializationStream.GetByteSize()
						});
						message->featureBitSet = defaultFeatureMask;
						message->specialization.Set(specializationStream);
						impl->debugInstrumentation.state.globalFeatureMask = defaultFeatureMask;
						impl->debugInstrumentation.defaultDescriptorMaskApplied = true;
						queuedDefaultFeatureMask = true;
					}
				}

				Dx12RebuildInstrumentationIssuesUnlocked(impl->debugInstrumentation);
			}

			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				for (uint64_t pipelineUid : impl->debugInstrumentation.pendingPipelineNameRequests) {
					auto* message = MessageStreamView<>(requestStream).Add<GetPipelineNameMessage>();
					message->pipelineUID = pipelineUid;
					impl->debugInstrumentation.requestedPipelineNames.insert(pipelineUid);
				}
				impl->debugInstrumentation.pendingPipelineNameRequests.clear();

				for (uint64_t shaderUid : impl->debugInstrumentation.pendingShaderCodeRequests) {
					auto* message = MessageStreamView<>(requestStream).Add<GetShaderCodeMessage>();
					message->shaderUID = shaderUid;
					message->poolCode = 0;
					message->deferred = 0;
					impl->debugInstrumentation.shaderMetadata[shaderUid].requested = true;
				}
				impl->debugInstrumentation.pendingShaderCodeRequests.clear();

				for (uint64_t sguid : impl->debugInstrumentation.pendingShaderSourceMappingRequests) {
					auto* message = MessageStreamView<>(requestStream).Add<GetShaderSourceMappingMessage>();
					message->sguid = sguid;
					impl->debugInstrumentation.requestedShaderSourceMappings.insert(sguid);
					impl->debugInstrumentation.shaderSourceMappings[sguid].requested = true;
				}
				impl->debugInstrumentation.pendingShaderSourceMappingRequests.clear();
			}

			if (requestStream.GetByteSize() != 0) {
				const Result result = Dx12CommitReShapeMessages(impl, requestStream);
				if (queuedDefaultFeatureMask) {
					if (IsOk(result)) {
						Dx12AppendInstrumentationDiagnostic(
							impl,
							DebugInstrumentationDiagnosticSeverity::Info,
							"Default GPU-Reshape instrumentation features are now enabled by default (Descriptor, Resource Bounds, Initialization, Concurrency) as shaders and pipelines are discovered.");
					} else {
						Dx12AppendInstrumentationDiagnostic(
							impl,
							DebugInstrumentationDiagnosticSeverity::Warning,
							"Default GPU-Reshape instrumentation feature enable failed; shader instrumentation may remain inactive until a feature mask is set.");
					}
				}
			}
		}

		static Result Dx12CommitReShapeMessages(Dx12Device* impl, MessageStream& stream) noexcept {
			auto* runtime = Dx12GetReShapeRuntime(impl);
			if (!runtime || !runtime->bridge) {
				RHI_FAIL(Result::Unsupported);
			}

			IMessageStorage* storage = runtime->bridge->GetOutput();
			if (!storage) {
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "Bridge output storage is unavailable during GPU-Reshape message commit.");
				RHI_FAIL(Result::Failed);
			}

			storage->AddStreamAndSwap(stream);
			runtime->bridge->Commit();
			Dx12PollReShapeMessages(impl);
			return Result::Ok;
		}

		static Result Dx12RefreshReShapeFeatures(Dx12Device* impl) noexcept {
			uint32_t attempt = 0;
			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				attempt = ++impl->debugInstrumentation.featureQueryAttempts;
			}

			char infoMessage[128] = {};
			sprintf_s(infoMessage,
				sizeof(infoMessage),
				"Requesting backend feature list (attempt %u).",
				attempt);
			Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Info, infoMessage);

			MessageStream stream;
			auto* message = MessageStreamView<>(stream).Add<GetFeaturesMessage>();
			message->featureBitSet = ~0ull;
			const Result result = Dx12CommitReShapeMessages(impl, stream);
			if (result != Result::Ok) {
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Warning, "Backend feature list request did not complete cleanly.");
			}
			return result;
		}

		static bool Dx12InitializeReShapeRuntime(Dx12Device* impl, const DeviceCreateInfo& ci) noexcept {
			Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Info, "Initializing GPU-Reshape DX12 runtime.");
			auto* runtime = new (std::nothrow) Dx12ReShapeRuntime();
			if (!runtime) {
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "Failed to allocate GPU-Reshape runtime state.");
				return false;
			}

			runtime->module = Dx12LoadReShapeLayerModule();
			if (!runtime->module) {
				delete runtime;
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "Failed to load GRS.Backends.DX12.Layer.dll. Ensure the ReShape layer is staged beside the executable.");
				return false;
			}

			runtime->createDeviceGPUOpen = reinterpret_cast<PFN_D3D12_CREATE_DEVICE_GPUOPEN>(GetProcAddress(runtime->module, "D3D12CreateDeviceGPUOpen"));
			if (!runtime->createDeviceGPUOpen) {
				FreeLibrary(runtime->module);
				delete runtime;
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "Failed to resolve D3D12CreateDeviceGPUOpen from GRS.Backends.DX12.Layer.dll.");
				return false;
			}

			::Backend::EnvironmentInfo environmentInfo{};
			environmentInfo.device.applicationName = "BasicRenderer";
			environmentInfo.device.apiName = "D3D12";
			environmentInfo.memoryBridge = true;
			environmentInfo.loadPlugins = true;

			if (!runtime->environment.Install(environmentInfo)) {
				FreeLibrary(runtime->module);
				delete runtime;
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "GPU-Reshape environment initialization failed. Check that the runtime layer and backend plugins are staged beside the executable.");
				return false;
			}

			if (auto startupContainer = runtime->environment.GetRegistry()->Get<::Backend::StartupContainer>()) {
				auto* texelAddressingMessage = MessageStreamView<>(startupContainer->stream).Add<SetTexelAddressingMessage>();
				texelAddressingMessage->enabled = true;
				Dx12AppendInstrumentationDiagnostic(
					impl,
					DebugInstrumentationDiagnosticSeverity::Info,
					"GPU-Reshape startup config enabled texel addressing for the in-process BasicRHI runtime.");
			}

			runtime->bridge = runtime->environment.GetRegistry()->Get<IBridge>();
			if (!runtime->bridge) {
				FreeLibrary(runtime->module);
				delete runtime;
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "GPU-Reshape bridge initialization failed.");
				return false;
			}

			runtime->bridgeTap = ComRef<IBridgeListener>(new (std::nothrow) Dx12ReShapeBridgeTap(runtime));
			if (!runtime->bridgeTap) {
				FreeLibrary(runtime->module);
				delete runtime;
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "Failed to allocate GPU-Reshape bridge tap for in-process message capture.");
				return false;
			}

			runtime->bridge->Register(runtime->bridgeTap);
			runtime->bridge->Register(LogMessage::kID, runtime->bridgeTap);
			runtime->bridge->Register(ShaderSourceMappingMessage::kID, runtime->bridgeTap);
			runtime->bridge->Register(ExecutionStackTraceMessage::kID, runtime->bridgeTap);
			runtime->bridge->Register(DescriptorMismatchMessage::kID, runtime->bridgeTap);
			runtime->bridge->Register(ResourceIndexOutOfBoundsMessage::kID, runtime->bridgeTap);

			D3D12_DEVICE_GPUOPEN_GPU_RESHAPE_INFO gpuOpenInfo{};
			gpuOpenInfo.registry = runtime->environment.GetRegistry();

			Microsoft::WRL::ComPtr<ID3D12Device10> reshapeDevice;
			const HRESULT hr = runtime->createDeviceGPUOpen(
				impl->adapter.Get(),
				D3D_FEATURE_LEVEL_12_0,
				IID_PPV_ARGS(&reshapeDevice),
				&gpuOpenInfo);
			if (FAILED(hr)) {
				runtime->bridge->Deregister(ResourceIndexOutOfBoundsMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(DescriptorMismatchMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(ExecutionStackTraceMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(ShaderSourceMappingMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(LogMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(runtime->bridgeTap);
				runtime->bridgeTap.Release();
				FreeLibrary(runtime->module);
				delete runtime;
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Error, "GPU-Reshape failed to wrap the D3D12 device. Falling back to an uninstrumented device.");
				return false;
			}

			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				impl->debugInstrumentation.runtime = runtime;
				impl->debugInstrumentation.state.active = true;
				impl->debugInstrumentation.featureQueryAttempts = 0;
				impl->debugInstrumentation.featureQueryCompleted = false;
				impl->debugInstrumentation.capabilities.installSupported = false;
				impl->debugInstrumentation.capabilities.globalInstrumentationSupported = true;
				impl->debugInstrumentation.capabilities.shaderInstrumentationSupported = false;
				impl->debugInstrumentation.capabilities.pipelineInstrumentationSupported = false;
				impl->debugInstrumentation.capabilities.synchronousRecordingSupported = true;
			}

			impl->pNativeDevice = reshapeDevice;
			Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Info, "GPU-Reshape wrapped the D3D12 device successfully.");

			MessageStream configStream;
			auto* configMessage = MessageStreamView<>(configStream).Add<SetApplicationInstrumentationConfigMessage>();
			configMessage->synchronousRecording = ci.instrumentation.enableSynchronousRecording;
			if (Dx12CommitReShapeMessages(impl, configStream) != Result::Ok) {
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Warning, "GPU-Reshape synchronous recording setup did not complete cleanly.");
			}

			if (Dx12RefreshReShapeFeatures(impl) != Result::Ok) {
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Warning, "GPU-Reshape feature discovery failed.");
			}

			if (impl->debugInstrumentation.features.empty()) {
				Dx12AppendInstrumentationDiagnostic(impl, DebugInstrumentationDiagnosticSeverity::Warning, "GPU-Reshape initialized, but no backend instrumentation features were discovered.");
			}

			return true;
		}

		static void Dx12ShutdownReShapeRuntime(Dx12Device* impl) noexcept {
			if (!impl) {
				return;
			}

			auto* runtime = Dx12GetReShapeRuntime(impl);
			if (!runtime) {
				return;
			}

			Dx12PollReShapeMessages(impl);
			if (runtime->bridge && runtime->bridgeTap) {
				runtime->bridge->Deregister(ResourceIndexOutOfBoundsMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(DescriptorMismatchMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(ExecutionStackTraceMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(ShaderSourceMappingMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(LogMessage::kID, runtime->bridgeTap);
				runtime->bridge->Deregister(runtime->bridgeTap);
				runtime->bridgeTap.Release();
			}
			if (runtime->module) {
				FreeLibrary(runtime->module);
				runtime->module = nullptr;
			}
			delete runtime;
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			impl->debugInstrumentation.runtime = nullptr;
			impl->debugInstrumentation.state.active = false;
			impl->debugInstrumentation.featureQueryCompleted = false;
		}
		#else
		static void Dx12PollReShapeMessages(Dx12Device*) noexcept {
		}

		static Result Dx12RefreshReShapeFeatures(Dx12Device*) noexcept {
			return Result::Unsupported;
		}

		static void Dx12EnsureReShapeFeatureList(Dx12Device*) noexcept {
		}

		static bool Dx12InitializeReShapeRuntime(Dx12Device*, const DeviceCreateInfo&) noexcept {
			return false;
		}

		static void Dx12ShutdownReShapeRuntime(Dx12Device*) noexcept {
		}
		#endif

		static void Dx12InitializeDebugInstrumentation(Dx12Device* impl, const DeviceCreateInfo& ci) noexcept {
			if (!impl) {
				return;
			}

			auto& instrumentation = impl->debugInstrumentation;
			instrumentation.state.requested = ci.instrumentation.enableRuntimeInstrumentation;
			instrumentation.state.active = false;
			instrumentation.state.synchronousRecording = ci.instrumentation.enableSynchronousRecording;
			instrumentation.state.globalFeatureMask = 0;
			instrumentation.capabilities.backendBuildEnabled = BASICRHI_ENABLE_RESHAPE != 0;
			instrumentation.capabilities.installSupported = false;
			instrumentation.capabilities.globalInstrumentationSupported = ci.instrumentation.enableRuntimeInstrumentation && (BASICRHI_ENABLE_RESHAPE != 0);
			instrumentation.capabilities.shaderInstrumentationSupported = false;
			instrumentation.capabilities.pipelineInstrumentationSupported = false;
			instrumentation.capabilities.synchronousRecordingSupported = ci.instrumentation.enableRuntimeInstrumentation && (BASICRHI_ENABLE_RESHAPE != 0);
			instrumentation.capabilities.featureCount = 0;

			if (!instrumentation.state.requested) {
				return;
			}

		#if !BASICRHI_ENABLE_RESHAPE
			Dx12AppendInstrumentationDiagnostic(
				impl,
				DebugInstrumentationDiagnosticSeverity::Warning,
				"Runtime instrumentation was requested, but BasicRHI was built without BASICRHI_ENABLE_RESHAPE. Instrumentation remains inactive.");
		#endif
		}

		static void d_checkDebugMessages(Device* d) noexcept {
			if (!d) {
				return;
			}
			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl || !impl->pNativeDevice) {
				return;
			}
			Dx12PollReShapeMessages(impl);
			LogDredData();
		}

		static Result d_getDebugInstrumentationCapabilities(const Device* d, DebugInstrumentationCapabilities& out) noexcept {
			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			Dx12EnsureReShapeFeatureList(impl);
			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			out = impl->debugInstrumentation.capabilities;
			out.featureCount = static_cast<uint32_t>(impl->debugInstrumentation.features.size());
			return Result::Ok;
		}

		static Result d_getDebugInstrumentationState(const Device* d, DebugInstrumentationState& out) noexcept {
			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			Dx12EnsureReShapeFeatureList(impl);
			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			out = impl->debugInstrumentation.state;
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationFeatureCount(const Device* d) noexcept {
			if (!d) {
				return 0;
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				return 0;
			}

			Dx12EnsureReShapeFeatureList(impl);
			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			return static_cast<uint32_t>(impl->debugInstrumentation.features.size());
		}

		static Result d_copyDebugInstrumentationFeatures(const Device* d,
			uint32_t first,
			DebugInstrumentationFeature* outFeatures,
			uint32_t capacity,
			uint32_t* copied) noexcept {
			if (copied) {
				*copied = 0;
			}

			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			Dx12EnsureReShapeFeatureList(impl);
			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			const auto& features = impl->debugInstrumentation.features;
			if (first >= features.size() || capacity == 0 || !outFeatures) {
				return Result::Ok;
			}

			const uint32_t available = static_cast<uint32_t>(features.size() - first);
			const uint32_t toCopy = (std::min)(capacity, available);
			for (uint32_t index = 0; index < toCopy; ++index) {
				outFeatures[index] = features[first + index];
			}

			if (copied) {
				*copied = toCopy;
			}
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationDiagnosticCount(const Device* d) noexcept {
			if (!d) {
				return 0;
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				return 0;
			}

			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			return static_cast<uint32_t>(impl->debugInstrumentation.diagnostics.size());
		}

		static Result d_copyDebugInstrumentationDiagnostics(const Device* d,
			uint32_t first,
			DebugInstrumentationDiagnostic* outDiagnostics,
			uint32_t capacity,
			uint32_t* copied) noexcept {
			if (copied) {
				*copied = 0;
			}

			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			const auto& diagnostics = impl->debugInstrumentation.diagnostics;
			if (first >= diagnostics.size() || capacity == 0 || !outDiagnostics) {
				return Result::Ok;
			}

			const uint32_t available = static_cast<uint32_t>(diagnostics.size() - first);
			const uint32_t toCopy = (std::min)(capacity, available);
			for (uint32_t index = 0; index < toCopy; ++index) {
				outDiagnostics[index] = diagnostics[first + index];
			}

			if (copied) {
				*copied = toCopy;
			}
			return Result::Ok;
		}

		static uint32_t d_getDebugInstrumentationIssueCount(const Device* d) noexcept {
			if (!d) {
				return 0;
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				return 0;
			}

			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			Dx12RebuildInstrumentationIssuesUnlocked(impl->debugInstrumentation);
			return static_cast<uint32_t>(impl->debugInstrumentation.issues.size());
		}

		static Result d_copyDebugInstrumentationIssues(const Device* d,
			uint32_t first,
			DebugInstrumentationIssue* outIssues,
			uint32_t capacity,
			uint32_t* copied) noexcept {
			if (copied) {
				*copied = 0;
			}

			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			Dx12PollReShapeMessages(impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			Dx12RebuildInstrumentationIssuesUnlocked(impl->debugInstrumentation);
			const auto& issues = impl->debugInstrumentation.issues;
			if (first >= issues.size() || capacity == 0 || !outIssues) {
				return Result::Ok;
			}

			const uint32_t available = static_cast<uint32_t>(issues.size() - first);
			const uint32_t toCopy = (std::min)(capacity, available);
			for (uint32_t index = 0; index < toCopy; ++index) {
				outIssues[index] = issues[first + index];
			}

			if (copied) {
				*copied = toCopy;
			}
			return Result::Ok;
		}

		static Result d_setDebugGlobalInstrumentationMask(Device* d, uint64_t featureMask) noexcept {
			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (!impl->debugInstrumentation.state.active) {
				RHI_FAIL(Result::Unsupported);
			}

			{
				std::lock_guard guard(impl->debugInstrumentation.mutex);
				impl->debugInstrumentation.explicitGlobalFeatureMaskConfigured = true;
				impl->debugInstrumentation.defaultDescriptorMaskApplied = true;
			}

			#if BASICRHI_ENABLE_RESHAPE
			MessageStream stream;
			MessageStream specializationStream;
			Dx12BuildDefaultInstrumentationSpecialization(specializationStream);
			auto* message = MessageStreamView<>(stream).Add<SetGlobalInstrumentationMessage>(SetGlobalInstrumentationMessage::AllocationInfo{
				.specializationByteSize = specializationStream.GetByteSize()
			});
			message->featureBitSet = featureMask;
			message->specialization.Set(specializationStream);

			if (Dx12CommitReShapeMessages(impl, stream) != Result::Ok) {
				RHI_FAIL(Result::Failed);
			}
			#else
			(void)featureMask;
			RHI_FAIL(Result::Unsupported);
			#endif

			std::lock_guard guard(impl->debugInstrumentation.mutex);
			impl->debugInstrumentation.state.globalFeatureMask = featureMask;
			return Result::Ok;
		}

		static Result d_setDebugSynchronousRecording(Device* d, bool enabled) noexcept {
			if (!d) {
				RHI_FAIL(Result::InvalidArgument);
			}

			auto* impl = static_cast<Dx12Device*>(d->impl);
			if (!impl) {
				RHI_FAIL(Result::InvalidArgument);
			}

			if (!impl->debugInstrumentation.state.active) {
				RHI_FAIL(Result::Unsupported);
			}

			#if BASICRHI_ENABLE_RESHAPE
			MessageStream stream;
			auto* message = MessageStreamView<>(stream).Add<SetApplicationInstrumentationConfigMessage>();
			message->synchronousRecording = enabled;

			if (Dx12CommitReShapeMessages(impl, stream) != Result::Ok) {
				RHI_FAIL(Result::Failed);
			}
			#else
			(void)enabled;
			RHI_FAIL(Result::Unsupported);
			#endif

			std::lock_guard guard(impl->debugInstrumentation.mutex);
			impl->debugInstrumentation.state.synchronousRecording = enabled;
			return Result::Ok;
		}

		static Result q_signal(Queue* q, const TimelinePoint& p) noexcept {
			auto* qs = dx12_detail::QState(q);
			auto* dev = qs->dev;
			if (!dev) {
				RHI_FAIL(Result::InvalidArgument);
			}
			auto* TL = dev->timelines.get(p.t); if (!TL) {
				BreakIfDebugging();
				return Result::InvalidArgument;
			}
			// Catch zero-value signals early - these are almost always bugs.
			// Break here to get a callstack showing who triggered this.
			if (p.value == 0) {
				spdlog::error(
					"q_signal: attempted to signal timeline(index={}, gen={}) with "
					"value 0. This will violate monotonic ordering if the timeline "
					"has ever been signaled before. Break here to inspect callstack.",
					p.t.index, p.t.generation);
				BreakIfDebugging();
				return Result::InvalidArgument;
			}
#if BUILD_TYPE == BUILD_DEBUG
			auto last = qs->lastSignaledValue.find(p.t);
			if (last != qs->lastSignaledValue.end() && p.value <= last->second) {
				spdlog::error(
					"q_signal monotonicity violation: timeline(index={}, gen={}) "
					"attempted value={} but lastSignaled={}",
					p.t.index, p.t.generation,
					p.value, last->second);
				BreakIfDebugging();
				return Result::InvalidArgument; // must be strictly greater
			}
			qs->lastSignaledValue[p.t] = p.value;
#endif
			return SUCCEEDED(qs->pNativeQueue->Signal(TL->fence.Get(), p.value)) ? Result::Ok : Result::Failed;
		}

		static Result q_wait(Queue* q, const TimelinePoint& p) noexcept {
			auto* qs = dx12_detail::QState(q);
			auto* dev = qs->dev; if (!dev) {
				RHI_FAIL(Result::InvalidArgument);
			}
			auto* TL = dev->timelines.get(p.t); if (!TL) {
				RHI_FAIL(Result::InvalidArgument);
			}
			return SUCCEEDED(qs->pNativeQueue->Wait(TL->fence.Get(), p.value)) ? Result::Ok : Result::Failed;
		}

		static void q_setName(Queue* q, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* qs = dx12_detail::QState(q);
			if (!qs || !qs->pNativeQueue) {
				BreakIfDebugging();
				return;
			}
			std::wstring w(n, n + ::strlen(n));
			qs->pNativeQueue->SetName(w.c_str());
		}

		// ---------------- CommandList vtable funcs ----------------

		static inline UINT MipDim(UINT base, UINT mip) {
			return (std::max)(1u, base >> mip);
		}

		static inline UINT CalcSubresourceFor(const Dx12Resource& T, UINT mip, UINT arraySlice) {
			// PlaneSlice = 0 (non-planar). TODO: support planar formats
			return D3D12CalcSubresource(mip, arraySlice, 0, T.tex.mips, T.tex.arraySize);
		}

		static void cl_end(CommandList* cl) noexcept {
			auto* w = dx12_detail::CL(cl);
			w->cl->Close();
		}
		static void cl_reset(CommandList* cl, const CommandAllocator& ca) noexcept {
			auto* l = dx12_detail::CL(cl);
			auto* a = dx12_detail::Alloc(&ca);
#if BUILD_TYPE == BUILD_DEBUG
			if (!l) {
				BreakIfDebugging();
				spdlog::error("cl_reset: invalid command list");
			}
			if (!a) {
				BreakIfDebugging();
				spdlog::error("cl_reset: invalid command allocator");
			}
			l->boundPipeline = nullptr;
#endif
			l->boundLayout = {};
			l->boundLayoutPtr = nullptr;
			l->rootCbvShadowStates.clear();
			for (auto& page : l->rootCbvScratchPages) {
				page.cursor = 0;
			}
			l->cl->Reset(a->alloc.Get(), nullptr);
		}

		static bool DxSafeClearRenderTargetView(
			ID3D12GraphicsCommandList* commandList,
			D3D12_CPU_DESCRIPTOR_HANDLE cpu,
			const float* rgba) noexcept {
			__try {
				commandList->ClearRenderTargetView(cpu, rgba, 0, nullptr);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		static bool DxSafeOMSetRenderTargets(
			ID3D12GraphicsCommandList* commandList,
			UINT numRtvs,
			const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
			const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) noexcept {
			__try {
				commandList->OMSetRenderTargets(numRtvs, rtvs, FALSE, dsv);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		static void cl_beginPass(CommandList* cl, const PassBeginInfo& p) noexcept {
			auto* l = dx12_detail::CL(cl);
			if (!l) {
				BreakIfDebugging();
				return;
			}

			std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
			rtvs.reserve(p.colors.size);
			for (uint32_t i = 0; i < p.colors.size; ++i) {
				D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
				if (DxGetDstCpu(l->dev, p.colors.data[i].rtv, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) {
					rtvs.push_back(cpu);
					if (p.colors.data[i].loadOp == LoadOp::Clear) {
						if (!DxSafeClearRenderTargetView(l->cl.Get(), cpu, p.colors.data[i].clear.rgba)) {
							spdlog::critical(
								"DX12 BeginPass: ClearRenderTargetView crashed for color[{}] cpu=0x{:X}",
								i,
								static_cast<unsigned long long>(cpu.ptr));
							LogDredData();
							BreakIfDebugging();
							std::abort();
						}
					}
					else if (p.colors.data[i].loadOp == LoadOp::DontCare) {
						auto* R = l->dev->resources.get(p.colors.data[i].resource);
						if (R && R->res) {
							D3D12_DISCARD_REGION discardRegion{};
							discardRegion.NumRects = 1;
							discardRegion.pRects = nullptr;
							if (p.colors.data[i].mipSlice == -1) {
								BreakIfDebugging();
							}
							discardRegion.FirstSubresource = p.colors.data[i].mipSlice;
							discardRegion.NumSubresources = 1;
							l->cl->DiscardResource(R->res.Get(), &discardRegion);
						}
					}
				}
				else {
					spdlog::error(
						"DX12 BeginPass: failed to resolve RTV descriptor for color[{}] heap=({}, {}) index={}",
						i,
						p.colors.data[i].rtv.heap.index,
						p.colors.data[i].rtv.heap.generation,
						p.colors.data[i].rtv.index);
				}
			}

			D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
			const D3D12_CPU_DESCRIPTOR_HANDLE* pDsv = nullptr;
			if (p.depth) {
				if (DxGetDstCpu(l->dev, p.depth->dsv, dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) {
					pDsv = &dsv;
					if (p.depth->depthLoad == LoadOp::Clear || p.depth->stencilLoad == LoadOp::Clear) {
						const auto& c = p.depth->clear;
						l->cl->ClearDepthStencilView(
							dsv,
							(p.depth->depthLoad == LoadOp::Clear ? D3D12_CLEAR_FLAG_DEPTH : (D3D12_CLEAR_FLAGS)0) |
							(p.depth->stencilLoad == LoadOp::Clear ? D3D12_CLEAR_FLAG_STENCIL : (D3D12_CLEAR_FLAGS)0),
							c.depthStencil.depth, c.depthStencil.stencil, 0, nullptr);
					}
					else if (p.depth->depthLoad == LoadOp::DontCare || p.depth->stencilLoad == LoadOp::DontCare) {
						auto* R = l->dev->resources.get(p.depth->resource);
						if (R && R->res) {
							l->cl->DiscardResource(R->res.Get(), nullptr);
						}
					}
				}
			}

			if (!DxSafeOMSetRenderTargets(l->cl.Get(), (UINT)rtvs.size(), rtvs.data(), pDsv)) {
				spdlog::critical("DX12 BeginPass: OMSetRenderTargets crashed");
				LogDredData();
				BreakIfDebugging();
				std::abort();
			}
			D3D12_VIEWPORT vp{ 0,0,(float)p.width,(float)p.height,0.0f,1.0f };
			D3D12_RECT sc{ 0,0,(LONG)p.width,(LONG)p.height };
			l->cl->RSSetViewports(1, &vp);
			l->cl->RSSetScissorRects(1, &sc);
		}

		static void cl_endPass(CommandList* /*cl*/) noexcept {} // nothing to do in DX12
		static void cl_bindLayout(CommandList* cl, PipelineLayoutHandle layoutH) noexcept {
			auto* impl = dx12_detail::CL(cl);
			auto* dev = impl->dev;
			auto* L = dev->pipelineLayouts.get(layoutH);
			if (!L || !L->root) {
				BreakIfDebugging();
				return;
			}

			switch (impl->type) {
			case D3D12_COMMAND_LIST_TYPE_DIRECT:
				impl->cl->SetGraphicsRootSignature(L->root.Get());
				impl->cl->SetComputeRootSignature(L->root.Get());
				break;
			case D3D12_COMMAND_LIST_TYPE_COMPUTE:
				impl->cl->SetComputeRootSignature(L->root.Get());
				break;
			case D3D12_COMMAND_LIST_TYPE_COPY:
				// no root signature for copy-only lists
				BreakIfDebugging();
				break;
			}

			if (impl->boundLayout.index != layoutH.index || impl->boundLayout.generation != layoutH.generation) {
				impl->rootCbvShadowStates.clear();
			}
			impl->boundLayout = layoutH;
			impl->boundLayoutPtr = L;
		}
		static void cl_bindPipeline(CommandList* cl, PipelineHandle psoH) noexcept {
			auto* l = dx12_detail::CL(cl);
			auto* dev = l->dev;
			if (auto* P = dev->pipelines.get(psoH)) {
#if BUILD_MODE == BUILD_DEBUG
				l->boundPipeline = P;
#endif
				l->cl->SetPipelineState(P->pso.Get());
				return;
			}
#if BUILD_TYPE == BUILD_DEBUG
			l->boundPipeline = nullptr;
			BreakIfDebugging();
			spdlog::error("cl_bindPipeline: invalid pipeline handle");
#endif
		}
		static void cl_setVB(CommandList* cl, uint32_t startSlot, uint32_t numViews, VertexBufferView* pBufferViews) noexcept {
			auto* l = dx12_detail::CL(cl);
			std::vector<D3D12_VERTEX_BUFFER_VIEW> views; views.resize(numViews);
			auto* dev = l->dev;
			for (uint32_t i = 0; i < numViews; ++i) {
				if (auto* B = dev->resources.get(pBufferViews[i].buffer)) {
					views[i].BufferLocation = B->res->GetGPUVirtualAddress() + pBufferViews[i].offset;
					views[i].SizeInBytes = pBufferViews[i].sizeBytes;
					views[i].StrideInBytes = pBufferViews[i].stride;
				}
			}
			l->cl->IASetVertexBuffers(startSlot, (UINT)numViews, views.data());
		}
		static void cl_setIB(CommandList* cl, const IndexBufferView& view) noexcept {
			auto* l = dx12_detail::CL(cl);
			auto* dev = l->dev;
			if (auto* B = dev->resources.get(view.buffer)) {
				D3D12_INDEX_BUFFER_VIEW ibv{};
				ibv.BufferLocation = B->res->GetGPUVirtualAddress() + view.offset;
				ibv.SizeInBytes = view.sizeBytes;
				ibv.Format = ToDxgi(view.format);
				l->cl->IASetIndexBuffer(&ibv);
			}
		}
		static void cl_draw(CommandList* cl, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) noexcept {
			auto* l = dx12_detail::CL(cl);
			l->cl->DrawInstanced(vc, ic, fv, fi);
		}
		static void cl_drawIndexed(CommandList* cl, uint32_t ic, uint32_t inst, uint32_t firstIdx, int32_t vtxOff, uint32_t firstInst) noexcept {
			auto* l = dx12_detail::CL(cl);
			l->cl->DrawIndexedInstanced(ic, inst, firstIdx, vtxOff, firstInst);
		}
		static void cl_dispatch(CommandList* cl, uint32_t x, uint32_t y, uint32_t z) noexcept {
			auto* l = dx12_detail::CL(cl);
			l->cl->Dispatch(x, y, z);
		}

		static void cl_clearRTV_slot(CommandList* c, DescriptorSlot s, const rhi::ClearValue& cv) noexcept {
			auto* impl = dx12_detail::CL(c);
			if (!impl) {
				BreakIfDebugging();
				return;
			}
			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			if (!DxGetDstCpu(impl->dev, s, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_RTV)) {
				BreakIfDebugging();
				return;
			}

			float rgba[4] = { cv.rgba[0], cv.rgba[1], cv.rgba[2], cv.rgba[3] };
			impl->cl->ClearRenderTargetView(cpu, rgba, 0, nullptr);
		}

		static void cl_clearDSV_slot(CommandList* c, DescriptorSlot s,
			bool clearDepth, bool clearStencil,
			float depth, uint8_t stencil) noexcept
		{
			auto* impl = dx12_detail::CL(c);
			if (!impl) {
				BreakIfDebugging();
				return;
			}
			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			if (!DxGetDstCpu(impl->dev, s, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) {
				BreakIfDebugging();
				return;
			}

			D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
			if (clearDepth)   flags |= D3D12_CLEAR_FLAG_DEPTH;
			if (clearStencil) flags |= D3D12_CLEAR_FLAG_STENCIL;

			impl->cl->ClearDepthStencilView(cpu, flags, depth, stencil, 0, nullptr);
		}
		static void cl_executeIndirect(
			CommandList* cl,
			CommandSignatureHandle sigH,
			ResourceHandle argBufH, uint64_t argOff,
			ResourceHandle cntBufH, uint64_t cntOff,
			uint32_t maxCount) noexcept
		{
			if (!cl || !cl->impl) {
				BreakIfDebugging();
				return;
			}

			auto* l = dx12_detail::CL(cl);
			auto* dev = l->dev;
			if (!dev) {
				BreakIfDebugging();
				return;
			}

			auto* S = dev->commandSignatures.get(sigH);
			if (!S || !S->sig) {
				BreakIfDebugging();
				return;
			}

			auto* argB = dev->resources.get(argBufH);
			if (!argB || !argB->res) {
				BreakIfDebugging();
				return;
			}

			ID3D12Resource* cntRes = nullptr;
			if (cntBufH.valid()) {
				auto* c = dev->resources.get(cntBufH);
				if (c && c->res) cntRes = c->res.Get();
			}

#if BUILD_MODE == BUILD_DEBUG
			if (l->boundPipeline == nullptr) {
				BreakIfDebugging();
			}
#endif

			l->cl->ExecuteIndirect(
				S->sig.Get(),
				maxCount,
				argB->res.Get(), argOff,
				cntRes, cntOff);
		}
		static void cl_setDescriptorHeaps(CommandList* cl, DescriptorHeapHandle csu, std::optional<DescriptorHeapHandle> samp) noexcept {
			auto* l = dx12_detail::CL(cl);
			auto* dev = l->dev;

			ID3D12DescriptorHeap* heaps[2]{};
			UINT n = 0;
			if (auto* H = dev->descHeaps.get(csu)) {
				heaps[n++] = H->heap.Get();
			}
			if (samp.has_value()) {
				if (auto* H = dev->descHeaps.get(samp.value())) {
					heaps[n++] = H->heap.Get();
				}
			}
			if (n) {
				l->cl->SetDescriptorHeaps(n, heaps);
			}
		}

		static void cl_barrier(CommandList* cl, const BarrierBatch& b) noexcept {
			if (!cl || !cl->impl) {
				BreakIfDebugging();
				return;
			}
			auto* l = dx12_detail::CL(cl);
			auto* dev = l->dev;
			if (!dev) {
				BreakIfDebugging();
				return;
			}

			std::vector<D3D12_TEXTURE_BARRIER> tex;
			std::vector<D3D12_BUFFER_BARRIER>  buf;
			std::vector<D3D12_GLOBAL_BARRIER>  glob;

			tex.reserve(b.textures.size);
			buf.reserve(b.buffers.size);
			glob.reserve(b.globals.size);

			// Textures
			for (uint32_t i = 0; i < b.textures.size; ++i) {
				const auto& t = b.textures.data[i];
				auto* T = dev->resources.get(t.texture);
				if (!T || !T->res) continue;

				D3D12_TEXTURE_BARRIER tb{};
				tb.SyncBefore = ToDX(t.beforeSync);
				tb.SyncAfter = ToDX(t.afterSync);
				tb.AccessBefore = ToDX(t.beforeAccess);
				tb.AccessAfter = ToDX(t.afterAccess);
				tb.LayoutBefore = ToDX(t.beforeLayout);
				tb.LayoutAfter = ToDX(t.afterLayout);
				tb.pResource = T->res.Get();
				tb.Subresources = ToDX(t.range);
				tex.push_back(tb);
			}

			// Buffers
			for (uint32_t i = 0; i < b.buffers.size; ++i) {
				const auto& br = b.buffers.data[i];
				auto* B = dev->resources.get(br.buffer);
				if (!B || !B->res) continue;

				D3D12_BUFFER_BARRIER bb{};
				bb.SyncBefore = ToDX(br.beforeSync);
				bb.SyncAfter = ToDX(br.afterSync);
				bb.AccessBefore = ToDX(br.beforeAccess);
				bb.AccessAfter = ToDX(br.afterAccess);
				bb.pResource = B->res.Get();
				bb.Offset = br.offset;
				bb.Size = br.size;
				buf.push_back(bb);
			}

			// Globals
			for (uint32_t i = 0; i < b.globals.size; ++i) {
				const auto& g = b.globals.data[i];
				D3D12_GLOBAL_BARRIER gb{};
				gb.SyncBefore = ToDX(g.beforeSync);
				gb.SyncAfter = ToDX(g.afterSync);
				gb.AccessBefore = ToDX(g.beforeAccess);
				gb.AccessAfter = ToDX(g.afterAccess);
				glob.push_back(gb);
			}

			// Build groups (one per kind if non-empty)
			std::vector<D3D12_BARRIER_GROUP> groups;
			groups.reserve(3);
			if (!buf.empty()) {
				D3D12_BARRIER_GROUP g{};
				g.Type = D3D12_BARRIER_TYPE_BUFFER;
				g.NumBarriers = (UINT)buf.size();
				g.pBufferBarriers = buf.data();
				groups.push_back(g);
			}
			if (!tex.empty()) {
				D3D12_BARRIER_GROUP g{};
				g.Type = D3D12_BARRIER_TYPE_TEXTURE;
				g.NumBarriers = (UINT)tex.size();
				g.pTextureBarriers = tex.data();
				groups.push_back(g);
			}
			if (!glob.empty()) {
				D3D12_BARRIER_GROUP g{};
				g.Type = D3D12_BARRIER_TYPE_GLOBAL;
				g.NumBarriers = (UINT)glob.size();
				g.pGlobalBarriers = glob.data();
				groups.push_back(g);
			}

			if (!groups.empty()) {
				l->cl->Barrier((UINT)groups.size(), groups.data());
			}
		}

		static void cl_clearUavUint(CommandList* cl,
			const UavClearInfo& u,
			const UavClearUint& v) noexcept
		{
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!rec || !impl) {
				BreakIfDebugging();
				return;
			}

			// Resolve the two matching descriptors
			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
			if (!DxGetDstCpu(impl, u.cpuVisible, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
				BreakIfDebugging();
				return;
			}
			if (!DxGetDstGpu(impl, u.shaderVisible, gpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
				BreakIfDebugging();
				return;
			}

			// Resource to clear
			ID3D12Resource* res = nullptr;
			if (u.resource.IsTexture()) {
				res = impl->resources.get(u.resource.GetHandle())->res.Get();
			}
			else {
				res = impl->resources.get(u.resource.GetHandle())->res.Get();
			}
			if (!res) {
				BreakIfDebugging();
				return;
			}

			// NOTE: caller must have bound the shader-visible heap via SetDescriptorHeaps()
			// and transitioned 'res' to UAV/UNORDERED_ACCESS
			rec->cl->ClearUnorderedAccessViewUint(gpu, cpu, res, v.v, 0, nullptr);
		}

		static void cl_clearUavFloat(CommandList* cl,
			const UavClearInfo& u,
			const UavClearFloat& v) noexcept
		{
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!rec || !impl) {
				BreakIfDebugging();
				return;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
			if (!DxGetDstCpu(impl, u.cpuVisible, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
				BreakIfDebugging();
				return;
			}
			if (!DxGetDstGpu(impl, u.shaderVisible, gpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
				BreakIfDebugging();
				return;
			}

			ID3D12Resource* res = nullptr;
			if (u.resource.IsTexture()) {
				res = impl->resources.get(u.resource.GetHandle())->res.Get();
			}
			else {
				res = impl->resources.get(u.resource.GetHandle())->res.Get();
			}
			if (!res) {
				BreakIfDebugging();
				return;
			}

			rec->cl->ClearUnorderedAccessViewFloat(gpu, cpu, res, v.v, 0, nullptr);
		}

		static UINT Align256(UINT x) { return (x + 255u) & ~255u; }

		// texture -> buffer
		static void cl_copyTextureToBuffer(rhi::CommandList* cl, const rhi::BufferTextureCopyFootprint& r) noexcept {
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!rec || !impl) {
				BreakIfDebugging();
				return;
			}

			auto* T = impl->resources.get(r.texture);
			auto* B = impl->resources.get(r.buffer);
			if (!T || !B || !T->res || !B->res) {
				BreakIfDebugging();
				return;
			}

			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = B->res.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint.Offset = r.footprint.offset;
			dst.PlacedFootprint.Footprint.Format = T->fmt;     // texture s actual DXGI format
			dst.PlacedFootprint.Footprint.Width = r.footprint.width;
			dst.PlacedFootprint.Footprint.Height = r.footprint.height;
			dst.PlacedFootprint.Footprint.Depth = r.footprint.depth;
			dst.PlacedFootprint.Footprint.RowPitch = r.footprint.rowPitch;

			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = T->res.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = CalcSubresourceFor(*T, r.mip,
				(T->dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 0u : r.arraySlice);

			// Full subresource copy (nullptr box) at (x,y,z) inside the texture
			rec->cl->CopyTextureRegion(&dst, r.x, r.y, r.z, &src, nullptr);
		}

		// buffer -> texture (symmetric)
		static void cl_copyBufferToTexture(rhi::CommandList* cl, const rhi::BufferTextureCopyFootprint& r) noexcept {
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!rec || !impl) {
				BreakIfDebugging();
				return;
			}

			auto* T = impl->resources.get(r.texture);
			auto* B = impl->resources.get(r.buffer);
			if (!T || !B || !T->res || !B->res) {
				BreakIfDebugging();
				return;
			}

			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = B->res.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Offset = r.footprint.offset;
			src.PlacedFootprint.Footprint.Format = T->fmt;
			src.PlacedFootprint.Footprint.Width = r.footprint.width;
			src.PlacedFootprint.Footprint.Height = r.footprint.height;
			src.PlacedFootprint.Footprint.Depth = r.footprint.depth;
			src.PlacedFootprint.Footprint.RowPitch = r.footprint.rowPitch;

			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = T->res.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = CalcSubresourceFor(*T, r.mip,
				(T->dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 0u : r.arraySlice);

			rec->cl->CopyTextureRegion(&dst, r.x, r.y, r.z, &src, nullptr);
		}

		static void cl_copyTextureRegion(
			CommandList* cl,
			const TextureCopyRegion& dst,
			const TextureCopyRegion& src) noexcept
		{
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!rec || !impl) {
				BreakIfDebugging();
				return;
			}

			auto* DstT = impl->resources.get(dst.texture);
			auto* SrcT = impl->resources.get(src.texture);
			if (!DstT || !SrcT || !DstT->res || !SrcT->res) {
				BreakIfDebugging();
				return;
			}

			// Build D3D12 copy locations
			D3D12_TEXTURE_COPY_LOCATION dxDst{};
			dxDst.pResource = DstT->res.Get();
			dxDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dxDst.SubresourceIndex = CalcSubresourceFor(*DstT, dst.mip,
				(DstT->dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 0u : dst.arraySlice);

			D3D12_TEXTURE_COPY_LOCATION dxSrc{};
			dxSrc.pResource = SrcT->res.Get();
			dxSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dxSrc.SubresourceIndex = CalcSubresourceFor(*SrcT, src.mip,
				(SrcT->dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? 0u : src.arraySlice);

			// If width/height/depth are zero, treat them as "copy full mip slice from src starting at (src.x,src.y,src.z)"
			UINT srcW = src.width ? src.width : MipDim(SrcT->tex.w, src.mip);
			UINT srcH = src.height ? src.height : MipDim(SrcT->tex.h, src.mip);
			UINT srcD = src.depth ? src.depth : ((SrcT->dim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
				? MipDim(SrcT->tex.depth, src.mip) : 1u);

			// Clamp box to the src subresource bounds just in case
			if (SrcT->dim != D3D12_RESOURCE_DIMENSION_TEXTURE3D) { srcD = 1; }

			D3D12_BOX srcBox{};
			srcBox.left = src.x;
			srcBox.top = src.y;
			srcBox.front = src.z;
			srcBox.right = src.x + srcW;
			srcBox.bottom = src.y + srcH;
			srcBox.back = src.z + srcD;

			// Perform the copy
			// NOTE: Resources must already be in COPY_SOURCE / COPY_DEST layouts respectively.
			rec->cl->CopyTextureRegion(
				&dxDst,
				dst.x, dst.y, dst.z,   // destination offsets
				&dxSrc,
				&srcBox);
		}

		static void cl_copyBufferRegion(CommandList* cl,
			ResourceHandle dst, uint64_t dstOffset,
			ResourceHandle src, uint64_t srcOffset,
			uint64_t numBytes) noexcept
		{
			if (!cl || !cl->IsValid() || numBytes == 0) {
				BreakIfDebugging();
				return;
			}

			auto* rec = dx12_detail::CL(cl);
			auto* dev = rec->dev;
			if (!rec || !dev) {
				BreakIfDebugging();
				return;
			}

			// Look up buffer resources
			auto* D = dev->resources.get(dst);
			auto* S = dev->resources.get(src);
			if (!D || !S || !D->res || !S->res) {
				BreakIfDebugging();
				return;
			}

			// We don't validate bounds here (we don't store sizes). DX12 will validate.
			// Required states (caller's responsibility via barriers):
			//   src:  COPY_SOURCE   (ResourceAccessType::CopySource / Layout::CopySource)
			//   dst:  COPY_DEST     (ResourceAccessType::CopyDest   / Layout::CopyDest)
			rec->cl->CopyBufferRegion(
				D->res.Get(), dstOffset,
				S->res.Get(), srcOffset,
				numBytes);
		}

		static void cl_setName(CommandList* cl, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* l = dx12_detail::CL(cl);
			if (!l || !l->cl) {
				BreakIfDebugging();
				return;
			}
			l->cl->SetName(std::wstring(n, n + ::strlen(n)).c_str());
		}

		static void cl_writeTimestamp(CommandList* cl, QueryPoolHandle pool, uint32_t index, Stage /*ignored*/) noexcept {
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!impl) {
				BreakIfDebugging();
				return;
			}

			auto* P = impl->queryPools.get(pool);
			if (!P || P->type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP) {
				BreakIfDebugging();
				return;
			}

			rec->cl->EndQuery(P->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
		}

		static void cl_beginQuery(CommandList* cl, QueryPoolHandle pool, uint32_t index) noexcept {
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!impl) {
				BreakIfDebugging();
				return;
			}

			auto* P = impl->queryPools.get(pool);
			if (!P) {
				BreakIfDebugging();
				return;
			}

			if (P->type == D3D12_QUERY_HEAP_TYPE_OCCLUSION) {
				rec->cl->BeginQuery(P->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
			}
			else if (P->type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS) {
				rec->cl->BeginQuery(P->heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index);
			}
			else if (P->type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1) {
				rec->cl->BeginQuery(P->heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, index);
			}
		}

		static void cl_endQuery(CommandList* cl, QueryPoolHandle pool, uint32_t index) noexcept {
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!impl) {
				BreakIfDebugging();
				return;
			}

			auto* P = impl->queryPools.get(pool);
			if (!P) {
				BreakIfDebugging();
				return;
			}

			if (P->type == D3D12_QUERY_HEAP_TYPE_OCCLUSION) {
				rec->cl->EndQuery(P->heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, index);
			}
			else if (P->type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS) {
				rec->cl->EndQuery(P->heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index);
			}
			else if (P->type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1) {
				rec->cl->EndQuery(P->heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, index);
			}
			else if (P->type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP) {
				// no-op; timestamps use writeTimestamp (EndQuery(TIMESTAMP))
			}
		}

		static void cl_resolveQueryData(CommandList* cl,
			QueryPoolHandle pool,
			uint32_t firstQuery, uint32_t queryCount,
			ResourceHandle dst, uint64_t dstOffset) noexcept
		{
			auto* rec = dx12_detail::CL(cl);
			auto* impl = rec ? rec->dev : nullptr;
			if (!impl) {
				BreakIfDebugging();
				return;
			}

			auto* P = impl->queryPools.get(pool);
			if (!P) {
				BreakIfDebugging();
				return;
			}

			// Resolve to the given buffer (assumed COPY_DEST)
			auto* B = impl->resources.get(dst);
			if (!B || !B->res) {
				BreakIfDebugging();
				return;
			}

			D3D12_QUERY_TYPE type{};
			switch (P->type) {
			case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:               type = D3D12_QUERY_TYPE_TIMESTAMP; break;
			case D3D12_QUERY_HEAP_TYPE_OCCLUSION:               type = D3D12_QUERY_TYPE_OCCLUSION; break;
			case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:     type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS; break;
			case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1:    type = D3D12_QUERY_TYPE_PIPELINE_STATISTICS1; break;
			default: return;
			}

			rec->cl->ResolveQueryData(P->heap.Get(), type, firstQuery, queryCount, B->res.Get(), dstOffset);
		}

		static void cl_resetQueries(CommandList* /*cl*/, QueryPoolHandle /*pool*/, uint32_t /*first*/, uint32_t /*count*/) noexcept {
			// D3D12 does not require resets; Vulkan impl will fill this.
		}

		static void cl_pushConstants(CommandList* c, ShaderStage stages,
			uint32_t set, uint32_t binding,
			uint32_t dstOffset32, uint32_t num32,
			const void* data) noexcept
		{
			auto* impl = dx12_detail::CL(c);
			if (!impl || !impl->boundLayoutPtr || !data || num32 == 0) {
				BreakIfDebugging();
				return;
			}

			// Find the matching push-constant root param
			const Dx12PipelineLayout* L = impl->boundLayoutPtr;
			const Dx12PipelineLayout::RootConstParam* rc = nullptr;
			for (const auto& r : L->rcParams) { // TODO: Better lookup than linear scan
				if (r.set == set && r.binding == binding) { rc = &r; break; }
			}
			if (!rc) {
				BreakIfDebugging();
				return; // not declared in layout
			}
			// Clamp for safety
			const uint32_t maxAvail = rc->num32;
			if (dstOffset32 >= maxAvail) return;
			if (dstOffset32 + num32 > maxAvail) num32 = maxAvail - dstOffset32;

			// Write to requested stages. On DX12, graphics/compute have distinct root constant slots.
			const auto* p32 = static_cast<const uint32_t*>(data);
			if (rc->type == PushConstantRangeType::RootConstants32) {
				if (static_cast<uint32_t>(stages) & static_cast<uint32_t>(ShaderStage::Compute)) {
					impl->cl->SetComputeRoot32BitConstants(rc->rootIndex, num32, p32, dstOffset32);
				}
				if (static_cast<uint32_t>(stages) & static_cast<uint32_t>(ShaderStage::AllGraphics)) {
					impl->cl->SetGraphicsRoot32BitConstants(rc->rootIndex, num32, p32, dstOffset32);
				}
				return;
			}

			auto* shadowState = Dx12GetRootCbvShadowState(impl, *rc);
			if (!shadowState || shadowState->values.size() != maxAvail) {
				BreakIfDebugging();
				return;
			}
			std::memcpy(shadowState->values.data() + dstOffset32, p32, static_cast<size_t>(num32) * sizeof(uint32_t));

			D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
			void* cpuAddress = nullptr;
			const size_t uploadBytes = static_cast<size_t>(maxAvail) * sizeof(uint32_t);
			if (!Dx12AllocateRootCbvScratch(impl, uploadBytes, gpuAddress, cpuAddress) || !cpuAddress) {
				BreakIfDebugging();
				spdlog::error(
					"DX12 root CBV scratch allocation failed for set={}, binding={}, bytes={}",
					rc->set,
					rc->binding,
					uploadBytes);
				return;
			}
			std::memcpy(cpuAddress, shadowState->values.data(), uploadBytes);

			if (static_cast<uint32_t>(stages) & static_cast<uint32_t>(ShaderStage::Compute)) {
				impl->cl->SetComputeRootConstantBufferView(rc->rootIndex, gpuAddress);
			}
			if (static_cast<uint32_t>(stages) & static_cast<uint32_t>(ShaderStage::AllGraphics)) {
				impl->cl->SetGraphicsRootConstantBufferView(rc->rootIndex, gpuAddress);
			}
		}

		static void cl_setPrimitiveTopology(CommandList* cl, PrimitiveTopology pt) noexcept {
			auto* l = dx12_detail::CL(cl);
			if (!l) {
				BreakIfDebugging();
				return;
			}
			l->cl->IASetPrimitiveTopology(ToDX(pt));
		}

		static void cl_dispatchMesh(CommandList* cl, uint32_t x, uint32_t y, uint32_t z) noexcept {
			auto* l = dx12_detail::CL(cl);
			if (!l) {
				BreakIfDebugging();
				return;
			}
			l->cl->DispatchMesh(x, y, z);
		}

		static void cl_setWorkGraph(CommandList* cl, const WorkGraphHandle& h, const ResourceHandle& backingMemory, bool resetBackingMemory) noexcept {
			auto* l = dx12_detail::CL(cl);
			if (!l) {
				BreakIfDebugging();
				return;
			}
			auto wg = l->dev->workGraphs.get(h);
			auto backingMem = l->dev->resources.get(backingMemory);
			if (!wg || !wg->stateObject || !backingMem || !backingMem->res) {
				BreakIfDebugging();
				return;
			}

			D3D12_SET_PROGRAM_DESC desc{};
			desc.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
			desc.WorkGraph.Flags = resetBackingMemory ? static_cast<D3D12_SET_WORK_GRAPH_FLAGS>(D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE) : static_cast<D3D12_SET_WORK_GRAPH_FLAGS>(D3D12_WORK_GRAPH_FLAG_NONE);
			desc.WorkGraph.NodeLocalRootArgumentsTable = D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE(0); // TODO?
			desc.WorkGraph.ProgramIdentifier = wg->programIdentifier;
			D3D12_GPU_VIRTUAL_ADDRESS_RANGE bgMemRange{};
			bgMemRange.StartAddress = backingMem->res->GetGPUVirtualAddress();
			bgMemRange.SizeInBytes = wg->memoryRequirements.MaxSizeInBytes;
			desc.WorkGraph.BackingMemory = bgMemRange;
			l->cl->SetProgram(&desc);
		}

		static void cl_dispatchWorkGraph(CommandList* cl, const WorkGraphDispatchDesc& desc) noexcept {
			auto* l = dx12_detail::CL(cl);
			if (!l) {
				BreakIfDebugging();
				return;
			}

			D3D12_DISPATCH_GRAPH_DESC d{};
			d.Mode = ToDX(desc.dispatchMode);
			switch (desc.dispatchMode) {
			case WorkGraphDispatchMode::NodeCpuInput: {
				d.NodeCPUInput = ToDX(desc.nodeCpuInput);
				l->cl->DispatchGraph(&d);
				return;
			} break;
			case WorkGraphDispatchMode::NodeGpuInput: {
				d.NodeGPUInput = l->dev->resources.get(desc.nodeGpuInput.inputBuffer)->res->GetGPUVirtualAddress() + desc.nodeGpuInput.inputAddressOffset;
				l->cl->DispatchGraph(&d);
				return;
			} break;
			case WorkGraphDispatchMode::MultiNodeCpuInput: {
				D3D12_MULTI_NODE_CPU_INPUT mni{};
				std::vector<D3D12_NODE_CPU_INPUT> mxNodes;
				mni = ToDX(desc.multiNodeCpuInput, mxNodes);
				d.MultiNodeCPUInput = mni;
				l->cl->DispatchGraph(&d);
				return;
			} break;
			case WorkGraphDispatchMode::MultiNodeGpuInput: {
				d.MultiNodeGPUInput = l->dev->resources.get(desc.multiNodeGpuInput.inputBuffer)->res->GetGPUVirtualAddress() + desc.multiNodeGpuInput.inputAddressOffset;
				l->cl->DispatchGraph(&d);
			} break;
			default:
				BreakIfDebugging();
				break;
			}
		}

		// ---------------- Swapchain vtable funcs ----------------
		static uint32_t sc_count(Swapchain* sc) noexcept { return dx12_detail::SC(sc)->count; }
		static uint32_t sc_curr(Swapchain* sc) noexcept { return dx12_detail::SC(sc)->pSlProxySC->GetCurrentBackBufferIndex(); }
		//static ViewHandle sc_rtv(Swapchain* sc, uint32_t i) noexcept { return dx12_detail::SC(sc)->rtvHandles[i]; }
		static ResourceHandle sc_img(Swapchain* sc, uint32_t i) noexcept { return dx12_detail::SC(sc)->imageHandles[i]; }
		static Result sc_present(Swapchain* sc, bool vsync) noexcept {
			auto* s = dx12_detail::SC(sc);
			UINT sync = vsync ? 1 : 0; UINT flags = 0;
			return s->pSlProxySC->Present(sync, flags) == S_OK ? Result::Ok : Result::Failed;
		}

		static Result sc_resizeBuffers(
			Swapchain* sc,
			uint32_t numBuffers,
			uint32_t w, uint32_t h,
			Format newFormat,
			uint32_t flags) noexcept
		{
			auto* s = dx12_detail::SC(sc);
			if (!s || !s->pSlProxySC || !s->dev) {
				spdlog::critical(
					"DX12 resizeBuffers: invalid state sc={} proxySc={} dev={}",
					static_cast<void*>(s),
					s ? static_cast<void*>(s->pSlProxySC.Get()) : nullptr,
					s ? static_cast<void*>(s->dev) : nullptr);
				return Result::InvalidNativePointer;
			}

			auto* dev = s->dev;

			const uint32_t oldCount = s->count;
			const uint32_t newCount = (numBuffers != 0) ? numBuffers : oldCount;
			const DXGI_FORMAT newFmt = (newFormat != Format::Unknown) ? ToDxgi(newFormat) : s->fmt;
			const UINT resizeFlags = s->flags | static_cast<UINT>(flags);

			// Ensure nothing is using the old backbuffers.
			if (dev->self.vt && dev->self.vt->deviceWaitIdle)
				(void)dev->self.vt->deviceWaitIdle(&dev->self);

			// Release all refs to old backbuffers.
			for (uint32_t i = 0; i < oldCount; ++i)
			{
				if (i < s->images.size())
					s->images[i].Reset();

				if (i < s->imageHandles.size())
				{
					const auto hnd = s->imageHandles[i];
					if (hnd.generation != 0)
						if (auto* r = dev->resources.get(hnd))
							r->res.Reset();
				}
			}

			// Resize swapchain buffers.
			HRESULT hr = s->pSlProxySC->ResizeBuffers((UINT)newCount, (UINT)w, (UINT)h, newFmt, resizeFlags);
			if (FAILED(hr)) {
				spdlog::critical(
					"DX12 resizeBuffers: proxy ResizeBuffers failed hr=0x{:08X} proxySc={} nativeSc={} oldCount={} newCount={} size={}x{} fmt={} flags=0x{:08X} storedFlags=0x{:08X}",
					static_cast<unsigned>(hr),
					static_cast<void*>(s->pSlProxySC.Get()),
					static_cast<void*>(s->pNativeSC.Get()),
					oldCount,
					newCount,
					w,
					h,
					static_cast<unsigned>(newFmt),
					resizeFlags,
					s->flags);
				return ToRHI(hr);
			}

			ComPtr<IDXGISwapChain3> nativeSc3;
			if (s->dev->steamlineInitialized) {
				IDXGISwapChain3* nativeSc3Raw = nullptr;
				slGetNativeInterface(s->pSlProxySC.Get(), reinterpret_cast<void**>(&nativeSc3Raw));
				nativeSc3.Attach(nativeSc3Raw);
			}
			else {
				nativeSc3 = s->pSlProxySC;
			}
			if (!nativeSc3) {
				spdlog::critical(
					"DX12 resizeBuffers: slGetNativeInterface returned null native swapchain proxySc={}",
					static_cast<void*>(s->pSlProxySC.Get()));
				return Result::InvalidNativePointer;
			}
			s->pNativeSC = nativeSc3;

			// If w/h were 0, query actual post-resize dims.
			if (w == 0 || h == 0)
			{
				DXGI_SWAP_CHAIN_DESC1 d{};
				if (SUCCEEDED(s->pSlProxySC->GetDesc1(&d))) { s->w = d.Width; s->h = d.Height; }
			}
			else { s->w = (UINT)w; s->h = (UINT)h; }

			s->fmt = newFmt;
			s->count = (UINT)newCount;
			s->flags = resizeFlags;

			// Adjust handle arrays (free excess if shrinking, alloc new if growing).
			if (newCount < s->imageHandles.size())
			{
				for (uint32_t i = newCount; i < (uint32_t)s->imageHandles.size(); ++i)
				{
					const auto hnd = s->imageHandles[i];
					if (hnd.generation != 0)
					{
						if (auto* r = dev->resources.get(hnd))
							r->res.Reset();
						dev->resources.free(hnd);
					}
				}
				s->imageHandles.resize(newCount);
			}
			else
			{
				s->imageHandles.resize(newCount); // default init new entries
			}

			s->images.resize(newCount);

			// Reacquire and re-register/update resources.
			for (uint32_t i = 0; i < newCount; ++i)
			{
				ComPtr<ID3D12Resource> img;
				hr = s->pNativeSC->GetBuffer((UINT)i, IID_PPV_ARGS(&img));
				if (FAILED(hr)) {
					spdlog::critical(
						"DX12 resizeBuffers: GetBuffer failed index={} hr=0x{:08X} nativeSc={}",
						i,
						static_cast<unsigned>(hr),
						static_cast<void*>(s->pNativeSC.Get()));
					return ToRHI(hr);
				}

				s->images[i] = img;

				Dx12Resource t(img, newFmt, s->w, s->h, 1, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1, s->dev);

				// Preserve existing handle if possible, in case they're being stored externally
				// otherwise allocate a new one.
				if (s->imageHandles[i].generation != 0)
				{
					if (auto* r = dev->resources.get(s->imageHandles[i]))
					{
						r->res = img;
						r->kind = Dx12ResourceKind::Texture;
						r->fmt = newFmt;
						r->tex.w = s->w;
						r->tex.h = s->h;
						r->tex.mips = 1;
						r->tex.arraySize = 1;
						r->tex.depth = 1;
						r->dim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
						r->dev = s->dev;
					}
					else
						s->imageHandles[i] = dev->resources.alloc(t);
				}
				else
				{
					s->imageHandles[i] = dev->resources.alloc(t);
				}
			}

			s->current = s->pSlProxySC->GetCurrentBackBufferIndex();
			return Result::Ok;
		}
		static void sc_setName(Swapchain* sc, const char* n) noexcept {} // Cannot name IDXGISwapChain
		// ---------------- Resource vtable funcs ----------------

		static void buf_map(Resource* r, void** data, uint64_t offset, uint64_t size) noexcept {
			if (!r || !data) {
				BreakIfDebugging();
				return;
			}
			auto* B = dx12_detail::Res(r);
			if (!B || !B->res) {
				*data = nullptr;
				BreakIfDebugging();
				return;
			}

			D3D12_RANGE readRange{};
			D3D12_RANGE* pRange = nullptr;
			if (size != ~0ull) {
				readRange.Begin = SIZE_T(offset);
				readRange.End = SIZE_T(offset + size);
				pRange = &readRange;
			}

			void* ptr = nullptr;
			HRESULT hr = B->res->Map(0, pRange, &ptr);
			if (SUCCEEDED(hr)) {
				*data = static_cast<uint8_t*>(ptr) + size_t(offset);
			}
			else {
				*data = nullptr;
			}
		}

		static void buf_unmap(Resource* r, uint64_t writeOffset, uint64_t writeSize) noexcept {
			auto* B = dx12_detail::Res(r);
			if (!B || !B->res) {
				BreakIfDebugging();
				return;
			}
			D3D12_RANGE range{};
			if (writeSize != ~0ull) {
				range.Begin = SIZE_T(writeOffset);
				range.End = SIZE_T(writeOffset + writeSize);
			}
			B->res->Unmap(0, (writeSize != ~0ull) ? &range : nullptr);
		}

		static void buf_setName(Resource* r, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* B = dx12_detail::Res(r);
			if (!B || !B->res) {
				BreakIfDebugging();
				return;
			}
			B->res->SetName(s2ws(n).c_str());
		}

		static void tex_map(Resource* r, void** data, uint64_t /*offset*/, uint64_t /*size*/) noexcept {
			if (!r || !data) {
				BreakIfDebugging();
				return;
			}
			auto* T = dx12_detail::Res(r);
			if (!T || !T->res) {
				*data = nullptr;
				BreakIfDebugging();
				return;
			}

			// NOTE: Texture mapping is only valid on UPLOAD/READBACK heaps.
			// This returns a pointer to subresource 0 memory. Caller must compute
			// row/slice offsets via GetCopyableFootprints.
			void* ptr = nullptr;
			HRESULT hr = T->res->Map(0, nullptr, &ptr);
			*data = SUCCEEDED(hr) ? ptr : nullptr;
		}
		static void tex_unmap(Resource* r, uint64_t writeOffset, uint64_t writeSize) noexcept {
			auto* T = dx12_detail::Res(r);
			if (T && T->res) T->res->Unmap(0, nullptr);
		}

		static void tex_setName(Resource* r, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* T = dx12_detail::Res(r);
			if (!T || !T->res) {
				BreakIfDebugging();
				return;
			}
			T->res->SetName(s2ws(n).c_str());
		}

		// ------------------ Allocator vtable funcs ----------------
		static void ca_reset(CommandAllocator* ca) noexcept {
			if (!ca || !ca->impl) {
				BreakIfDebugging();
				return;
			}
			auto* A = dx12_detail::Alloc(ca);
			A->alloc->Reset(); // ID3D12CommandAllocator::Reset()
		}

		// ------------------ QueryPool vtable funcs ----------------
		static QueryResultInfo qp_getQueryResultInfo(QueryPool* p) noexcept {
			auto* P = dx12_detail::QP(p);
			QueryResultInfo out{};
			if (!P) {
				BreakIfDebugging();
				return out;
			}
			out.count = P->count;

			switch (P->type) {
			case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
				out.type = QueryType::Timestamp;
				out.elementSize = sizeof(uint64_t);
				break;
			case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
				out.type = QueryType::Occlusion;
				out.elementSize = sizeof(uint64_t);
				break;
			case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
				out.type = QueryType::PipelineStatistics;
				out.elementSize = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
				break;
			case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1:
				out.type = QueryType::PipelineStatistics;
				out.elementSize = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
				break;
			}
			return out;
		}

		static PipelineStatsLayout qp_getPipelineStatsLayout(QueryPool* p,
			PipelineStatsFieldDesc* outBuf,
			uint32_t cap) noexcept
		{
			auto* P = dx12_detail::QP(p);
			PipelineStatsLayout L{};
			if (!P) {
				BreakIfDebugging();
				return L;
			}

			L.info = qp_getQueryResultInfo(p);

			// Build a local vector, then copy to outBuf
			std::vector<PipelineStatsFieldDesc> tmp;
			tmp.reserve(16);

			if (!P->usePSO1) {
				using S = D3D12_QUERY_DATA_PIPELINE_STATISTICS;
				auto push = [&](PipelineStatTypes f, size_t off) {
					tmp.push_back({ f, uint32_t(off), uint32_t(sizeof(uint64_t)), true });
					};

				for (unsigned int i = 0; i < cap; i++) {
					auto type = outBuf[i].field;
					switch (type) {
					case PipelineStatTypes::IAVertices:            push(type, offsetof(S, IAVertices)); break;
					case PipelineStatTypes::IAPrimitives:          push(type, offsetof(S, IAPrimitives)); break;
					case PipelineStatTypes::VSInvocations:         push(type, offsetof(S, VSInvocations)); break;
					case PipelineStatTypes::GSInvocations:         push(type, offsetof(S, GSInvocations)); break;
					case PipelineStatTypes::GSPrimitives:          push(type, offsetof(S, GSPrimitives)); break;
					case PipelineStatTypes::TSControlInvocations:  push(type, offsetof(S, HSInvocations)); break;
					case PipelineStatTypes::TSEvaluationInvocations: push(type, offsetof(S, DSInvocations)); break;
					case PipelineStatTypes::PSInvocations:         push(type, offsetof(S, PSInvocations)); break;
					case PipelineStatTypes::CSInvocations:         push(type, offsetof(S, CSInvocations)); break;
					case PipelineStatTypes::TaskInvocations:       tmp.push_back({ PipelineStatTypes::TaskInvocations, 0, 0, false }); break;
					case PipelineStatTypes::MeshInvocations:       tmp.push_back({ PipelineStatTypes::MeshInvocations, 0, 0, false }); break;
					case PipelineStatTypes::MeshPrimitives:        tmp.push_back({ PipelineStatTypes::MeshPrimitives,  0, 0, false }); break;
					default:
						tmp.push_back({ type, 0, 0, false });
						break;
					}
				}
			}
			else {
				using S = D3D12_QUERY_DATA_PIPELINE_STATISTICS1;
				auto push = [&](PipelineStatTypes f, size_t off) {
					tmp.push_back({ f, uint32_t(off), uint32_t(sizeof(uint64_t)), true });
					};

				for (unsigned int i = 0; i < cap; i++) {
					auto type = outBuf[i].field;
					switch (type) {
					case PipelineStatTypes::IAVertices:            push(type, offsetof(S, IAVertices)); break;
					case PipelineStatTypes::IAPrimitives:          push(type, offsetof(S, IAPrimitives)); break;
					case PipelineStatTypes::VSInvocations:         push(type, offsetof(S, VSInvocations)); break;
					case PipelineStatTypes::GSInvocations:         push(type, offsetof(S, GSInvocations)); break;
					case PipelineStatTypes::GSPrimitives:          push(type, offsetof(S, GSPrimitives)); break;
					case PipelineStatTypes::TSControlInvocations:  push(type, offsetof(S, HSInvocations)); break;
					case PipelineStatTypes::TSEvaluationInvocations: push(type, offsetof(S, DSInvocations)); break;
					case PipelineStatTypes::PSInvocations:         push(type, offsetof(S, PSInvocations)); break;
					case PipelineStatTypes::CSInvocations:         push(type, offsetof(S, CSInvocations)); break;
					case PipelineStatTypes::TaskInvocations:       push(type, offsetof(S, ASInvocations)); break;
					case PipelineStatTypes::MeshInvocations:       push(type, offsetof(S, MSInvocations)); break;
					case PipelineStatTypes::MeshPrimitives:        push(type, offsetof(S, MSPrimitives)); break;
					default:
						tmp.push_back({ type, 0, 0, false });
						break;
					}
				}
			}

			// Copy out
			const uint32_t n = std::min<uint32_t>(cap, (uint32_t)tmp.size());
			if (outBuf && n) std::memcpy(outBuf, tmp.data(), n * sizeof(tmp[0]));
			// Return layout header: info + fields span (caller knows cap, we return size via .fields.size)
			L.fields = { outBuf, n };
			return L;
		}

		static void qp_setName(QueryPool* qp, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* Q = dx12_detail::QP(qp);
			if (!Q || !Q->heap) {
				BreakIfDebugging();
				return;
			}
			Q->heap->SetName(s2ws(n).c_str());
		}

		// ------------------ Pipeline vtable funcs ----------------
		static void pso_setName(Pipeline* p, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* P = dx12_detail::Pso(p);
			if (!P || !P->pso) {
				BreakIfDebugging();
				return;
			}
			P->pso->SetName(s2ws(n).c_str());
		}

		// ------------------ WorkGraph vtable funcs ----------------
		static uint64_t wg_getRequiredScratchMemorySize(WorkGraph* wg) noexcept {
			auto* W = dx12_detail::WG(wg);
			if (!W) {
				BreakIfDebugging();
				return 0;
			}
			return W->memoryRequirements.MaxSizeInBytes;
		}
		static void wg_setName(WorkGraph* wg, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			if (!wg || !wg->IsValid()) {
				BreakIfDebugging();
				return;
			}
			auto* W = dx12_detail::WG(wg);
			if (!W || !W->stateObject) {
				BreakIfDebugging();
				return;
			}
			W->stateObject->SetName(s2ws(n).c_str());
		}

		// ------------------ PipelineLayout vtable funcs ----------------
		static void pl_setName(PipelineLayout* pl, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* L = dx12_detail::PL(pl);
			if (!L || !L->root) {
				BreakIfDebugging();
				return;
			}
			L->root->SetName(s2ws(n).c_str());
		}

		// ------------------ CommandSignature vtable funcs ----------------
		static void cs_setName(CommandSignature* cs, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* S = dx12_detail::CSig(cs);
			if (!S || !S->sig) {
				BreakIfDebugging();
				return;
			}
			S->sig->SetName(s2ws(n).c_str());
		}

		// ------------------ DescriptorHeap vtable funcs ----------------
		static void dh_setName(DescriptorHeap* dh, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* H = dx12_detail::DH(dh);
			if (!H || !H->heap) {
				BreakIfDebugging();
				return;
			}
			H->heap->SetName(s2ws(n).c_str());
		}

		// ------------------ Sampler vtable funcs ----------------
		static void s_setName(Sampler* s, const char* n) noexcept {
			return; // cannot name ID3D12SamplerState
		}

		// ------------------ Timeline vtable funcs ----------------
		static uint64_t tl_timelineCompletedValue(Timeline* t) noexcept {
			auto* impl = dx12_detail::TL(t);
			return impl->fence ? impl->fence->GetCompletedValue() : 0;
		}

		static Result tl_timelineHostWait(Timeline* tl, const uint64_t p, uint32_t timeout_ms) noexcept {
			auto* TL = dx12_detail::TL(tl);
			HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!e) {
				BreakIfDebugging();
				return Result::Failed;
			}
			HRESULT hr = TL->fence->SetEventOnCompletion(p, e);
			if (FAILED(hr)) {
				CloseHandle(e);
				BreakIfDebugging();
				return Result::Failed;
			}
			const DWORD waitMs = (timeout_ms == UINT32_MAX) ? INFINITE : static_cast<DWORD>(timeout_ms);
			DWORD waitResult = WaitForSingleObject(e, waitMs);
			CloseHandle(e);
			if (waitResult == WAIT_TIMEOUT) {
				return Result::WaitTimeout;
			}
			return Result::Ok;
		}
		static void tl_setName(Timeline* tl, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* T = dx12_detail::TL(tl);
			if (!T || !T->fence) {
				BreakIfDebugging();
				return;
			}
			T->fence->SetName(s2ws(n).c_str());
		}

		// ------------------ Heap vtable funcs ----------------
		static void h_setName(Heap* h, const char* n) noexcept {
			if (!n) {
				BreakIfDebugging();
				return;
			}
			auto* H = dx12_detail::Hp(h);
			if (!H || !H->heap) {
				BreakIfDebugging();
				return;
			}
			H->heap->SetName(s2ws(n).c_str());
		}

		// ---------------- Helpers ----------------

		void EnableShaderBasedValidation() {
			CComPtr<ID3D12Debug> spDebugController0;
			CComPtr<ID3D12Debug1> spDebugController1;
			VERIFY(D3D12GetDebugInterface(IID_PPV_ARGS(&spDebugController0)));
			VERIFY(spDebugController0->QueryInterface(IID_PPV_ARGS(&spDebugController1)));
			spDebugController1->SetEnableGPUBasedValidation(true);
		}

		static void EnableDebug(ID3D12Device* device) {
			ComPtr<ID3D12Debug> debugController;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
				debugController->EnableDebugLayer();
			}
			//EnableShaderBasedValidation();
			ComPtr<ID3D12InfoQueue> iq; if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&iq)))) {
				D3D12_MESSAGE_ID blocked[] = { (D3D12_MESSAGE_ID)1356, (D3D12_MESSAGE_ID)1328, (D3D12_MESSAGE_ID)1008 };
				D3D12_INFO_QUEUE_FILTER f{}; f.DenyList.NumIDs = (UINT)_countof(blocked); f.DenyList.pIDList = blocked; iq->AddStorageFilterEntries(&f);
				iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
				iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
			}
		}

	#if BASICRHI_ENABLE_STREAMLINE
		void SlLogMessageCallback(sl::LogType level, const char* message) {
			//spdlog::info("Streamline Log: {}", message);
		}
		std::wstring GetExePath() {
			WCHAR buffer[MAX_PATH] = { 0 };
			GetModuleFileNameW(NULL, buffer, MAX_PATH);
			std::wstring::size_type pos = std::wstring(buffer).find_last_of(L"\\/");
			return std::wstring(buffer).substr(0, pos);
		}

		bool InitSL() noexcept
		{
			// IMPORTANT: Always securely load SL library, see source/core/sl.security/secureLoadLibrary for more details
	// Always secure load SL modules
			if (!sl::security::verifyEmbeddedSignature(L"sl.interposer.dll"))
			{
				// SL module not signed, disable SL
			}
			else
			{
				auto mod = LoadLibraryW(L"sl.interposer.dll");

				if (!mod) {
					spdlog::error("Failed to load sl.interposer.dll, ensure it is in the correct directory.");
					return false;
				}

			}

			sl::Preferences pref{};
			pref.showConsole = false; // for debugging, set to false in production
			pref.logLevel = sl::LogLevel::eDefault;
			auto path = GetExePath() + L"\\NVSL";
			const wchar_t* path_wchar = path.c_str();
			pref.pathsToPlugins = { &path_wchar }; // change this if Streamline plugins are not located next to the executable
			pref.numPathsToPlugins = 1; // change this if Streamline plugins are not located next to the executable
			pref.pathToLogsAndData = {}; // change this to enable logging to a file
			pref.logMessageCallback = SlLogMessageCallback; // highly recommended to track warning/error messages in your callback
			pref.engine = sl::EngineType::eCustom; // If using UE or Unity
			pref.engineVersion = "0.0.1"; // Optional version
			pref.projectId = "72a89ee2-1139-4cc5-8daa-d27189bed781"; // Optional project id
			sl::Feature myFeatures[] = { sl::kFeatureDLSS };
			pref.featuresToLoad = myFeatures;
			pref.numFeaturesToLoad = _countof(myFeatures);
			pref.renderAPI = sl::RenderAPI::eD3D12;
			pref.flags |= sl::PreferenceFlags::eUseManualHooking;
			if (SL_FAILED(res, slInit(pref)))
			{
				// Handle error, check the logs
				if (res == sl::Result::eErrorDriverOutOfDate) { /* inform user */ }
				// and so on ...
				return false;
			}
			return true;
		}
	#else
		bool InitSL() noexcept
		{
			return false;
		}
	#endif

	}

	void Dx12Device::Shutdown() noexcept {

		// Clear DRED globals before tearing down the device
		g_dredDevice = nullptr;
		g_breakCallback = nullptr;

		Dx12WaitQueueIdle(*queues.get(gfxHandle));
		Dx12WaitQueueIdle(*queues.get(compHandle));
		Dx12WaitQueueIdle(*queues.get(copyHandle));
		// Also drain any dynamically-created queues
		for (auto& slot : queues.slots) {
			if (slot.alive) Dx12WaitQueueIdle(slot.obj);
		}

		swapchains.clear();
		queryPools.clear();
		heaps.clear();
		timelines.clear();
		commandLists.clear();
		allocators.clear();
		descHeaps.clear();
		commandSignatures.clear();
		pipelines.clear();
		pipelineLayouts.clear();
		samplers.clear();
		resources.clear();

		queues.clear();
		gfxHandle = {}; compHandle = {}; copyHandle = {};

#if BUILD_TYPE == BUILD_DEBUG
		// Hold a temporary ref so we can report AFTER we drop our member refs.
		Microsoft::WRL::ComPtr<ID3D12Device> devForReport = pNativeDevice;
#endif

		// Drop SL Proxy objects
		pSLProxyDevice.Reset();
		pSLProxyFactory.Reset();

	#if BASICRHI_ENABLE_STREAMLINE
		if (steamlineInitialized) {
			slShutdown();
		}
	#endif

		// Drop DXGI objects
		adapter.Reset();
		pNativeFactory.Reset();

		pNativeDevice.Reset();

#if BUILD_TYPE == BUILD_DEBUG
		Dx12ReportLiveObjects(devForReport.Get(), "Dx12Device::Shutdown - D3D12 live objects (post-release)");
		DxgiReportLiveObjects("Dx12Device::Shutdown - DXGI live objects (post-release)");
		devForReport.Reset();
#endif

		Dx12ShutdownReShapeRuntime(this);
	}


	const DeviceVTable g_devvt = {
		&d_createPipelineFromStream,
		&d_createWorkGraph,
		&d_createPipelineLayout,
		&d_createCommandSignature,
		&d_createCommandAllocator,
		&d_createCommandList,
		&d_createSwapchain,
		&d_createDescriptorHeap,
		&d_createConstantBufferView,
		&d_createShaderResourceView,
		&d_createUnorderedAccessView,
		&d_createRenderTargetView,
		&d_createDepthStencilView,
		&d_createSampler,
		&d_createCommittedResource,
		&d_createTimeline,
		&d_createHeap,
		&d_createPlacedResource,
		&d_createQueryPool,

		&d_destroySampler,
		&d_destroyPipelineLayout,
		&d_destroyPipeline,
		&d_destroyWorkGraph,
		&d_destroyCommandSignature,
		&d_destroyCommandAllocator,
		&d_destroyCommandList,
		&d_destroySwapchain,
		&d_destroyDescriptorHeap,
		&d_destroyBuffer,
		&d_destroyTexture,
		&d_destroyTimeline,
		&d_destroyHeap,
		&d_destroyQueryPool,

		&d_getQueue,
		&d_createQueue,
		&d_destroyQueue,
		&d_waitIdle,
		&d_flushDeletionQueue,
		&d_getDescriptorHandleIncrementSize,
		&d_getTimestampCalibration,
		&d_getCopyableFootprints,
		&d_getResourceAllocationInfo,
		&d_queryFeatureInfo,
		&d_setResidencyPriority,
		&d_queryVideoMemoryInfo,

		&d_setNameBuffer,
		&d_setNameTexture,
		&d_setNameSampler,
		&d_setNamePipelineLayout,
		&d_setNamePipeline,
		&d_setNameCommandSignature,
		&d_setNameDescriptorHeap,
		&d_setNameTimeline,
		&d_setNameHeap,
		&d_checkDebugMessages,
		&d_getDebugInstrumentationCapabilities,
		&d_getDebugInstrumentationState,
		&d_getDebugInstrumentationFeatureCount,
		&d_copyDebugInstrumentationFeatures,
		&d_getDebugInstrumentationDiagnosticCount,
		&d_copyDebugInstrumentationDiagnostics,
		&d_getDebugInstrumentationIssueCount,
		&d_copyDebugInstrumentationIssues,
		&d_setDebugGlobalInstrumentationMask,
		&d_setDebugSynchronousRecording,
		&d_destroyDevice,
		6u
	};

	const QueueVTable g_qvt = {
		&q_submit,
		&q_signal,
		&q_wait,
		&q_checkDebugMessages,
		&q_setName,
		2u };

	const CommandAllocatorVTable g_calvt = {
		&ca_reset,
		1u
	};

	const CommandListVTable g_clvt = {
		&cl_end,
		&cl_reset,
		&cl_beginPass,
		&cl_endPass,
		&cl_barrier,
		&cl_bindLayout,
		&cl_bindPipeline,
		&cl_setVB,
		&cl_setIB,
		&cl_draw,
		&cl_drawIndexed,
		&cl_dispatch,
		cl_clearRTV_slot,
		cl_clearDSV_slot,
		&cl_executeIndirect,
		&cl_setDescriptorHeaps,
		&cl_clearUavUint,
		&cl_clearUavFloat,
		&cl_copyTextureToBuffer,
		&cl_copyBufferToTexture,
		&cl_copyTextureRegion,
		&cl_copyBufferRegion,
		&cl_writeTimestamp,
		&cl_beginQuery,
		&cl_endQuery,
		&cl_resolveQueryData,
		&cl_resetQueries,
		&cl_pushConstants,
		&cl_setPrimitiveTopology,
		&cl_dispatchMesh,
		&cl_setWorkGraph,
		&cl_dispatchWorkGraph,
		&cl_setName,
		1u
	};
	const SwapchainVTable g_scvt = {
		&sc_count,
		&sc_curr,
		//&sc_rtv,
		&sc_img,
		&sc_present,
		&sc_resizeBuffers,
		&sc_setName,
		1u };
	const ResourceVTable g_buf_rvt = {
		&buf_map,
		&buf_unmap,
		&buf_setName,
		1
	};
	const ResourceVTable g_tex_rvt = {
		&tex_map,
		&tex_unmap,
		&tex_setName,
		1
	};
	const QueryPoolVTable g_qpvt = {
		&qp_getQueryResultInfo,
		&qp_getPipelineStatsLayout,
		&qp_setName,
		1u
	};
	const PipelineVTable g_psovt = {
		&pso_setName,
		1u
	};
	const WorkGraphVTable g_wgvt = {
		&wg_setName,
		&wg_getRequiredScratchMemorySize,
		1u
	};
	const PipelineLayoutVTable g_plvt = {
		&pl_setName,
		1u
	};
	const CommandSignatureVTable g_csvt = {
		&cs_setName,
		1u
	};
	const DescriptorHeapVTable g_dhvt = {
		&dh_setName,
		1u
	};
	const SamplerVTable g_svt = {
		&s_setName,
		1u
	};
	const TimelineVTable g_tlvt = {
		&tl_timelineCompletedValue,
		&tl_timelineHostWait,
		&tl_setName,
		1u
	};
	const HeapVTable g_hevt = {
		&h_setName,
		1u
	};

	rhi::Result CreateD3D12Device(const DeviceCreateInfo& ci, DevicePtr& outPtr, bool enableStreamlineInterposer) noexcept
	{
		bool l_enableStreamline = enableStreamlineInterposer;
	#if !BASICRHI_ENABLE_STREAMLINE
		l_enableStreamline = false;
	#endif
		if (l_enableStreamline)
		{
			if (!InitSL())
			{
				spdlog::error("Failed to initialize NVIDIA Streamline. DLSS will not be available.");
				l_enableStreamline = false;
			}
		}

		UINT flags = 0;
		if (ci.enableDebug)
		{
			ComPtr<ID3D12Debug> dbg;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
				dbg->EnableDebugLayer(), flags |= DXGI_CREATE_FACTORY_DEBUG;
			}

			// Enable DRED auto-breadcrumbs and page fault reporting
			// Must be configured *before* D3D12 device creation.
			ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
				dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				spdlog::info("DRED auto-breadcrumbs and page fault reporting enabled.");
			}
		}

		auto impl = std::make_shared<Dx12Device>();
		impl->selfWeak = impl;
		Dx12Device* pImpl = impl.get();
		Dx12InitializeDebugInstrumentation(pImpl, ci);

		// Native factory/device
		CreateDXGIFactory2(flags, IID_PPV_ARGS(&impl->pNativeFactory));

		// Streamline manual hooking setup
		if (l_enableStreamline)
		{
	#if BASICRHI_ENABLE_STREAMLINE
			impl->steamlineInitialized = true;
			// IMPORTANT: slInit(pref.flags |= eUseManualHooking) must have been called
			// before this.

			impl->pSLProxyFactory = impl->pNativeFactory;
			{
				IDXGIFactory* fac = impl->pSLProxyFactory.Get();
				if (SL_FAILED(res, slUpgradeInterface(reinterpret_cast<void**>(&fac)))) {
					RHI_FAIL(Result::Failed);
				}
				impl->pSLProxyFactory.Attach(static_cast<IDXGIFactory7*>(fac));
			}
	#else
			l_enableStreamline = false;
			impl->pSLProxyFactory = impl->pNativeFactory;
	#endif
		}
		else
		{
			impl->pSLProxyFactory = impl->pNativeFactory;
		}

		ComPtr<IDXGIAdapter1> adapter;
		impl->pNativeFactory->EnumAdapters1(0, &adapter);
		adapter.As(&impl->adapter);

		if (ci.instrumentation.enableRuntimeInstrumentation && l_enableStreamline)
		{
			spdlog::warn("Disabling Streamline because GPU-Reshape instrumentation is enabled for this device.");
			Dx12AppendInstrumentationDiagnostic(
				pImpl,
				DebugInstrumentationDiagnosticSeverity::Warning,
				"Streamline is disabled while GPU-Reshape instrumentation is active on the same device.");
			l_enableStreamline = false;
		}

		bool reshapeWrappedDevice = false;
	#if BASICRHI_ENABLE_RESHAPE
		if (ci.instrumentation.enableRuntimeInstrumentation) {
			reshapeWrappedDevice = Dx12InitializeReShapeRuntime(pImpl, ci);
		}
	#endif

		if (reshapeWrappedDevice) {
			ComPtr<IDXGIFactory7> reshapeFactory;
			const HRESULT reshapeFactoryHr = CreateDXGIFactory2(flags, IID_PPV_ARGS(&reshapeFactory));
			if (FAILED(reshapeFactoryHr)) {
				RHI_FAIL(ToRHI(reshapeFactoryHr));
			}

			// The initial factory was created before the ReShape layer DLL was loaded, so
			// swapchains from that factory return native backbuffers. Refresh the factory now
			// that the layer is active so GetBuffer returns wrapped resources that match the
			// wrapped device stored in pNativeDevice.
			impl->pSLProxyFactory = reshapeFactory;
		}

		if (!reshapeWrappedDevice) {
			ComPtr<ID3D12Device> base;
			D3D12CreateDevice(impl->adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&base));

			auto hasDevice10 = base.As(&impl->pNativeDevice);
			if (FAILED(hasDevice10)) {
				RHI_FAIL(ToRHI(hasDevice10));
			}
		}

		// Streamline manual hooking setup
		if (l_enableStreamline)
		{
	#if BASICRHI_ENABLE_STREAMLINE
			// Tell SL which native device to use
			if (SL_FAILED(res, slSetD3DDevice(impl->pNativeDevice.Get()))) {
				RHI_FAIL(Result::Failed);
			}

			// Make proxy device/factory via slUpgradeInterface
			impl->pSLProxyDevice = impl->pNativeDevice;
			{
				ID3D12Device* dev = impl->pSLProxyDevice.Get();
				if (SL_FAILED(res, slUpgradeInterface(reinterpret_cast<void**>(&dev)))) {
					RHI_FAIL(Result::Failed);
				}
				impl->pSLProxyDevice.Attach(dev);
			}
	#else
			l_enableStreamline = false;
			impl->pSLProxyDevice = impl->pNativeDevice;
	#endif
		}
		else
		{
			impl->pSLProxyDevice = impl->pNativeDevice;
		}

		spdlog::info(
			"DX12 device setup: streamlineEnabled={} nativeDevice10={} proxyDevice={} samePointer={}",
			l_enableStreamline,
			static_cast<const void*>(impl->pNativeDevice.Get()),
			static_cast<const void*>(impl->pSLProxyDevice.Get()),
			impl->pNativeDevice.Get() == impl->pSLProxyDevice.Get());
		if (l_enableStreamline)
		{
	#if BASICRHI_ENABLE_STREAMLINE
			ID3D12Device* extractedNativeDevice = nullptr;
			if (SL_FAILED(res, slGetNativeInterface(impl->pSLProxyDevice.Get(), reinterpret_cast<void**>(&extractedNativeDevice))) || !extractedNativeDevice) {
				spdlog::warn(
					"DX12 device setup: slGetNativeInterface(proxyDevice) failed proxyDevice={}",
					static_cast<const void*>(impl->pSLProxyDevice.Get()));
			}
			else {
				spdlog::info(
					"DX12 device setup: proxy->native extractedDevice={} matchesStoredNative={}",
					static_cast<const void*>(extractedNativeDevice),
					extractedNativeDevice == impl->pNativeDevice.Get());
				extractedNativeDevice->Release();
			}
	#endif
		}

		if (ci.enableDebug) {
			EnableDebug(impl->pNativeDevice.Get());

			// Register the DRED device and break callback so BreakIfDebugging
			// automatically checks for device removal and dumps DRED data.
			g_dredDevice = impl->pNativeDevice.Get();
			g_breakCallback = &LogDredData;
		}

		// Queue creation: MUST go through proxy device, store both proxy+native
		auto makeQ = [&](D3D12_COMMAND_LIST_TYPE t, const wchar_t* debugName) -> QueueHandle
			{
				Dx12QueueState out{};
				D3D12_COMMAND_QUEUE_DESC qd{};
				qd.Type = t;

				// Hooked API: CreateCommandQueue must be invoked on proxy when SL enabled
				ComPtr<ID3D12CommandQueue> qProxy;
				HRESULT hr = impl->pSLProxyDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&qProxy));
				if (FAILED(hr)) { return {}; }

				out.pSLProxyQueue = qProxy;

				// Extract native queue for normal engine use to avoid proxies internally
				if (l_enableStreamline)
				{
				#if BASICRHI_ENABLE_STREAMLINE
					ID3D12CommandQueue* qNative = nullptr;
					if (SL_FAILED(res, slGetNativeInterface(out.pSLProxyQueue.Get(), (void**)&qNative)))
					{
						out.pNativeQueue = out.pSLProxyQueue;
					}
					else
					{
						out.pNativeQueue.Attach(qNative); // qNative is returned with refcount
					}
				#else
					out.pNativeQueue = out.pSLProxyQueue;
				#endif
				}
				else
				{
					out.pNativeQueue = out.pSLProxyQueue;
				}
				out.dev = impl.get();
				impl->pNativeDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&out.fence));
				out.value = 0;

				if (out.pNativeQueue) {
					out.pNativeQueue->SetName(debugName);
				}
				if (out.pSLProxyQueue && out.pSLProxyQueue.Get() != out.pNativeQueue.Get()) {
					out.pSLProxyQueue->SetName(debugName);
				}

				return impl->queues.alloc(out);
			};

		impl->gfxHandle  = makeQ(D3D12_COMMAND_LIST_TYPE_DIRECT,  L"DX12 Graphics Queue");
		impl->compHandle = makeQ(D3D12_COMMAND_LIST_TYPE_COMPUTE, L"DX12 Compute Queue");
		impl->copyHandle = makeQ(D3D12_COMMAND_LIST_TYPE_COPY,    L"DX12 Copy Queue");

		Device d{ pImpl, &g_devvt };
		outPtr = MakeDevicePtr(&d, impl);
		return Result::Ok;
	}

	namespace debug {
		std::vector<InstrumentationExecutionDetailSnapshot> GetInstrumentationExecutionDetails(
			Device device,
			const DebugInstrumentationIssue& issue) {
			(void)issue;
			std::vector<InstrumentationExecutionDetailSnapshot> details;
			if (!device || device.vt != &g_devvt || !device.impl) {
				return details;
			}

			#if BASICRHI_ENABLE_RESHAPE
			auto* impl = static_cast<Dx12Device*>(device.impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			return Dx12CollectExecutionDetailSnapshotsUnlocked(impl->debugInstrumentation, issue);
			#else
			return details;
			#endif
		}

		bool RetainInstrumentationExecutionDetail(
			Device device,
			uint64_t detailId) {
			if (!device || device.vt != &g_devvt || !device.impl || !detailId) {
				return false;
			}

			#if BASICRHI_ENABLE_RESHAPE
			auto* impl = static_cast<Dx12Device*>(device.impl);
			std::lock_guard guard(impl->debugInstrumentation.mutex);
			return Dx12RetainExecutionDetailUnlocked(impl->debugInstrumentation, detailId);
			#else
			return false;
			#endif
		}
	}


}