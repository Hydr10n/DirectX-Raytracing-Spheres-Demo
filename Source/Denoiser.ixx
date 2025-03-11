module;

export module Denoiser;

export import NRD;
export import Streamline;

export enum class Denoiser { None, DLSSRayReconstruction, NRDReBLUR, NRDReLAX };
