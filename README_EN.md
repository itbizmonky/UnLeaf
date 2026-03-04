[English](README_EN.md) | [日本語](README.md)

# 🍃 UnLeaf - The Zero-Overhead EcoQoS Optimizer

**UnLeaf** is a hyper-optimized, event-driven background service for Windows 11 and 10 that surgically disables **EcoQoS (Efficiency Mode)** and Power Throttling for your specified applications. 

Designed for hardcore PC gamers, streamers, and power users, UnLeaf ensures your background tasks (like Discord, OBS, or server instances) never suffer from Windows' aggressive CPU throttling, keeping your frame rates high and system latency absolutely minimal.

## 🚀 Why UnLeaf? (The ETW Advantage)

Most traditional process optimization tools (like Process Lasso) rely on a **"Polling"** architecture. They constantly wake up your CPU every few seconds to check running processes, wasting CPU cycles and memory (often consuming 15-20MB and 0.5% CPU constantly).

**UnLeaf is different.** It utilizes an **Event-Driven Architecture** hooked directly into the Windows Kernel via ETW (Event Tracing for Windows). 

* **Zero CPU Overhead:** The engine sleeps at **0.00% CPU**. It only wakes up for a few milliseconds precisely when a process is created or destroyed.
* **Micro Memory Footprint:** Engineered with modern C++ and Win32 APIs, it consumes only **~3MB to 5MB** of memory even under heavy loads.
* **Instant Reaction:** Bypasses EcoQoS the exact millisecond a targeted child process is spawned. No polling delays.
* **No Memory Leaks:** rigorously tested to ensure flawless handle and memory cleanup over weeks of continuous uptime.

## 📦 Installation (For General Users)

If you just want to use the tool with its graphical interface, you **do not** need to build it from the source.

1. Go to the [Releases](../../releases) page.
2. Download the latest `UnLeaf_v1.x.x_Final.zip`.
3. Extract the folder and run `UnLeaf_Manager.exe`. (Admin privileges required for the initial service installation).
4. Add your favorite apps to the target list, click "Start Service", and let UnLeaf handle the rest completely in the background.

## 🛠️ Build Instructions (For Developers)

UnLeaf embraces an **Open Core Model**. The core engine (`UnLeaf_Service`) is completely open-source, allowing you to audit, build, and deploy the ultra-lightweight daemon yourself. *(Note: The UI Manager is closed-source and not included in this repository).*

### Prerequisites
* Windows 10/11 SDK
* CMake (3.20 or higher)
* MSVC Compiler (Visual Studio 2022 recommended)

### Build the Service Engine
Clone the repository and build using standard CMake commands. The `CMakeLists.txt` is dynamically configured to build only the OSS components if the Manager UI is not present.

```powershell
git clone [https://github.com/itbizmonky/UnLeaf.git](https://github.com/itbizmonky/UnLeaf.git)
cd UnLeaf
mkdir build
cd build
cmake ..
cmake --build . --config Release