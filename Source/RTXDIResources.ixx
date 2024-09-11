module;

#include <DirectXMath.h>

#include "directx/d3d12.h"

#include "rtxdi/ImportanceSamplingContext.h"
#include "rtxdi/RISBufferSegmentAllocator.h"

export module RTXDIResources;

import CommandList;
import DeviceContext;
import GPUBuffer;
import Texture;

using namespace DirectX;
using namespace rtxdi;
using namespace std;

export {
	struct LightInfo {
		XMFLOAT3 Center;
		UINT Scalars;
		XMUINT2 Directions, Radiance;
	};

	struct RTXDIResources {
		unique_ptr<ImportanceSamplingContext> Context;

		unique_ptr<GPUBuffer>
			RIS,
			RISLightInfo,
			LightInfo,
			LightIndices,
			NeighborOffsets,
			DIReservoir;

		unique_ptr<Texture> LocalLightPDF;

		void CreateLightResources(const DeviceContext& deviceContext, UINT emissiveTriangleCount, UINT objectCount) {
			const auto RISBufferSegmentSize = Context->getRISBufferSegmentAllocator().getTotalSizeInElements();
			RIS = GPUBuffer::CreateDefault<XMUINT2>(deviceContext, RISBufferSegmentSize);
			RISLightInfo = GPUBuffer::CreateDefault<XMUINT4>(deviceContext, RISBufferSegmentSize * 2);

			LightInfo = GPUBuffer::CreateDefault<::LightInfo>(deviceContext, emissiveTriangleCount);
			LightIndices = GPUBuffer::CreateDefault<UINT>(deviceContext, objectCount);

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

		void CreateRenderSizeDependentResources(CommandList& commandList) {
			const auto& deviceContext = commandList.GetDeviceContext();

			const auto neighborOffsetCount = Context->getNeighborOffsetCount();
			vector<UINT16> offsets(neighborOffsetCount);
			FillNeighborOffsetBuffer(reinterpret_cast<uint8_t*>(data(offsets)), neighborOffsetCount);
			NeighborOffsets = GPUBuffer::CreateDefault<UINT16>(deviceContext, neighborOffsetCount, DXGI_FORMAT_R8G8_SNORM);
			NeighborOffsets->CreateSRV(BufferSRVType::Typed);
			commandList.Copy(*NeighborOffsets, offsets);
			commandList.SetState(*NeighborOffsets, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			DIReservoir = GPUBuffer::CreateDefault<RTXDI_PackedDIReservoir>(deviceContext, Context->getReSTIRDIContext().getReservoirBufferParameters().reservoirArrayPitch * c_NumReSTIRDIReservoirBuffers);
		}
	};
}
