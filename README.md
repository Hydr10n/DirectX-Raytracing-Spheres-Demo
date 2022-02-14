# DirectX Raytracing Spheres Demo

Real-time rendering using DirectX Raytracing with compatible GPUs and physics simulation using NVIDIA PhysX.

![Raytracing Spheres](Screenshots/Raytracing-Spheres.png)

https://user-images.githubusercontent.com/39995363/150128189-1301be4b-1961-446c-98f5-4541a75a80b9.mp4

---

## Controls
- Xbox Controller
    |||
    |-|-|
    |Menu button|Pause/resume|
    |LS (rotate)|Move|
    |LS (rotate + press)|Move faster|
    |RS (rotate)|Look around|
    |X button|Toggle gravity of Earth|
    |B button|Toggle gravity of the star|

- Keyboard
    |||
    |-|-|
    |Alt + Enter|Toggle between windowed/borderless and fullscreen mode|
    |Esc|Pause/resume|
    |W A S D|Move|
    |Left shift + W A S D|Move faster|
    |G|Toggle gravity of Earth|
    |H|Toggle gravity of the star|

- Mouse
    |||
    |-|-|
    |(Move)|Look around|

### Settings in Content Menu
- Window Mode: Windowed/borderless/fullscreen
- Resolution
- Anti-Aliasing: Off/2x/4x/8x

---

## Minimum Build Requirements
### Development Tools
- Microsoft Visual Studio 2022

- vcpkg
```cmd
> git clone https://github.com/Microsoft/vcpkg
> .\vcpkg\bootstrap-vcpkg.bat
> .\vcpkg\vcpkg integrate install
```

### Dependencies
- [PhysX](https://github.com/NVIDIAGameWorks/PhysX)
```cmd
> .\vcpkg\vcpkg install physx:x64-windows
```

## Minimum System Requirements
- OS: Microsoft Windows 10 64-bit, version 1809
- Graphics
    - NVIDIA GeForce GTX 1060 with 6 GB VRAM
    - NVIDIA GeForce GTX 1660
    - AMD Radeon RX 6000 Series
