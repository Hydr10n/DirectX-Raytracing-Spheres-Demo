module;

#include <DirectXMath.h>

#include "directx/d3d12.h"

#include "directxtk12/ResourceUploadBatch.h"

#include "rtxdi/ImportanceSamplingContext.h"
#include "rtxdi/RISBufferSegmentAllocator.h"

export module RTXDIResources;

import DescriptorHeap;
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

		unique_ptr<DefaultBuffer<XMUINT2>> RIS;
		unique_ptr<DefaultBuffer<XMUINT4>> RISLightInfo;
		unique_ptr<DefaultBuffer<LightInfo>> LightInfo;
		unique_ptr<DefaultBuffer<UINT>> LightIndices;
		unique_ptr<Texture> LocalLightPDF;

		unique_ptr<DefaultBuffer<UINT16>> NeighborOffsets;

		unique_ptr<DefaultBuffer<RTXDI_PackedDIReservoir>> DIReservoir;

		void CreateLightResources(ID3D12Device* pDevice, UINT emissiveTriangleCount, UINT objectCount) {
			const auto RISBufferSegmentSize = Context->getRISBufferSegmentAllocator().getTotalSizeInElements();
			RIS = make_unique<DefaultBuffer<XMUINT2>>(pDevice, RISBufferSegmentSize);
			RISLightInfo = make_unique<DefaultBuffer<XMUINT4>>(pDevice, RISBufferSegmentSize * 2);

			LightInfo = make_unique<DefaultBuffer<::LightInfo>>(pDevice, emissiveTriangleCount);
			LightIndices = make_unique<DefaultBuffer<UINT>>(pDevice, objectCount);

			XMUINT2 textureSize;
			UINT mipLevels;
			ComputePdfTextureSize(emissiveTriangleCount, textureSize.x, textureSize.y, mipLevels);
			LocalLightPDF = make_unique<Texture>(pDevice, DXGI_FORMAT_R32_FLOAT, textureSize, static_cast<UINT16>(mipLevels));
		}

		void CreateRenderSizeDependentResources(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const DescriptorHeapEx& descriptorHeap, UINT descriptorIndex) {
			const auto neighborOffsetCount = Context->getNeighborOffsetCount();
			vector<UINT16> offsets(neighborOffsetCount);
			FillNeighborOffsetBuffer(reinterpret_cast<uint8_t*>(data(offsets)), neighborOffsetCount);
			NeighborOffsets = make_unique<DefaultBuffer<UINT16>>(pDevice, resourceUploadBatch, offsets);
			NeighborOffsets->CreateTypedSRV(descriptorHeap, descriptorIndex, DXGI_FORMAT_R8G8_SNORM);

			DIReservoir = make_unique<DefaultBuffer<RTXDI_PackedDIReservoir>>(pDevice, Context->getReSTIRDIContext().getReservoirBufferParameters().reservoirArrayPitch * c_NumReSTIRDIReservoirBuffers);
		}
	};
}
