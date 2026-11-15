#pragma once

#include "app.h"

// Dear ImGui for the launcher, in its window and over the game.
bool InitUi(UiWindow& ui, std::string& error);
void ShutdownUi(UiWindow& ui);
// Gives the window the logo as its icon. macOS shows the app's icon instead.
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
