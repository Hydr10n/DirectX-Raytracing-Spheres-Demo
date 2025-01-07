module;

#include <d3d12.h>

export module DeviceContext;

import DescriptorHeap;

export {
	namespace D3D12MA {
		class Allocator;
	}

	namespace rtxmu {
		class DxAccelStructManager;
	}

	namespace DirectX {
		struct DeviceContext {
			ID3D12Device5* const Device;
			ID3D12CommandQueue* const CommandQueue;
			D3D12MA::Allocator* const MemoryAllocator;
			rtxmu::DxAccelStructManager* const AccelerationStructureManager;
			DescriptorHeapEx
				* const DefaultDescriptorHeap,
				* const ResourceDescriptorHeap,
				* const RenderDescriptorHeap,
				* const DepthStencilDescriptorHeap;

			operator ID3D12Device5* () const noexcept { return Device; }
		};
	}
}
