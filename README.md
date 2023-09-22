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
	- Vertical Field of View
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
	- Bloom
		- Threshold
		- Blur Size

### Supported Input Devices
- Xbox Controller
- Keyboard
- Mouse

---

## Minimum Build Requirements
### Development Tools
- Microsoft Visual Studio 2022 (17.4)

- vcpkg
	```powershell
	> git clone https://github.com/Microsoft/vcpkg
	> cd vcpkg
	> .\bootstrap-vcpkg.bat
	> .\vcpkg integrate install
	```

### Dependencies
- Windows 11 SDK (10.0.22621.0)

- [MathLib](https://github.com/NVIDIAGameWorks/MathLib)

- [NVIDIA Streamline](https://github.com/NVIDIAGameWorks/Streamline)

- [NVIDIA Real-Time Denoisers](https://github.com/NVIDIAGameWorks/RayTracingDenoiser)

- [NVIDIA Render Interface](https://github.com/NVIDIAGameWorks/NRI)

- [NVIDIA PhysX](https://github.com/NVIDIA-Omniverse/PhysX)
	```powershell
	> .\vcpkg install omniverse-physx-sdk:x64-windows
	```

- [DirectX Tool Kit for DirectX 12](https://github.com/Microsoft/DirectXTK12)
	```powershell
	> .\vcpkg install directxtk12:x64-windows
	```

- [DirectXTex Texture Processing Library](https://github.com/Microsoft/DirectXTK12)
	```powershell
	> .\vcpkg install directxtex[dx12,openexr]:x64-windows
	```

- [Dear ImGui](https://github.com/ocornut/imgui)
	```powershell
	> .\vcpkg install imgui[dx12-binding,win32-binding]:x64-windows
	```

- [JSON for Modern C++](https://github.com/nlohmann/json)
	```powershell
	> .\vcpkg install nlohmann-json:x64-windows
	```

## Minimum System Requirements
- OS: Microsoft Windows 10 64-bit, version 2004
- Graphics: Any GPU supporting DirectX Raytracing Tier 1.1
	- NVIDIA GeForce RTX Series
	- AMD Radeon RX 6000 Series
