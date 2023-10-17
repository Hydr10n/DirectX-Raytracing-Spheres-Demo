# DirectX Raytracing Spheres Demo

Real-time physically based rendering using GPU capable of DirectX Raytracing.

![Raytracing Spheres](Screenshots/Raytracing-Spheres-01.png)
![Raytracing Spheres](Screenshots/Raytracing-Spheres-02.png)
![Raytracing Spheres](Screenshots/Raytracing-Spheres-03.png)
![Raytracing Spheres](Screenshots/Raytracing-Spheres-04.png)
![Raytracing Spheres](Screenshots/Raytracing-Spheres-05.png)

https://user-images.githubusercontent.com/39995363/150128189-1301be4b-1961-446c-98f5-4541a75a80b9.mp4

https://user-images.githubusercontent.com/39995363/219614024-f2b1b53f-d738-4d6c-b127-78f538b594cf.mp4

---

## Features
### Inline Raytracing
- Diffuse Reflection
- Specular Reflection
- Specular Transmission

### PBR Metallic/Roughness Workflow

### Rigid-Body Simulation Using NVIDIA PhysX

### Graphics Settings
- Window Mode
- Resolution
- V-Sync
- Camera
	- Jitter
	- Horizontal Field of View
- Raytracing
	- Russian Roulette
	- Max Number of Bounces
	- Samples Per Pixel
- Post-Processing
	- NVIDIA Real-Time Denoisers
		- Validation Overlay
		- Split Screen
	- Temporal Anti-Aliasing
	- NVIDIA DLSS
		- Super Resolution
	- NVIDIA Image Scaling
		- Sharpness
	- Chromatic Aberration
	- Bloom
		- Threshold
		- Blur Size
	- Tone Mapping
		- Operator
		- Exposure

### Supported Input Devices
- Xbox Controller
- Keyboard
- Mouse

---

## Minimum Build Requirements
### Development Tools
- Microsoft Visual Studio 2022 (17.4)

- CMake (3.28)

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

- Windows 11 SDK (10.0.22621.0)

## Minimum System Requirements
- OS: Microsoft Windows 10 64-bit, version 2004
- Graphics: Any GPU supporting DirectX Raytracing Tier 1.1
	- NVIDIA GeForce RTX Series
	- AMD Radeon RX 6000 Series
