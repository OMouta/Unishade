#pragma once

#include <windows.h>
#include <stop_token>

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

// Asks the game's display about once a second whether HDR is on and how bright SDR white is, and captures in the
// matching format. Call it from the frame loop while capturing.
void UpdateHdr();

// Slows capture while the overlay is hidden. Older Windows versions pause it until the overlay shows again.
void SetCaptureIdle(bool idle);

// Copies the newest captured frame into the overlay swapchain, creating or resizing it as needed.
// Returns false while the presentation queue is full, leaving the newest frame pending.
bool PresentLatestFrame();

// Does the message loop's frame work once: closes a game that went away, places or hides the overlay and shows the
// newest frame. A window procedure calls it from a timer while a modal loop, such as moving a window or a menu, keeps
// the message loop from running. Never throws, does nothing when called again from inside itself, and pumps no
// messages.
void PumpFrames() noexcept;

// The message loop waits for capacity without blocking capture or window messages.
HANDLE FrameLatencyEvent();
void NotifyFrameReady();

// Logs a render stage that stays blocked for two seconds, even if the host thread stops responding.
void MonitorFrames(std::stop_token stop);
void LogGraphicsMemory();
void LogFrameTimings();
