module;

#include <DirectXMath.h>

#include "directx/d3d12.h"

#include "directxtk12/DescriptorHeap.h"
#include "directxtk12/ResourceUploadBatch.h"

#include "rtxdi/ImportanceSamplingContext.h"

export module RTXDIResources;

import DescriptorHeap;
import GPUBuffer;

using namespace DirectX;
using namespace rtxdi;
using namespace std;

export {
	struct RAB_LightInfo {
		XMFLOAT3 Center;
		UINT Scalars, Directions[2];
		XMUINT2 Radiance;
	};

	struct RTXDIResources {
		unique_ptr<ImportanceSamplingContext> Context;

		unique_ptr<DefaultBuffer<RAB_LightInfo>> LightInfo;
		unique_ptr<DefaultBuffer<UINT>> LightIndices;

		unique_ptr<DefaultBuffer<UINT16>> NeighborOffsets;

		unique_ptr<DefaultBuffer<RTXDI_PackedDIReservoir>> DIReservoir;

		void CreateLightBuffers(ID3D12Device* pDevice, UINT emissiveTriangleCount, UINT objectCount) {
			LightInfo = make_unique<DefaultBuffer<RAB_LightInfo>>(pDevice, emissiveTriangleCount);
			LightIndices = make_unique<DefaultBuffer<UINT>>(pDevice, objectCount);
		}

		void CreateNeighborOffsets(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, const DescriptorHeap& descriptorHeap, UINT neighborOffsetsDescriptorIndex) {
			const auto neighborOffsetCount = Context->getNeighborOffsetCount();
			vector<UINT16> offsets(neighborOffsetCount);
			FillNeighborOffsetBuffer(reinterpret_cast<uint8_t*>(data(offsets)), neighborOffsetCount);
			NeighborOffsets = make_unique<DefaultBuffer<UINT16>>(pDevice, resourceUploadBatch, offsets);
			NeighborOffsets->CreateTypedSRV(descriptorHeap, neighborOffsetsDescriptorIndex, DXGI_FORMAT_R8G8_SNORM);
		}

		void CreateDIReservoir(ID3D12Device* pDevice) {
			DIReservoir = make_unique<DefaultBuffer<RTXDI_PackedDIReservoir>>(pDevice, Context->getReSTIRDIContext().getReservoirBufferParameters().reservoirArrayPitch * c_NumReSTIRDIReservoirBuffers);
		}
	};
}
