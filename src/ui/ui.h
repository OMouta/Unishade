#pragma once

#include "app/app.h"

// Dear ImGui for the launcher, in its window and over the game.
bool InitUi(UiWindow& ui, std::string& error);
void ShutdownUi(UiWindow& ui);
// Gives the window the logo as its icon, or in a beta the beta's icon. macOS shows the app's icon instead.
void SetWindowIcon(GLFWwindow* window);

// Starts a Dear ImGui frame in the window's context.
void BeginUi(UiWindow& ui);
// Draws the frame into the window's swapchain image, which BeginFrame acquired, and presents it.
void EndUi(UiWindow& ui);

void DrawLauncher(App& app);
// The launcher as the menu when it is open, and short messages at the bottom of the game.
void DrawOverlay(App& app);
// Ends what only lasts while the menu is open, such as recording a shortcut.
void ResetMenu(App& app);
// Ends the menu's way out, so it opens anew next time. DrawOverlay does once it is gone.
void ForgetMenu(App& app);
// Records what the interface shows of output, the effects' picture in VK_IMAGE_LAYOUT_GENERAL, into the overlay's
// commands before its Dear ImGui frame: the frost behind the menu while it shows, the launcher's preview while it is
// asked for, and the thumbnail of a screenshot this frame takes.
void DrawGamePictures(App& app, VkCommandBuffer commands, const GpuImage& output, bool screenshot);
// Call before the graphics card's device goes, after ShutdownUi.
void ReleaseGamePictures();
