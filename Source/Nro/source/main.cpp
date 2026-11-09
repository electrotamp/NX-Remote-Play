#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <exception>

#include "UI/UI.hpp"
#include "Platform/Platform.hpp"
#include "Platform/fs.hpp"
#include "UI/imgui/imgui.h"
#include "UI/Image.hpp"
#include "ipc.h"
#include "Network/UpdateCheck.hpp"
#include "Network/RendezvousClient.hpp"

#include "Scenes/Scenes.hpp"
#include "Scenes/Common.hpp"

#include "translaton.hpp"

Image::Img NxRemotePlayLogo;

namespace
{
Scene currentScene = Scene::FatalError;

std::string_view statusMessage = "Fatal error";
std::string_view errorSecondline = "This should never happen, try rebooting your console";

std::string formattedError;

void ImguiBindController()
{
    ImGuiIO &io = ImGui::GetIO();

    const float leftX = g_gamepad.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
    const float leftY = g_gamepad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
    const float stickLeft = leftX < -0.35f ? -leftX : 0.0f;
    const float stickRight = leftX > 0.35f ? leftX : 0.0f;
    const float stickUp = leftY < -0.35f ? -leftY : 0.0f;
    const float stickDown = leftY > 0.35f ? leftY : 0.0f;
    io.NavInputs[ImGuiNavInput_LStickLeft] = stickLeft;
    io.NavInputs[ImGuiNavInput_LStickRight] = stickRight;
    io.NavInputs[ImGuiNavInput_LStickUp] = stickUp;
    io.NavInputs[ImGuiNavInput_LStickDown] = stickDown;

    // This bundled Dear ImGui version's own widget-to-widget nav movement
    // (NavUpdateCreateMoveRequest in imgui.cpp) is hardcoded to read only
    // ImGuiNavInput_Dpad*/keyboard arrows for that purpose - the analog
    // LStick* slots above are only ever consumed for scrolling/dragging,
    // never for moving focus between widgets. Fold the stick into the
    // same Dpad slots so pushing it moves focus exactly like the D-pad,
    // everywhere that relies on ImGui's own nav (outside Settings, which
    // reads the stick directly and doesn't go through this at all).
    io.NavInputs[ImGuiNavInput_DpadDown] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] || stickDown > 0.0f;
    io.NavInputs[ImGuiNavInput_DpadUp] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] || stickUp > 0.0f;
    io.NavInputs[ImGuiNavInput_DpadLeft] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] || stickLeft > 0.0f;
    io.NavInputs[ImGuiNavInput_DpadRight] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] || stickRight > 0.0f;

    io.NavInputs[ImGuiNavInput_Activate] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_A];
    io.NavInputs[ImGuiNavInput_Cancel] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_B];

    io.NavInputs[ImGuiNavInput_Menu] = g_gamepad.buttons[GLFW_GAMEPAD_BUTTON_START];
}

} // namespace

namespace scenes
{

void FatalError()
{
    SetupMainWindow("Fatal error");
    CenterImage(NxRemotePlayLogo, 1);

    CenterText(statusMessage);
    if (errorSecondline.size() > 0)
    {
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(errorSecondline.data(), errorSecondline.data() + errorSecondline.size());
        ImGui::PopTextWrapPos();
    }

    ImGui::NewLine();
    CenterText(Strings::Error.SysmoduleConnectionTroubleshootLink);
    CenterText("nxremoteplay.com");

    ImGui::NewLine();
    if (ImGuiCenterButtons<std::string_view>({Strings::Error.FailExitButton}) != -1 ||
        ImGui::GetIO().NavInputs[ImGuiNavInput_Menu])
        app::RequestExit();

    ImGui::End();
}
} // namespace scenes

namespace app
{
void SetNextScene(Scene s)
{
    currentScene = s;
}

void FatalError(std::string_view message, std::string_view secondline)
{
    SetNextScene(Scene::FatalError);
    statusMessage = message;
    errorSecondline = secondline;
}

void RequestExit()
{
    Glfw::SetShouldClose();
}
} // namespace app

static void LogCrash(const char *what)
{
    fprintf(stderr, "UNHANDLED EXCEPTION: %s\n", what);
    // Keep uncaught exceptions in the user-exportable NRO log.
    if (FILE *g = fopen("/config/nxremoteplay/nro_log.txt", "a"))
    {
        fprintf(g, "UNHANDLED EXCEPTION: %s\n", what);
        fclose(g);
    }
    try
    {
        Network::Rendezvous::Connector::Shutdown();
    }
    catch (...)
    {
    }
}

static int RunApp()
{
    Platform::Init();

    Strings::ResetStringTable();

    if (!UI::Init())
    {
        Platform::Exit();
        return -1;
    }

    NxRemotePlayLogo = Image::Img(ASSET("logo.png"));

    currentScene = Scene::Host;
    scenes::InitHost();

    while (Glfw::MainLoop())
    {
        Platform::GetInputs();
        ImguiBindController();
        Platform::ImguiBindings();

        UI::StartFrame();

        // Render background
        ImGui::SetNextWindowSize({UI::WindowWidth, UI::WindowHeight});
        ImGui::SetNextWindowPos({0, 0});

        ImGui::Begin("_background_", 0, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration);

        UI::SmallFont();

        switch (currentScene)
        {
        case Scene::Host:
            scenes::Host();
            break;
        case Scene::FatalError:
            scenes::FatalError();
            break;
        default:
            Glfw::SetShouldClose();
            break;
        }

        UI::PopFont();
        ImGui::End();

        UI::EndFrame();
        // this 33 ms sleep capped the whole UI at ~23-30 loops/s even while the stream plays at 60 fps
        // (NRO telemetry: 23 UI loops/s with up to 13 frames queued per loop). While a stream is connected the
        // vsync'd swap (glfwSwapInterval(1)) already paces the loop at the display rate, so only sleep otherwise.
        const bool streaming = Network::Rendezvous::Connector::GetState() == Network::Rendezvous::State::Connected;
        Glfw::BlockPlusExit = streaming;
        if (!streaming)
            Platform::Sleep(1 / 30.0f * 1000);
    }

    // Stop connector workers before releasing resources and the NRO address space.
    Network::Rendezvous::Connector::Shutdown();
    Network::UpdateCheck::Join();
    UI::Exit();
    Platform::Exit();
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    // Log unexpected failures and stop connector workers before exiting.
    try
    {
        return RunApp();
    }
    catch (const std::exception &e)
    {
        LogCrash(e.what());
        return 1;
    }
    catch (...)
    {
        LogCrash("non-std::exception thrown");
        return 1;
    }
}
