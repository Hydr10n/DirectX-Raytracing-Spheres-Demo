# DirectX Raytracing Spheres Demo

Real-time rendering using DirectX Raytracing with compatible GPUs and physics simulation using NVIDIA PhysX.

![Raytracing Spheres](Screenshots/Raytracing-Spheres.png)

https://user-images.githubusercontent.com/39995363/146579832-2433f438-c669-4486-9e1e-d57950d340fc.mp4


https://user-images.githubusercontent.com/39995363/150128189-1301be4b-1961-446c-98f5-4541a75a80b9.mp4

---

## Controls
- Xbox Controller
    |||
    |-|-|
    |D-pad|Translate camera X/Y|
    |LS/RS (rotate)|Orbit camera X/Y|
    |LS (press)|Return camera to default focus/radius|
    |RS (press)|Reset camera|
    |LB/RB|Decrease/increase camera translation sensitivity|
    |LB + RB|Reset camera translation sensitivity|
    |A button|Toggle gravity of Earth|
    |B button|Toggle gravity of the star|

- Keyboard
    |||
    |-|-|
    |Alt + Enter|Toggle between windowed/borderless and fullscreen mode|
    |W A S D ↑ ← ↓ →|Translate camera X/Y|
    |Shift + W A S D ↑ ← ↓ →|Translate camera X/Y at half speed|
    |PageUp/PageDown|Translate camera Z|
    |Home|Reset camera|
    |End|Return camera to default focus/radius|
    |G|Toggle gravity of Earth|
    |H|Toggle gravity of the star|

- Mouse
    |||
    |-|-|
    |Left button (drag)|Orbit camera X/Y|
    |Scroll wheel|Increase/decrease camera orbit radius|

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
> .\vcpkg\vcpkg install physx physx:x64-windows
```

## Minimum System Requirements
- OS: Microsoft Windows 10 64-bit, version 1809
- Graphics: NVIDIA GeForce GTX 10 Series or AMD Radeon RX 5000 Series
