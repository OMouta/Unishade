#pragma once

// DLSS5, NVIDIA's DLSS neural rendering, run on a color texture by calling nvngx_dlssnr.dll directly. Windows,
// D3D12 and NVIDIA only. The functions live in Unishade_nvngx.dll: see dlss5.cpp for why it has that name.

#include <stdbool.h>
#include <stddef.h>

#ifdef UNISHADE_DLSS5_EXPORTS
#define DLSS5_API __declspec(dllexport)
#else
#define DLSS5_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12Resource ID3D12Resource;
typedef struct Dlss5 Dlss5;

// The look settings, as nvngx_dlssnr.dll 310.8.0.0 reads them.
typedef struct Dlss5Look
{
    int style;            // DLSSNR.Style: 0 is Natural, 1 is Cinematic. Every other value gives one more look.
    float intensity;      // DLSSNR.Intensity: from the input unchanged at 0 to the full effect at 1. Clamped to that.
    float localTone;      // DLSSNR.LocalToneStrength. This and the next two are 1 by default and not clamped.
    float localStructure; // DLSSNR.LocalStructureStrength
    float skinStructure;  // DLSSNR.SkinStructureStrength
    bool autoMask;        // DLSSNR.UseAutoMask
    int passCount;        // How many times to run it, each run on the last one's output. 1 to 10.
} Dlss5Look;

// Receives Unishade's own steps and the DLL's log lines. Called from any thread, one line at a time.
typedef void (*Dlss5LogCallback)(const char* line);

// Makes a D3D12 device on the first NVIDIA adapter, loads nvngx_dlssnr.dll from runtimePath and starts NGX on the
// device. dataPath is a writable folder NGX keeps its files in. Only one may exist at a time, since NGX's log
// callback has no context. Returns null with the reason in error, which can be null.
DLSS5_API Dlss5* Dlss5Create(const wchar_t* runtimePath, const wchar_t* dataPath, Dlss5LogCallback log, char* error, size_t errorSize);

// The device the textures given to Dlss5Evaluate are made on.
DLSS5_API ID3D12Device* Dlss5Device(Dlss5* dlss5);

// Runs DLSS5 from color into output and waits for the GPU to finish. Both textures are on Dlss5Device's device, the
// same size and in the COMMON state, which they are left in. Output allows unordered access. Color is only read.
// A new size makes the feature again. Returns false with the reason in Dlss5Error.
DLSS5_API bool Dlss5Evaluate(Dlss5* dlss5, ID3D12Resource* color, ID3D12Resource* output, const Dlss5Look* look);

// Why the last Dlss5Evaluate failed.
DLSS5_API const char* Dlss5Error(const Dlss5* dlss5);

// Waits for the GPU, releases the features and shuts NGX down.
DLSS5_API void Dlss5Destroy(Dlss5* dlss5);

#ifdef __cplusplus
}
#endif
