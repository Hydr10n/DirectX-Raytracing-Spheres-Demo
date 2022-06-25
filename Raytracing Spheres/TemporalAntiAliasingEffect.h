#pragma once

#include "directxtk12/Effects.h"

#include "wrl.h"

namespace DirectX {
	struct TemporalAntiAliasingEffect : IEffect {
		struct { float Alpha = 0.2f, ColorBoxSigma = 1; } Constant;

		SIZE TextureSize;

		struct { D3D12_GPU_DESCRIPTOR_HANDLE PreviousOutputSRV, CurrentOutputSRV, MotionVectorsSRV, FinalOutputUAV; } TextureDescriptors;

		TemporalAntiAliasingEffect(ID3D12Device* device);

		void Apply(ID3D12GraphicsCommandList* commandList) override;

	private:
		ID3D12Device* m_device;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
