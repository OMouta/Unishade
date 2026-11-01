#pragma once

// Upscales the finished frame with AMD's FSR 1 when effects run below 100%, and shows it over the swapchain with
// DirectComposition. Call it after effects and the menu are drawn, before Present.
// Never throws. Present reports a lost device, and other errors fall back to Windows' stretch.
void UpscaleFrame() noexcept;
// Call before releasing the graphics device.
void ReleaseUpscaler();
