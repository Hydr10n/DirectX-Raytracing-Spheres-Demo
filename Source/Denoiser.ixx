module;

#include <DirectXMath.h>

export module Denoiser;

export import NRD;
export import Streamline;

using namespace DirectX;

export {
	enum class Denoiser { None, DLSSRayReconstruction, NRDReBLUR, NRDReLAX };

	struct DenoisingSettings {
		Denoiser Denoiser;
		XMUINT3 _;
		XMFLOAT4 NRDReBLURHitDistance;
	};
}
