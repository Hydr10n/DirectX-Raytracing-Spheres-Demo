# DirectX Raytracing Spheres Demo

Real-time physically based rendering using GPU capable of DirectX Raytracing.

![Raytracing Spheres](Screenshots/Raytracing-Spheres.png)

https://github.com/user-attachments/assets/20e60f87-81ce-409c-8fed-74e4cb35fbec

---

## Features
- PBR Metallic/Roughness Workflow
- Rigid-Body Simulation Using NVIDIA PhysX
- GPU Upload Heap

## Graphics Settings
- Window Mode: Windowed | Borderless | Fullscreen
- Resolution
- HDR
- V-Sync
- NVIDIA Reflex
- Camera
	- Jitter
	- Horizontal Field of View
- Raytracing
	- Russian Roulette
	- Bounces
	- Samples/Pixel
	- NVIDIA Shader Execution Reordering
	- NVIDIA RTXDI
		- ReSTIR DI
			- ReGIR
				- Cell
					- Size
				- Build Samples
			- Initial Sampling
				- Local Light
					- Mode: Uniform | Power RIS | ReGIR RIS
					- Samples
				- BRDF Samples
			- Temporal Resampling
				- Bias Correction Mode: Basic | Pairwise | Raytraced
				- Boiling Filter Strength
			- Spatial Resampling
				- Bias Correction Mode: Basic | Pairwise | Raytraced
				- Samples
	- NVIDIA RTXGI
		- SHARC
			- Downscale Factor
			- Scene Scale
			- Roughness Threshold
- Post-Processing
	- Super Resolution: NVIDIA DLSS | Intel XeSS
	- Denoising
		- Denoiser: NVIDIA DLSS Ray Reconstruction | NVIDIA ReBLUR | NVIDIA ReLAX
	- Frame Generation: NVIDIA DLSS
	- NVIDIA Image Scaling
		- Sharpness
	- Chromatic Aberration
	- Bloom
		- Strength
	- Tone Mapping
		- HDR
			- Paper White Nits
			- Color Rotation: Rec.709 to Rec.2020 | DCI-P3-D65 to Rec.2020 | Rec.709 to DCI-P3-D65
		- Non-HDR
			- Operator: Saturate | Reinhard | ACES Filmic
			- Exposure

---

## Minimum Build Requirements
### Development Tools
- Microsoft Visual Studio 2022 (17.4)

- CMake (3.28)

- Git with LFS

- vcpkg
	```powershell
	> git clone https://github.com/Microsoft/vcpkg
	> cd vcpkg
	> .\bootstrap-vcpkg.bat
	> [Environment]::SetEnvironmentVariable("VCPKG_ROOT", $PWD.Path, [EnvironmentVariableTarget]::User)
	```

### Dependencies
- Git Submodule
	```powershell
	> git submodule update --init --recursive
	```
