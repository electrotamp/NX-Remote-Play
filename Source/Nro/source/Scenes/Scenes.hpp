#pragma once

#include <cstdint>
#include <string_view>

enum class Scene
{
    Host,

    FatalError,
};

// Each implemented in its own scenes cpp file
namespace scenes
{
void InitHost();
void Host();
void FatalError();
} // namespace scenes

// from main.cpp
namespace app
{
void FatalError(std::string_view message, std::string_view secondline);

void SetNextScene(Scene s);

void RequestExit();
} // namespace app
