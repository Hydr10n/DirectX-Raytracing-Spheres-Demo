module;

#include "directx/d3d12.h"

#include "directxtk12/ResourceUploadBatch.h"

#include <DirectXMath.h>

#include "rtxdi/ReSTIRDI.h"

export module RTXDIResources;

import DescriptorHeap;
import GPUBuffer;

using namespace DirectX;
using namespace rtxdi;
using namespace std;

export {
	struct RAB_LightInfo { XMFLOAT3 Base, Edge1, Edge2, Radiance; };

	struct RTXDIResources {
		unique_ptr<ReSTIRDIContext> ReSTIRDIContext;

		shared_ptr<DefaultBuffer<RAB_LightInfo>> LightInfo;
		shared_ptr<DefaultBuffer<UINT>> LightIndices;
		shared_ptr<DefaultBuffer<UINT16>> NeighborOffsets;
		shared_ptr<DefaultBuffer<RTXDI_PackedDIReservoir>> DIReservoir;

		void Initialize(ID3D12Device* pDevice, UINT emissiveTriangleCount, UINT objectCount) {
			LightInfo = make_shared<DefaultBuffer<RAB_LightInfo>>(pDevice, emissiveTriangleCount);

			LightIndices = make_shared<DefaultBuffer<UINT>>(pDevice, objectCount);
		}

		void Initialize(ID3D12Device* pDevice, ResourceUploadBatch& resourceUploadBatch, D3D12_CPU_DESCRIPTOR_HANDLE neighborOffsetsDescriptor) {
			{
				const auto neighborOffsetCount = ReSTIRDIContext->getStaticParameters().NeighborOffsetCount;
				vector<UINT16> offsets(neighborOffsetCount);
				FillNeighborOffsetBuffer(reinterpret_cast<uint8_t*>(data(offsets)), neighborOffsetCount);
				NeighborOffsets = make_shared<DefaultBuffer<UINT16>>(pDevice, resourceUploadBatch, offsets);
				NeighborOffsets->CreateTypedSRV(neighborOffsetsDescriptor, DXGI_FORMAT_R8G8_SNORM);
			}

			DIReservoir = make_shared<DefaultBuffer<RTXDI_PackedDIReservoir>>(pDevice, ReSTIRDIContext->getReservoirBufferParameters().reservoirArrayPitch * c_NumReSTIRDIReservoirBuffers);
		}
	};
}