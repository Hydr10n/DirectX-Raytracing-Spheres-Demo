module;

#include <DirectXMath.h>

#include "directx/d3d12.h"

#include "Rtxdi/ImportanceSamplingContext.h"
#include "Rtxdi/LightSampling/RISBufferSegmentAllocator.h"

export module RTXDIResources;

import CommandList;
import DeviceContext;
import GPUBuffer;
import Texture;

using namespace DirectX;
using namespace rtxdi;
using namespace std;

export {
	struct LightInfo { XMFLOAT3 Base, Edges[2], Radiance; };

	struct RTXDIResources {
		unique_ptr<ImportanceSamplingContext> Context;

		unique_ptr<GPUBuffer>
			RIS,
			LightInfo,
			LightIndices,
			NeighborOffsets,
			DIReservoir;

		unique_ptr<Texture> LocalLightPDF;

		void CreateLightResources(const DeviceContext& deviceContext, uint32_t emissiveTriangleCount, uint32_t objectCount) {
			const auto RISBufferSegmentSize = Context->GetRISBufferSegmentAllocator().getTotalSizeInElements();
			RIS = GPUBuffer::CreateDefault<XMUINT2>(deviceContext, RISBufferSegmentSize);

			LightInfo = GPUBuffer::CreateDefault<::LightInfo>(deviceContext, emissiveTriangleCount);
			LightIndices = GPUBuffer::CreateDefault<uint32_t>(deviceContext, objectCount);

			uint32_t width, height, mipLevels;
			ComputePdfTextureSize(emissiveTriangleCount, width, height, mipLevels);
			LocalLightPDF = make_unique<Texture>(
				deviceContext,
				Texture::CreationDesc{
					.Format = DXGI_FORMAT_R32_FLOAT,
					.Width = width,
					.Height = height,
					.MipLevels = static_cast<UINT16>(mipLevels)
				}.AsUnorderedAccess().AsRenderTarget()
				);
			LocalLightPDF->CreateSRV();
			LocalLightPDF->CreateUAV();
			LocalLightPDF->CreateRTV();
		}

		void ResetLightResources() {
			LightInfo.reset();
			LightIndices.reset();
			LocalLightPDF.reset();
		}

		void CreateRenderSizeDependentResources(CommandList& commandList) {
			const auto& deviceContext = commandList.GetDeviceContext();

			const auto neighborOffsetCount = Context->GetNeighborOffsetCount();
			vector<UINT16> offsets(neighborOffsetCount);
			FillNeighborOffsetBuffer(reinterpret_cast<uint8_t*>(data(offsets)), neighborOffsetCount);
			NeighborOffsets = GPUBuffer::CreateDefault<UINT16>(deviceContext, neighborOffsetCount, DXGI_FORMAT_R8G8_SNORM);
			NeighborOffsets->CreateSRV(BufferSRVType::Typed);
			commandList.Copy(*NeighborOffsets, offsets);
			commandList.SetState(*NeighborOffsets, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			DIReservoir = GPUBuffer::CreateDefault<RTXDI_PackedDIReservoir>(deviceContext, Context->GetReSTIRDIContext().GetReservoirBufferParameters().reservoirArrayPitch * c_NumReSTIRDIReservoirBuffers);
		}
	};
}
