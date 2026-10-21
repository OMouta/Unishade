#pragma once

#include <d3d11.h>
#include <cstdint>

// Loads the depth model from beside the exe and supplies ReShade's DEPTH texture through the host's
// add-on. Returns false when depth is not installed, and logs why when it is installed but unusable.
bool InitDepth();

// Estimates depth for the captured frame on a worker thread and publishes the newest result to ReShade.
// Frames that arrive while an estimate is in progress are skipped. Repeated input still publishes a completed result.
void UpdateDepth(ID3D11Texture2D* frame, int64_t frameTimestamp);

void ResetDepthInput();
void LogDepthDiagnostics();

// Whether depth is estimated, for the menu to offer its settings. False once it stopped on an error.
bool DepthEnabled();

// Releases everything depth made on the D3D11 device, after the device was lost. It is made again on the new device
// with the next frame. The model keeps running on its own D3D12 device.
void ReleaseDepthDevice();

void ShutdownDepth();
