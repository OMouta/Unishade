#pragma once

#include <windows.h>

// D3D11 device shared by the capture pool and the overlay swapchain.
void CreateDevice();

// Whether error means the device is gone, such as after a driver update, a GPU reset or a switch to another GPU. Any
// error counts once the device reports that it was removed.
bool DeviceLost(HRESULT error);

// Ends capture and releases the swapchain, depth's resources and the device, after the device was lost. ReShade
// destroys its effect runtime with the swapchain and makes a new one for the next. CreateDevice makes a new device.
void ReleaseDevice();

// Asks Windows once, without waiting, to allow capture without the border it draws around the captured window.
void RequestBorderlessCapture();

// Starts Windows.Graphics.Capture on the target game window and records it as the target.
void StartCapture(HWND target);

// Ends capture, releases input, and clears the target.
void StopCapture();

// Slows capture while the overlay is hidden. Older Windows versions pause it until the overlay shows again.
void SetCaptureIdle(bool idle);

// Copies the newest captured frame into the overlay swapchain, creating or resizing it as needed.
void PresentLatestFrame();
