#pragma once

#include "DirectXTK12/Effects.h"

namespace DirectX {
	struct TemporalAntiAliasingEffect : IEffect {
		struct { float Alpha = 0.2f, ColorBoxSigma = 1; } Constants;

		struct {
			SIZE Size;
			D3D12_GPU_DESCRIPTOR_HANDLE PreviousOutputSRV, CurrentOutputSRV, MotionVectorsSRV, FinalOutputUAV;
		} Textures{};

		TemporalAntiAliasingEffect(ID3D12Device* device);

		void Apply(ID3D12GraphicsCommandList* commandList) override;

	private:
		ID3D12Device* m_device;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineStateObject;
	};
}
