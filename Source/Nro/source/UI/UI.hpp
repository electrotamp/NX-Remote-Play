#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "glad.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace UI
{
constexpr uint32_t WindowWidth = 1280;
constexpr uint32_t WindowHeight = 720;

extern GLFWwindow *MainWindow;

extern float WRatio;
extern float HRatio;

void SmallFont();
ImFont *BoldTypeface();
void PopFont();

bool Init();
void Exit();

void StartFrame();
void EndFrame();

} // namespace UI

namespace Glfw
{
// while true, a window-close request raised by the + button is ignored (see MainLoop in UI.cpp).
extern bool BlockPlusExit;
bool MainLoop();
void SetShouldClose();
} // namespace Glfw
