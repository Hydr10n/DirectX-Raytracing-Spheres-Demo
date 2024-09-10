module;

#include "D3D12MemAlloc.h"

export module DeviceContext;

import DescriptorHeap;

using namespace D3D12MA;

export namespace DirectX {
	struct DeviceContext {
		ID3D12Device5* Device;
		ID3D12CommandQueue* CommandQueue;
		Allocator* MemoryAllocator;
		DescriptorHeapEx* DefaultDescriptorHeap, * ResourceDescriptorHeap, * RenderDescriptorHeap, * DepthStencilDescriptorHeap;
	};
}
