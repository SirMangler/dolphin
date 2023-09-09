// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "ImGuiFrontend.h"

#include "DolphinWinRT/UWPUtils.h"
#include "DolphinWinRT/Host.h"

#include "ImGuiNetplay.h"
#include "WinRTKeyboard.h"

#include <imgui.h>

#ifdef WINRT_XBOX
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/windows.graphics.display.core.h>
#include <winrt/windows.gaming.input.h>
#include <windows.applicationmodel.h>
#include <gamingdeviceinformation.h>
#endif

#include <unordered_map>
#include <wil/com.h>

#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/TitleDatabase.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/ConfigManager.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Config/UISettings.h"

#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/Timer.h"
#include "Common/FileSearch.h"

#include "UICommon/UICommon.h"
#include "UICommon/GameFile.h"
#include "UICommon/GameFileCache.h"

#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/VideoBackendBase.h"

#include "InputCommon/InputConfig.h"
#include "InputCommon/ControllerInterface/WGInput/WGInput.h"

namespace WGI = winrt::Windows::Gaming::Input;
using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt;

namespace ImGuiFrontend {

constexpr const char* PROFILES_DIR = "Profiles/";
std::vector<std::string> m_wiimote_profiles;
std::string m_selected_wiimote_profile[] = {"", "", "", ""};
std::vector<std::string> m_gc_profiles;
std::string m_selected_gc_profile[] = {"", "", "", ""};
std::vector<std::string> m_paths;
bool m_show_path_warning = false;
float m_frame_scale = 1.0f;
int m_selectedGameIdx;
UIState m_state = UIState();
CarouselCategory m_ccat = CarouselCategory::CAll;
std::map<std::string, FrontendTheme> m_themes;
FrontendTheme* m_selected_theme;

ImGuiFrontend::ImGuiFrontend()
{
  WindowSystemInfo wsi;

#ifdef WINRT_XBOX
  // To-Do: Handle other platforms, extract this code so it can be done by the host!

  CoreWindow window = CoreWindow::GetForCurrentThread();
  void* abi = winrt::get_abi(window);

  wsi.type = WindowSystemType::Windows;
  wsi.render_surface = abi;
  wsi.render_width = window.Bounds().Width;
  wsi.render_height = window.Bounds().Height;

  GAMING_DEVICE_MODEL_INFORMATION info = {};
  GetGamingDeviceModelInformation(&info);
  if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
  {
    winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation hdi =
        winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView();
    if (hdi)
    {
      constexpr float frontend_modifier = 1.8f;
      uint32_t width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();

      m_frame_scale = ((float) width / 1920.0f) * frontend_modifier;
      wsi.render_width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
      wsi.render_height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
      // Our UI is based on 1080p, and we're adding a modifier to zoom in by 80%
      wsi.render_surface_scale = ((float) wsi.render_width / 1920.0f) * 1.8f;
    }
  }
#endif

  // Manually reactivate the video backend in case a GameINI overrides the video backend setting.
  VideoBackendBase::PopulateBackendInfo(wsi);

  // Issue any API calls which must occur on the main thread for the graphics backend.
  WindowSystemInfo prepared_wsi(wsi);
  g_video_backend->PrepareWindow(prepared_wsi);

  VideoBackendBase::PopulateBackendInfo(wsi);
  if (!g_video_backend->Initialize(wsi))
  {
    PanicAlertFmt("Failed to initialize video backend!");
    return;
  }

  ImGui::GetIO().KeyMap[ImGuiKey_Backspace] = '\b';

  std::string profiles_path =
      File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR + Wiimote::GetConfig()->GetProfileName();
  for (const auto& filename : Common::DoFileSearch({profiles_path}, {".ini"}))
  {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    if (!basename.empty())
      m_wiimote_profiles.emplace_back(basename);
  }

  m_wiimote_profiles.emplace_back("None");
  m_wiimote_profiles.emplace_back("Wiimote + Nunchuk");
  m_wiimote_profiles.emplace_back("Classic Controller");
  m_wiimote_profiles.emplace_back("Sideways Wiimote");

  profiles_path =
      File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR + Pad::GetConfig()->GetProfileName();
  for (const auto& filename : Common::DoFileSearch({profiles_path}, {".ini"}))
  {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    if (!basename.empty())
      m_gc_profiles.emplace_back(basename);
  }

  m_gc_profiles.emplace_back("None");
  m_gc_profiles.emplace_back("Default");

  Wiimote::LoadConfig();
  Pad::LoadConfig();

  PopulateControls();
  LoadGameList();
  LoadThemes();

  m_ccat = (CarouselCategory) Config::Get(Config::FRONTEND_LAST_CATEGORY);
  m_last_category = m_ccat;
  FilterGamesForCategory();

  m_selectedGameIdx = Config::Get(Config::FRONTEND_LAST_GAME);

  if (m_selectedGameIdx >= m_displayed_games.size() || m_selectedGameIdx < 0)
    m_selectedGameIdx = 0;
}

void ImGuiFrontend::PopulateControls()
{
  g_controller_interface.RefreshDevices();

  if (!g_controller_interface.HasDefaultDevice())
    return;

  for (auto& device_str : g_controller_interface.GetAllDeviceStrings())
  {
    ciface::Core::DeviceQualifier dq;
    dq.FromString(device_str);

    auto device = g_controller_interface.FindDevice(dq);
    if (device)
    {
      m_controllers.push_back(std::move(device));
    }
  }
}

void ImGuiFrontend::RefreshControls(bool updateGameSelection)
{
  if (m_controllers.empty())
    PopulateControls();

  bool input_handled = false;
  for (auto device : m_controllers)
  {
    if (input_handled || device == nullptr || !device->IsValid())
      return;

    device->UpdateInput();

    // wrap around if exceeding the max games or going below
    if (updateGameSelection)
    {
      auto now = std::chrono::high_resolution_clock::now();
      long timeSinceLastInput =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - m_scroll_last).count();
      if (TryInput("Pad W", device))
      {
        if (!m_direction_pressed)
        {
          m_selectedGameIdx = m_selectedGameIdx <= 0 ?
                                  static_cast<int>(m_displayed_games.size()) - 1 :
                                  m_selectedGameIdx - 1;
          m_direction_pressed = true;
          break;
        }

        input_handled = true;
      }
      else if (TryInput("Pad E", device))
      {
        if (!m_direction_pressed)
        {
          m_selectedGameIdx = m_selectedGameIdx >= static_cast<int>(m_displayed_games.size()) - 1 ? 0 : m_selectedGameIdx + 1;
          m_direction_pressed = true;
          break;
        }

        input_handled = true;
      }
      else if (TryInput("Left X-", device))
      {
        if (timeSinceLastInput > 200L)
        {
          m_selectedGameIdx = m_selectedGameIdx <= 0 ?
                                  static_cast<int>(m_displayed_games.size()) - 1 :
                                  m_selectedGameIdx - 1;
          m_scroll_last = std::chrono::high_resolution_clock::now();
          break;
        }

        input_handled = true;
      }
      else if (TryInput("Left X+", device))
      {
        if (timeSinceLastInput > 200L)
        {
          m_selectedGameIdx = m_selectedGameIdx >= static_cast<int>(m_displayed_games.size()) - 1 ?
                                  0 :
                                  m_selectedGameIdx + 1;
          m_scroll_last = std::chrono::high_resolution_clock::now();
          break;
        }

        input_handled = true;
      }
      else if (TryInput("Bumper L", device))
      {
        if (timeSinceLastInput > 250L)
        {
          if (!m_state.showListView && !m_state.showSettingsWindow && g_netplay_dialog == nullptr)
          {
            int idx = -1 + (int) m_ccat;
            if (idx < 1)
              idx = -1 + (int) CCount;

            m_ccat = (CarouselCategory) idx;
          }

          m_scroll_last = std::chrono::high_resolution_clock::now();
          break;
        }

        input_handled = true;
      }
      else if (TryInput("Bumper R", device))
      {
        if (timeSinceLastInput > 250L)
        {
          if (!m_state.showListView && !m_state.showSettingsWindow && g_netplay_dialog == nullptr)
          {
            int idx = 1 + (int) m_ccat;
            if (idx >= CCount)
              idx = 1;

            m_ccat = (CarouselCategory) idx;
          }

          m_scroll_last = std::chrono::high_resolution_clock::now();
          break;
        }

        input_handled = true;
      }
      else if (m_displayed_games.size() > 10 && TryInput("Trigger L", device))
      {
        if (timeSinceLastInput > 500L)
        {
          int i = m_selectedGameIdx - 10;
          if (i < 0)
          {
            // wrap around, total games + -index
            m_selectedGameIdx = static_cast<int>(m_displayed_games.size()) + i;
          }
          else
          {
            m_selectedGameIdx = i;
          }

          m_scroll_last = std::chrono::high_resolution_clock::now();
          break;
        }

        input_handled = true;
      }
      else if (m_displayed_games.size() > 10 && TryInput("Trigger R", device))
      {
        if (timeSinceLastInput > 500L)
        {
          int i = m_selectedGameIdx + 10;
          if (i >= m_displayed_games.size())
          {
            // wrap around, i - total games
            m_selectedGameIdx = i - static_cast<int>(m_displayed_games.size());
          }
          else
          {
            m_selectedGameIdx = i;
          }

          m_scroll_last = std::chrono::high_resolution_clock::now();
          break;
        }

        input_handled = true;
      }
      else
      {
        m_direction_pressed = false;
      }
    }
  }
}

FrontendResult ImGuiFrontend::RunUntilSelection()
{
  return RunMainLoop();
}

FrontendResult ImGuiFrontend::RunMainLoop()
{
  FrontendResult selection;

  if (g_netplay_dialog)
  {
    // Reset for if we're exiting a game into netplay again
    g_netplay_dialog->Reset();
  }

  // Main loop
  bool done = false;
  while (!done)
  {
    CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
        winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);

    for (auto device : m_controllers)
    {
      if (device && device->IsValid() && !m_state.controlsDisabled)
      {
        if (TryInput("Menu", device))
        {
          if (!m_state.menuPressed && !m_state.showListView && !g_netplay_dialog)
          {
            m_state.showSettingsWindow = !m_state.showSettingsWindow;
            m_state.menuPressed = true;
            LoadGameList();
          }

          break;
        }
        else if (TryInput("Button X", device))
        {
          if (!m_state.menuPressed && !m_state.showSettingsWindow && !g_netplay_dialog)
          {
            m_state.showListView = !m_state.showListView;
            m_state.menuPressed = true;
          }

          break;
        }
        else if (TryInput("View", device))
        {
          if (!m_state.menuPressed && !m_state.showListView && !m_state.showSettingsWindow)
          {
            if (g_netplay_dialog)
            {
              g_netplay_client = nullptr;
              g_netplay_server = nullptr;
              g_netplay_dialog = nullptr;
            }
            else
            {
              g_netplay_dialog = std::make_shared<ImGuiNetPlay>(this, m_games, m_frame_scale);
            }

            m_state.menuPressed = true;
          }

          break;
        }
        else
        {
          m_state.menuPressed = false;
        }
      }
    }

    ImGuiIO& io = ImGui::GetIO();
    io.KeysDown[0x08] = false;

    {
      std::unique_lock lk(UWP::g_buffer_mutex);
      for (uint32_t c : UWP::g_char_buffer)
      {
        io.AddInputCharacter(c);

        if (c == '\b')
        {
          io.KeysDown[0x08] = true;
        }
      }
      UWP::g_char_buffer.clear();
    }

    // -- Draw Background first
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::SetNextWindowPos(ImVec2(0, 0));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    if (ImGui::Begin("Background", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav))
    {
      ImGui::Image(m_selected_theme->GetBackground(m_state.currentBG).get(),
                   ImGui::GetIO().DisplaySize);

      if (m_state.showListView)
      {
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::Image(m_selected_theme->GetBackground(BG_List_UI).get(), ImGui::GetIO().DisplaySize);
      }
      else if (!m_state.showSettingsWindow && g_netplay_dialog == nullptr)
      {
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::Image(m_selected_theme->GetBackground(BG_Carousel_UI).get(),
                     ImGui::GetIO().DisplaySize);
      }

      ImGui::End();
    }
    ImGui::PopStyleVar(3);
    // -- Background

    if (g_netplay_dialog != nullptr)
    {
      m_state.currentBG = BG_Netplay;

      auto result = g_netplay_dialog->Draw();
      if (result == BootGame)
      {
        selection.netplay = true;
        break;
      }
      else if (result == ExitNetplay)
      {
        g_netplay_dialog = nullptr;
      }
    }
    else if (m_state.showSettingsWindow)
    {
      m_state.currentBG = BG_Menu;

      ImGui::SetNextWindowSize(ImVec2(540 * m_frame_scale, 425 * m_frame_scale));
      ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - (540 / 2) * m_frame_scale,
                                     ImGui::GetIO().DisplaySize.y / 2 - (425 / 2) * m_frame_scale));
      if (ImGui::Begin("Settings", nullptr,
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize))
      {
        DrawSettingsMenu(&m_state, m_frame_scale);
        ImGui::End();
      }
    }
    else if (m_state.showListView)
    {
      m_state.currentBG = BG_List;

      selection = CreateListPage();
      if (selection.game_result != nullptr || selection.netplay)
      {
        break;
      }
    }
    else
    {
      m_state.currentBG = static_cast<ThemeBG>(m_ccat);

      selection = CreateMainPage();
      if (selection.game_result != nullptr)
      {
        Config::SetBase(Config::FRONTEND_LAST_GAME, (int) m_selectedGameIdx);
        Config::SetBase(Config::FRONTEND_LAST_CATEGORY, (int) m_ccat);
        Config::Save();

        break;
      }
      else if (selection.netplay)
      {
        break;
      }
    }

    if (!m_state.controlsDisabled)
      RefreshControls(!m_state.showSettingsWindow);

    g_presenter->Present();
  }

  ImGui::SetNextWindowSize(ImVec2(100 * m_frame_scale, 50 * m_frame_scale));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - (100 / 2) * m_frame_scale,
                                 ImGui::GetIO().DisplaySize.y / 2 - (50 / 2) * m_frame_scale));
  if (ImGui::Begin("Loading..", nullptr,
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Loading..");
    ImGui::End();
  }

  g_presenter->Present();

  return selection;
}

void CreateGeneralTab(UIState* state)
{
  bool dualCore = Config::Get(Config::MAIN_CPU_THREAD);
  if (ImGui::Checkbox("Dual Core", &dualCore))
  {
    Config::SetBaseOrCurrent(Config::MAIN_CPU_THREAD, dualCore);
    Config::Save();
  }

  bool cheats = Config::Get(Config::MAIN_ENABLE_CHEATS);
  if (ImGui::Checkbox("Enable Cheats", &cheats))
  {
    Config::SetBaseOrCurrent(Config::MAIN_ENABLE_CHEATS, cheats);
    Config::Save();
  }

  bool mismatchedRegion = Config::Get(Config::MAIN_OVERRIDE_REGION_SETTINGS);
  if (ImGui::Checkbox("Allow Mismatched Region Settings", &mismatchedRegion))
  {
    Config::SetBaseOrCurrent(Config::MAIN_OVERRIDE_REGION_SETTINGS, mismatchedRegion);
    Config::Save();
  }

  bool changeDiscs = Config::Get(Config::MAIN_AUTO_DISC_CHANGE);
  if (ImGui::Checkbox("Change Discs Automatically", &changeDiscs))
  {
      Config::SetBaseOrCurrent(Config::MAIN_AUTO_DISC_CHANGE, changeDiscs);
      Config::Save();
  }

  const auto fallback = Config::Get(Config::MAIN_FALLBACK_REGION);
  if (ImGui::TreeNode("Fallback Region"))
  {
    if (ImGui::RadioButton("NTSC JP", fallback == DiscIO::Region::NTSC_J))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_J);
      Config::Save();
    }

    if (ImGui::RadioButton("NTSC NA", fallback == DiscIO::Region::NTSC_U))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_U);
      Config::Save();
    }

    if (ImGui::RadioButton("PAL", fallback == DiscIO::Region::PAL))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::PAL);
      Config::Save();
    }

    if (ImGui::RadioButton("Unknown", fallback == DiscIO::Region::Unknown))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::Unknown);
      Config::Save();
    }

    if (ImGui::RadioButton("NTSC Korea", fallback == DiscIO::Region::NTSC_K))
    {
      Config::SetBaseOrCurrent(Config::MAIN_FALLBACK_REGION, DiscIO::Region::NTSC_K);
      Config::Save();
    }

    ImGui::TreePop();
  }
}

void CreateInterfaceTab(UIState* state)
{
  bool showFps = Config::Get(Config::GFX_SHOW_FPS);
  if (ImGui::Checkbox("Show FPS", &showFps))
  {
    Config::SetBaseOrCurrent(Config::GFX_SHOW_FPS, showFps);
    Config::Save();
  }

  bool showOSD = Config::Get(Config::MAIN_OSD_MESSAGES);
  if (ImGui::Checkbox("Show On-Screen Messages", &showOSD))
  {
    Config::SetBaseOrCurrent(Config::MAIN_OSD_MESSAGES, showOSD);
    Config::Save();
  }

  bool showStats = Config::Get(Config::GFX_OVERLAY_STATS);
  if (ImGui::Checkbox("Show Rendering Statistics", &showStats))
  {
    Config::SetBaseOrCurrent(Config::GFX_OVERLAY_STATS, showStats);
    Config::Save();
  }

  ImGui::Spacing();

  if (ImGui::BeginCombo("Theme", m_selected_theme->GetName().c_str()))
  {
    for (auto& theme : m_themes)
    {
      if (ImGui::Selectable(theme.first.c_str(),
                            m_selected_theme->GetName() == theme.second.GetName()))
      {
        m_selected_theme = &theme.second;
        Config::SetBaseOrCurrent(Config::FRONTEND_SELECTED_THEME, theme.first);
      }
    }

    ImGui::EndCombo();
  }
}


void CreateGraphicsTab(UIState* state)
{
  bool vSync = Config::Get(Config::GFX_VSYNC);
  if (ImGui::Checkbox("V-Sync", &vSync))
  {
    Config::SetBaseOrCurrent(Config::GFX_VSYNC, vSync);
    Config::Save();
  }

  bool scaledEfb = Config::Get(Config::GFX_HACK_COPY_EFB_SCALED);
  if (ImGui::Checkbox("Scaled EFB Copy", &scaledEfb))
  {
    Config::SetBaseOrCurrent(Config::GFX_HACK_COPY_EFB_SCALED, scaledEfb);
    Config::Save();
  }

  bool disableFog = Config::Get(Config::GFX_DISABLE_FOG);
  if (ImGui::Checkbox("Disable Fog", &disableFog))
  {
    Config::SetBaseOrCurrent(Config::GFX_DISABLE_FOG, disableFog);
    Config::Save();
  }

  bool perPixelLighting = Config::Get(Config::GFX_ENABLE_PIXEL_LIGHTING);
  if (ImGui::Checkbox("Per-Pixel Lighting", &perPixelLighting))
  {
    Config::SetBaseOrCurrent(Config::GFX_ENABLE_PIXEL_LIGHTING, perPixelLighting);
    Config::Save();
  }

  bool disableCopyFilter = Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  if (ImGui::Checkbox("Disable Copy Filter", &disableCopyFilter))
  {
    Config::SetBaseOrCurrent(Config::GFX_ENHANCE_DISABLE_COPY_FILTER, disableCopyFilter);
    Config::Save();
  }

  const char* ir_items[] = {"Auto (Multiple of 640x528)",      "Native (640x528)",
                            "2x Native (1280x1056) for 720p",  "3x Native (1920x1584) for 1080p",
                            "4x Native (2560x2112) for 1440p", "5x Native (3200x2640)",
                            "6x Native (3840x3168) for 4K",    "7x Native (4480x3696)",
                            "8x Native (5120x4224) for 5K"};

  int ir_idx = Config::Get(Config::GFX_EFB_SCALE);

  if (ImGui::TreeNode("Internal Resolution"))
  {
    for (int i = 0; i < 9; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(ir_items[i], i == ir_idx))
      {
        Config::SetBase(Config::GFX_EFB_SCALE, i);
        Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }

  const char* aspect_items[] = {"Auto", "Force 16:9", "Force 4:3", "Stretch"};
  int aspect_idx = 0;
  auto aspect = Config::Get(Config::GFX_ASPECT_RATIO);
  switch (aspect)
  {
  case AspectMode::Auto:
    aspect_idx = 0;
    break;
  case AspectMode::AnalogWide:
    aspect_idx = 1;
    break;
  case AspectMode::Analog:
    aspect_idx = 2;
    break;
  case AspectMode::Stretch:
    aspect_idx = 3;
    break;
  }

  if (ImGui::TreeNode("Aspect Ratio"))
  {
    for (int i = 0; i < 4; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(aspect_items[i], i == aspect_idx))
      {
        switch (i)
        {
        case 0:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Auto);
            break;
        case 1:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::AnalogWide);
            break;
        case 2:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Analog);
            break;
        case 3:
            Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, AspectMode::Stretch);
            break;
        }

        Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }

  const char* shader_items[] = {"Synchronous", "Hybrid Ubershaders", "Exclusive Ubershaders",
                                "Skip Drawing"};
  int shader_idx = 0;
  auto shader = Config::Get(Config::GFX_SHADER_COMPILATION_MODE);
  switch (shader)
  {
  case ShaderCompilationMode::Synchronous:
    shader_idx = 0;
    break;
  case ShaderCompilationMode::AsynchronousUberShaders:
    shader_idx = 1;
    break;
  case ShaderCompilationMode::SynchronousUberShaders:
    shader_idx = 2;
    break;
  case ShaderCompilationMode::AsynchronousSkipRendering:
    shader_idx = 3;
    break;
  }

  if (ImGui::TreeNode("Shader Compilation"))
  {
    for (int i = 0; i < 4; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(shader_items[i], i == shader_idx))
      {
        switch (i)
        {
        case 0:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE,
                                     ShaderCompilationMode::Synchronous);
            break;
        case 1:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE,
                                     ShaderCompilationMode::AsynchronousUberShaders);
            break;
        case 2:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE,
                                     ShaderCompilationMode::SynchronousUberShaders);
            break;
        case 3:
            Config::SetBaseOrCurrent(Config::GFX_SHADER_COMPILATION_MODE,
                                     ShaderCompilationMode::AsynchronousSkipRendering);
            break;
        }

        Config::Save();
      }
      ImGui::PopID();
    }

    bool waitForCompile = Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);
    if (ImGui::Checkbox("Compile Shaders Before Starting", &waitForCompile))
    {
      Config::SetBaseOrCurrent(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, waitForCompile);
      Config::Save();
    }

    ImGui::TreePop();
  }

  const char* aalevel_items[] = {"None", "2x", "4x", "8x"};
  auto msaa = Config::Get(Config::GFX_MSAA);
  bool ssaa = Config::Get(Config::GFX_SSAA);

  if (ImGui::TreeNode("Anti-Aliasing"))
  {
    if (ImGui::RadioButton("MSAA", !ssaa))
    {
      Config::SetBaseOrCurrent(Config::GFX_SSAA, false);
      Config::Save();
    }

    if (ImGui::RadioButton("SSAA", ssaa))
    {
      Config::SetBaseOrCurrent(Config::GFX_SSAA, true);
      Config::Save();
    }

    ImGui::Separator();
    for (u32 i = 0; i < 4; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(aalevel_items[i], i == msaa))
      {
        Config::SetBaseOrCurrent(Config::GFX_MSAA, i);
        Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }

  const char* anisolevel_items[] = {"1x", "2x", "4x", "8x", "16x"};
  auto aniso = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);

  if (ImGui::TreeNode("Anisotropic Filtering"))
  {
    for (int i = 0; i < 5; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(anisolevel_items[i], i == aniso))
      {
        Config::SetBaseOrCurrent(Config::GFX_ENHANCE_MAX_ANISOTROPY, i);
        Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }
}

void CreateControlsTab(UIState* state)
{
  auto devices = g_controller_interface.GetAllDeviceStrings();

  if (ImGui::BeginTabBar("controlsbar"))
  {
    if (ImGui::BeginTabItem("GameCube"))
    {
      for (int i = 0; i < 4; i++)
      {
        CreateGCPort(i, devices);
      }
      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Wii"))
    {
      for (int i = 0; i < 4; i++)
      {
        CreateWiiPort(i, devices);
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
}

void CreateGameCubeTab(UIState* state)
{
  const char* language_items[] = {"English", "German", "French", "Spanish", "Italian", "Dutch"};
  auto lang = Config::Get(Config::MAIN_GC_LANGUAGE);

  if (ImGui::TreeNode("System Language"))
  {
    for (int i = 0; i < 6; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(language_items[i], i == lang))
      {
          Config::SetBaseOrCurrent(Config::MAIN_GC_LANGUAGE, i);
          Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }

  auto slot1 = Config::Get(Config::MAIN_SLOT_A);
  if (ImGui::TreeNode("Slot A"))
  {
    if (ImGui::RadioButton("<Nothing>", slot1 == ExpansionInterface::EXIDeviceType::None))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_A, ExpansionInterface::EXIDeviceType::None);
      Config::Save();
    }

    if (ImGui::RadioButton("Dummy", slot1 == ExpansionInterface::EXIDeviceType::Dummy))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_A,
                               ExpansionInterface::EXIDeviceType::Dummy);
      Config::Save();
    }

    if (ImGui::RadioButton("Memory Card", slot1 == ExpansionInterface::EXIDeviceType::MemoryCard))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_A,
                               ExpansionInterface::EXIDeviceType::MemoryCard);
      Config::Save();
    }

    if (ImGui::RadioButton("GCI Folder",
                           slot1 == ExpansionInterface::EXIDeviceType::MemoryCardFolder))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_A,
                               ExpansionInterface::EXIDeviceType::MemoryCardFolder);
      Config::Save();
    }

    if (ImGui::RadioButton("USB Gecko", slot1 == ExpansionInterface::EXIDeviceType::Gecko))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_A,
                               ExpansionInterface::EXIDeviceType::Gecko);
      Config::Save();
    }

    if (ImGui::RadioButton("Advance Game Port", slot1 == ExpansionInterface::EXIDeviceType::AGP))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_A, ExpansionInterface::EXIDeviceType::AGP);
      Config::Save();
    }

    ImGui::TreePop();
  }

  // Todo: This really shouldn't be copy+pasted and could be cleaned up.
  auto slot2 = Config::Get(Config::MAIN_SLOT_B);
  if (ImGui::TreeNode("Slot B"))
  {
    if (ImGui::RadioButton("<Nothing>", slot2 == ExpansionInterface::EXIDeviceType::None))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::None);
      Config::Save();
    }

    if (ImGui::RadioButton("Dummy", slot2 == ExpansionInterface::EXIDeviceType::Dummy))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::Dummy);
      Config::Save();
    }

    if (ImGui::RadioButton("Memory Card", slot2 == ExpansionInterface::EXIDeviceType::MemoryCard))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::MemoryCard);
      Config::Save();
    }

    if (ImGui::RadioButton("GCI Folder",
                           slot2 == ExpansionInterface::EXIDeviceType::MemoryCardFolder))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_B,
                               ExpansionInterface::EXIDeviceType::MemoryCardFolder);
      Config::Save();
    }

    if (ImGui::RadioButton("USB Gecko", slot2 == ExpansionInterface::EXIDeviceType::Gecko))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::Gecko);
      Config::Save();
    }

    if (ImGui::RadioButton("Advance Game Port", slot2 == ExpansionInterface::EXIDeviceType::AGP))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::AGP);
      Config::Save();
    }

    ImGui::TreePop();
  }

  auto sp1 = Config::Get(Config::MAIN_SERIAL_PORT_1);
  if (ImGui::TreeNode("SP1"))
  {
    if (ImGui::RadioButton("<Nothing>", sp1 == ExpansionInterface::EXIDeviceType::None))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::None);
      Config::Save();
    }

    if (ImGui::RadioButton("Dummy", sp1 == ExpansionInterface::EXIDeviceType::Dummy))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::Dummy);
      Config::Save();
    }

    if (ImGui::RadioButton("Broadband Adapter (TAP)", sp1 == ExpansionInterface::EXIDeviceType::Ethernet))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SERIAL_PORT_1,
                               ExpansionInterface::EXIDeviceType::Ethernet);
      Config::Save();
    }

    if (ImGui::RadioButton("Broadband Adapter (XLink Kai)",
                           sp1 == ExpansionInterface::EXIDeviceType::EthernetXLink))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SERIAL_PORT_1,
                               ExpansionInterface::EXIDeviceType::EthernetXLink);
      Config::Save();
    }

    if (ImGui::RadioButton("Broadband Adapter (HLE)",
                           sp1 == ExpansionInterface::EXIDeviceType::EthernetBuiltIn))
    {
      Config::SetBaseOrCurrent(Config::MAIN_SERIAL_PORT_1,
                               ExpansionInterface::EXIDeviceType::EthernetBuiltIn);
      Config::Save();
    }

    ImGui::TreePop();
  }
}

void CreateWiiTab(UIState* state)
{
  bool pal60 = Config::Get(Config::SYSCONF_PAL60);
  if (ImGui::Checkbox("Enable PAL60", &pal60))
  {
    Config::SetBaseOrCurrent(Config::SYSCONF_PAL60, pal60);
    Config::Save();
  }

  const char* language_items[] = {"Japanese", "English", "German", "French",
                                  "Spanish",  "Italian", "Dutch",  "Simplified Chinese",
                                  "Traditional Chinese",
                                  "Korean"};
  auto lang = Config::Get(Config::SYSCONF_LANGUAGE);

  if (ImGui::TreeNode("System Language"))
  {
    for (u32 i = 0; i < 10; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(language_items[i], i == lang))
      {
          Config::SetBaseOrCurrent(Config::SYSCONF_LANGUAGE, i);
          Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }

  const char* sound_items[] = {"Mono", "Stereo", "Surround"};
  auto sound = Config::Get(Config::SYSCONF_SOUND_MODE);

  if (ImGui::TreeNode("Sound"))
  {
    for (u32 i = 0; i < 3; i++)
    {
      ImGui::PushID(i);
      if (ImGui::RadioButton(sound_items[i], i == sound))
      {
          Config::SetBaseOrCurrent(Config::SYSCONF_SOUND_MODE, i);
          Config::Save();
      }
      ImGui::PopID();
    }

    ImGui::TreePop();
  }
}

void CreateAdvancedTab(UIState* state)
{
  bool viSkipEnable = Config::Get(Config::GFX_HACK_VI_SKIP);
  if (ImGui::Checkbox("Enable VI Skip", &viSkipEnable))
  {
    Config::SetBaseOrCurrent(Config::GFX_HACK_VI_SKIP, viSkipEnable);
    Config::Save();
  }

  bool hiresTexEnable = Config::Get(Config::GFX_HIRES_TEXTURES);
  if (ImGui::Checkbox("Load Custom Textures", &hiresTexEnable))
  {
    Config::SetBaseOrCurrent(Config::GFX_HIRES_TEXTURES, hiresTexEnable);
    Config::Save();
  }

  bool prefetchTexEnable = Config::Get(Config::GFX_CACHE_HIRES_TEXTURES);
  if (ImGui::Checkbox("Prefetch Custom Textures", &prefetchTexEnable))
  {
    Config::SetBaseOrCurrent(Config::GFX_CACHE_HIRES_TEXTURES, prefetchTexEnable);
    Config::Save();
  }

  bool graphicsModsEnable = Config::Get(Config::GFX_MODS_ENABLE);
  if (ImGui::Checkbox("Enable Graphics Mods", &graphicsModsEnable))
  {
    Config::SetBaseOrCurrent(Config::GFX_MODS_ENABLE,
                                   graphicsModsEnable);
    Config::Save();
  }

  bool textureDumping = Config::Get(Config::GFX_DUMP_TEXTURES);
  if (ImGui::Checkbox("Enable Texture Dumping", &textureDumping))
  {
    Config::SetBaseOrCurrent(Config::GFX_DUMP_TEXTURES, textureDumping);
    Config::Save();
  }

  auto textureCache = Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  if (ImGui::TreeNode("Texture Cache Accuracy"))
  {
    if (ImGui::RadioButton("Safe", textureCache == 0))
    {
      Config::SetBaseOrCurrent(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, 0);
      Config::Save();
    }

    if (ImGui::RadioButton("Balanced", textureCache == 512))
    {
      Config::SetBaseOrCurrent(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, 512);
      Config::Save();
    }

    if (ImGui::RadioButton("Fast", textureCache == 128))
    {
      Config::SetBaseOrCurrent(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, 128);
      Config::Save();
    }

    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Clock Override"))
  {
    ImGui::Text("WARNING: Changing this from the default (1.0 = 100%) can and will break\ngames and cause glitches. Do so at your own risk. \nPlease do not report bugs that occur with a non-default clock. \nThis is not a magical performance slider!");

    bool overclockEnable = Config::Get(Config::MAIN_OVERCLOCK_ENABLE);
    if (ImGui::Checkbox("Enable Emulated CPU Clock Override", &overclockEnable))
    {
      Config::SetBaseOrCurrent(Config::MAIN_OVERCLOCK_ENABLE, overclockEnable);
      Config::Save();
    }

    float clockOverride = Config::Get(Config::MAIN_OVERCLOCK);
    if (ImGui::SliderFloat("Emulated CPU Clock Speed Override", &clockOverride, 0.06f, 4.0f))
    {
      Config::SetBaseOrCurrent(Config::MAIN_OVERCLOCK, clockOverride);
      Config::Save();
    }

    ImGui::TreePop();
  }
}

void CreatePathsTab(UIState* state)
{
  ImGui::Text("Game Folders");
  if (ImGui::BeginListBox("##folders")) {
    for (auto path : m_paths)
    {
      if (ImGui::Selectable(path.c_str()))
      {
        state->selectedPath = path;
      }
    }
    ImGui::EndListBox();
  }

  if (state->selectedPath == "")
  {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button("Remove Path"))
  {
    if (state->selectedPath != "")
    {
      m_paths.erase(std::remove(m_paths.begin(), m_paths.end(), state->selectedPath),
                    m_paths.end());
      Config::SetIsoPaths(m_paths);
      Config::Save();
    }
  }

  if (state->selectedPath == "")
  {
    ImGui::EndDisabled();
  }

  ImGui::SameLine();
  if (ImGui::Button("Add Path"))
  {
    state->controlsDisabled = true;
    UWP::OpenGameFolderPicker([state](std::string path) {
      if (path != "")
      {
        if (!UWP::TestPathPermissions(path))
        {
          m_show_path_warning = true;
        }
        else
        {
          m_paths.emplace_back(path);
          Config::SetIsoPaths(m_paths);
          Config::Save();
        }
      }

      state->controlsDisabled = false;
    });
  }

  ImGui::Spacing();
  ImGui::Separator();

  if (ImGui::Button("Set Dolphin User Folder Location"))
  {
    state->controlsDisabled = true;
    UWP::OpenNewUserPicker([=](std::string path) {
      if (path != "")
      {
        if (!UWP::TestPathPermissions(path))
        {
          m_show_path_warning = true;
        }
      }

      state->controlsDisabled = false;
    });

    // Reset everything and load the new config location.
    UICommon::Shutdown();
    UICommon::SetUserDirectory(UWP::GetUserLocation());
    UICommon::CreateDirectories();
    UICommon::Init();
  }

  if (ImGui::Button("Reset Dolphin User Folder Location"))
  {
    UWP::ResetUserLocation();

    // Reset everything and load the new config location.
    UICommon::Shutdown();
    UICommon::SetUserDirectory(UWP::GetUserLocation());
    UICommon::CreateDirectories();
    UICommon::Init();
  }

  if (m_show_path_warning)
  {
    ImGui::OpenPopup("Warning");
    m_show_path_warning = false;
    ImGui::SetNextWindowSize(ImVec2(950 * m_frame_scale, 80 * m_frame_scale));
  }

  if (ImGui::BeginPopupModal("Warning"))
  {
    ImGui::TextWrapped("The folder path you have selected is not writable! Please check that you "
                       "have the correct permissions for the folder you have selected.");
    ImGui::Separator();
    if (ImGui::Button("OK"))
    {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextWrapped("Note: Please remember to do your USB filesystem setup, or paths to "
                     "your USB will not work properly!");
}

void CreateWiiPort(int index, std::vector<std::string> devices)
{
  if (ImGui::BeginChild(std::format("gc-wii-{}", index).c_str(), ImVec2(-1, 75 * m_frame_scale),
                        true))
  {
    auto controller = Wiimote::GetConfig()->GetController(index);
    auto default_device = controller->GetDefaultDevice().name;

    ImGui::Text("Wiimote Port %d", index + 1);

    if (ImGui::BeginCombo("Device", default_device.c_str()))
    {
      for (auto device : devices)
      {
        if (ImGui::Selectable(device.c_str(), strcmp(default_device.c_str(), device.c_str()) == 0))
        {
            controller->SetDefaultDevice(device);
            controller->UpdateReferences(g_controller_interface);
            Wiimote::GetConfig()->SaveConfig();
        }
      }

      ImGui::EndCombo();
    }

    if (ImGui::BeginCombo("Profile", m_selected_wiimote_profile[index].c_str()))
    {
      for (auto profile : m_wiimote_profiles)
      {
        if (ImGui::Selectable(profile.c_str(), m_selected_wiimote_profile[index] == profile))
        {
            m_selected_wiimote_profile[index] = profile;

            if (m_selected_wiimote_profile[index] == "None")
            {
              // Loading an empty inifile section clears everything.
              Common::IniFile::Section sec;

              controller->LoadConfig(&sec);
              controller->SetDefaultDevice(default_device);
              Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index), WiimoteSource::None);
            }
            else if (m_selected_wiimote_profile[index] == "Wiimote + Nunchuk")
            {
              controller->LoadDefaults(g_controller_interface);

              Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index), WiimoteSource::Emulated);
            }
            else if (m_selected_wiimote_profile[index] == "Classic Controller")
            { 
              Common::IniFile ini;
              ini.Load(File::GetSysDirectory() + PROFILES_DIR +
                       Wiimote::GetConfig()->GetProfileName() + "/Classic.ini");

              controller->LoadConfig(ini.GetOrCreateSection("Profile"));
              Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                       WiimoteSource::Emulated);
            }
            else if (m_selected_wiimote_profile[index] == "Sideways Wiimote")
            { 
              Common::IniFile ini;
              ini.Load(File::GetSysDirectory() + PROFILES_DIR +
                       Wiimote::GetConfig()->GetProfileName() + "/Sideways.ini");

              controller->LoadConfig(ini.GetOrCreateSection("Profile"));
              Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                       WiimoteSource::Emulated);
            }
            else
            {
              Common::IniFile ini;
              ini.Load(File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR +
                       Wiimote::GetConfig()->GetProfileName() + "/" + profile + ".ini");

              controller->LoadConfig(ini.GetOrCreateSection("Profile"));
              Config::SetBaseOrCurrent(Config::GetInfoForWiimoteSource(index),
                                       WiimoteSource::Emulated);
            }

            controller->UpdateReferences(g_controller_interface);
            Wiimote::GetConfig()->SaveConfig();
            Config::Save();
        }
      }

      ImGui::EndCombo();
    }
  }

  ImGui::EndChild();
}

void CreateGCPort(int index, std::vector<std::string> devices)
{
  if (ImGui::BeginChild(std::format("gc-port-{}", index).c_str(), ImVec2(-1, 75 * m_frame_scale), true))
  {
    auto controller = Pad::GetConfig()->GetController(index);
    auto default_device = controller->GetDefaultDevice().name;

    ImGui::Text("GameCube Port %d", index + 1);

    if (ImGui::BeginCombo("Device", default_device.c_str()))
    {
      for (auto device : devices)
      {
        if (ImGui::Selectable(device.c_str(), strcmp(default_device.c_str(), device.c_str()) == 0))
        {
            controller->SetDefaultDevice(device);
            controller->UpdateReferences(g_controller_interface);
            Pad::GetConfig()->SaveConfig();
        }
      }

      ImGui::EndCombo();
    }

    if (ImGui::BeginCombo("Profile", m_selected_gc_profile[index].c_str()))
    {
      for (auto profile : m_gc_profiles)
      {
        if (ImGui::Selectable(profile.c_str(), m_selected_gc_profile[index] == profile))
        {
            m_selected_gc_profile[index] = profile;

            if (m_selected_gc_profile[index] == "None")
            {
              // Loading an empty inifile section clears everything.
              Common::IniFile::Section sec;
              controller->LoadConfig(&sec);
              controller->SetDefaultDevice(default_device);
              Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(index),
                                       SerialInterface::SIDevices::SIDEVICE_NONE);
            }
            else if (m_selected_gc_profile[index] == "Default")
            {
              controller->LoadDefaults(g_controller_interface);
              Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(index),
                                       SerialInterface::SIDevices::SIDEVICE_GC_CONTROLLER);
            }
            else
            {
              Common::IniFile ini;
              ini.Load(File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR +
                       Pad::GetConfig()->GetProfileName() + "/" + profile + ".ini");

              controller->LoadConfig(ini.GetOrCreateSection("Profile"));
              Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(index),
                                       SerialInterface::SIDevices::SIDEVICE_GC_CONTROLLER);
            }

            controller->UpdateReferences(g_controller_interface);
            Pad::GetConfig()->SaveConfig();
            Config::Save();
        }
      }

      ImGui::EndCombo();
    }
  }

  ImGui::EndChild();
}

FrontendResult ImGuiFrontend::CreateMainPage()
{
  //float selOffset = m_selectedGameIdx >= 5 ? 160.0f * (m_selectedGameIdx - 4) * -1.0f : 0;
  float posX = 30 * m_frame_scale;
  float posY = (345.0f / 2) * m_frame_scale;
  auto extraFlags = m_games.size() < 5 ? ImGuiWindowFlags_None :
                                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;

  ImGui::SetNextWindowPos(ImVec2(posX, posY));
  if (ImGui::Begin("Dolphin Emulator", nullptr, ImGuiWindowFlags_NoTitleBar |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground |
                       extraFlags))
  {
    auto game = CreateGameCarousel();
    ImGui::End();
    if (game != nullptr)
    {
      return FrontendResult(game);
    }
  }

  const u64 current_time_us = Common::Timer::NowUs();
  const u64 time_diff_us = current_time_us -m_imgui_last_frame_time;
  const float time_diff_secs = static_cast<float>(time_diff_us / 1000000.0);
  m_imgui_last_frame_time = current_time_us;

  // Update I/O with window dimensions.
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = time_diff_secs;

  return FrontendResult(); // keep running
}

FrontendResult ImGuiFrontend::CreateListPage()
{
  ImGui::SetNextWindowSize(ImVec2(540 * m_frame_scale, 425 * m_frame_scale));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - (540 / 2) * m_frame_scale,
                                 ImGui::GetIO().DisplaySize.y / 2 - (425 / 2) * m_frame_scale));

  if (ImGui::Begin("Dolphin Emulator", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings))
  {
    auto game = CreateGameList();
    ImGui::End();
    if (game != nullptr)
    {
      return FrontendResult(game);
    }
  }

  return FrontendResult();
}

std::shared_ptr<UICommon::GameFile> ImGuiFrontend::CreateGameList()
{
  if (ImGui::Button("Search Game"))
  {
    UWP::ShowKeyboard();
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SameLine();

  ImGui::PushItemWidth(-1);
  ImGui::InputText("##gamesearch", m_list_search_buf, 32);
  ImGui::PopItemWidth();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (ImGui::BeginListBox("##Games List", ImVec2(-1, -1)))
  {
    size_t search = strlen(m_list_search_buf);
    std::vector<std::shared_ptr<UICommon::GameFile>> games;
    if (search > 0)
    {
      std::string search_phrase = std::string(m_list_search_buf);
      if (search_phrase != m_prev_list_search)
      {
        m_list_search_results.clear();
        for (auto& game : m_games)
        {
            auto& name = game->GetName(m_title_database);
            auto it = std::search(name.begin(), name.end(), search_phrase.begin(),
                                  search_phrase.end(), [](unsigned char ch1, unsigned char ch2) {
                                    return std::toupper(ch1) == std::toupper(ch2);
                                  });

            if (it != name.end())
            {
              m_list_search_results.push_back(game);
            }
        }

        m_prev_list_search = m_list_search_buf;
      }

      games = m_list_search_results;
    }
    else
    {
      games = m_games;
    }

    for (auto& game : games)
    {
      if (ImGui::Selectable(std::format("{}##{}", game->GetName(m_title_database).c_str(),
                                                  game->GetFilePath()).c_str()))
      {
        ImGui::EndListBox();
        return game;
      }
    }

    ImGui::EndListBox();
  }

  return nullptr;
}

std::shared_ptr<UICommon::GameFile> ImGuiFrontend::CreateGameCarousel()
{
  if (m_last_category != m_ccat)
  {
    m_last_category = m_ccat;
    FilterGamesForCategory();

    m_selectedGameIdx = 0;
  }

  if (ImGui::GetIO().NavInputs[ImGuiNavInput_Activate] > 0.5f)
  {
    if (m_displayed_games.size() != 0)
      return m_displayed_games[m_selectedGameIdx];
  }

  // Display 5 games, 2 games to the left of the selection, 2 games to the right.
  for (int i = m_selectedGameIdx - 2; i < m_selectedGameIdx + 3; i++)
  {
    int idx = i;
    if (m_displayed_games.size() >= 4)
    {
      if (i < 0)
      {
        // wrap around, total games + -index
        idx = static_cast<int>(m_displayed_games.size()) + i;
      }
      else if (i >= m_displayed_games.size())
      {
        // wrap around, i - total games
        idx = i - static_cast<int>(m_displayed_games.size());
      }
    }
    else
    {
      if (i < 0)
      {
        idx = static_cast<int>(m_displayed_games.size()) + i;
      }
      else if (i >= m_displayed_games.size())
      {
        continue;
      }
    }

    if (idx < 0 || idx >= m_displayed_games.size())
      continue;

    ImVec4 border_col;
    float selectedScale = 1.0f;
    if (m_selectedGameIdx == idx)
    {
      border_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
      // The background image doesn't fit 2 games very well when scaled up.
      selectedScale = m_displayed_games.size() > 2 ? 1.15 : 1.0f;
    }
    else
    {
      border_col = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    AbstractTexture* handle = GetHandleForGame(m_displayed_games[idx]);
    ImGui::SameLine();
    ImGui::BeginChild(
        m_displayed_games[idx]->GetFilePath().c_str(),
        ImVec2((160 + 25) * m_frame_scale * selectedScale, 250 * m_frame_scale * selectedScale), true,
        ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar);

    if (handle != 0)
    {
      ImGui::Image((ImTextureID) handle,
          ImVec2(160.f * m_frame_scale * selectedScale, 224.f * m_frame_scale * selectedScale),
          ImVec2(0, 0), ImVec2(1, 1), ImVec4(1.0f, 1.0f, 1.0f, 1.0f), border_col);
      ImGui::Text(m_displayed_games[idx]->GetName(m_title_database).c_str());
    }
    else
    {
      ImGui::Text(m_displayed_games[idx]->GetName(m_title_database).c_str());
    }

    ImGui::EndChild();
  }

  return nullptr;
}

AbstractTexture* ImGuiFrontend::GetHandleForGame(std::shared_ptr<UICommon::GameFile> game)
{
  std::string game_id = game->GetGameID();
  auto result = m_cover_textures.find(game_id);
  if (m_cover_textures.find(game_id) == m_cover_textures.end())
  {
    std::shared_ptr<AbstractTexture> texture = CreateCoverTexture(game);
    if (texture == nullptr)
    {
      AbstractTexture* missing = GetOrCreateMissingTex();
      m_cover_textures.emplace(game_id, missing);
      return missing;
    }
    else
    {
      auto pair = m_cover_textures.emplace(game_id, std::move(texture));
      return pair.first->second.get();
    }
  }

  return result->second.get();
}

std::shared_ptr<AbstractTexture> CreateTextureFromPath(std::string path)
{
  std::string png;
  if (!File::ReadFileToString(path, png))
    return {};

  std::vector<uint8_t> buffer = {png.begin(), png.end()};
  if (buffer.empty())
    return {};

  std::vector<uint8_t> data;
  u32 width, height;
  Common::LoadPNG(buffer, &data, &width, &height);

  TextureConfig tex_config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0);

  std::shared_ptr<AbstractTexture> tex = g_gfx->CreateTexture(tex_config, path);
  if (!tex)
  {
    PanicAlertFmt("Failed to create ImGui texture");
    return {};
  }

  tex->Load(0, width, height, width, data.data(), sizeof(u32) * width * height);

  return std::move(tex);
}

std::shared_ptr<AbstractTexture>
ImGuiFrontend::CreateCoverTexture(std::shared_ptr<UICommon::GameFile> game)
{
  if (!File::Exists(File::GetUserPath(D_COVERCACHE_IDX) + game->GetGameTDBID() + ".png"))
  {
    game->DownloadDefaultCover();
  }

  return std::move(CreateTextureFromPath(File::GetUserPath(D_COVERCACHE_IDX) + game->GetGameTDBID() + ".png"));
}

AbstractTexture* ImGuiFrontend::GetOrCreateMissingTex()
{
  if (m_missing_tex != nullptr)
    return m_missing_tex.get();
  
  auto missing_tex = CreateTextureFromPath("Assets/missing.png");
  m_missing_tex = std::move(missing_tex);

  return m_missing_tex.get();
}

void ImGuiFrontend::LoadGameList()
{
  m_paths.clear();
  m_games.clear();
  m_displayed_games.clear();
  m_paths = Config::GetIsoPaths();
  for (auto dir : m_paths)
  {
    RecurseFolderForGames(dir);
  }

#ifdef WINRT_XBOX
  // Load from the default path
  auto localCachePath = winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path());
  RecurseFolderForGames(localCachePath);
#endif

  std::sort(m_games.begin(), m_games.end(),
          [this](std::shared_ptr<UICommon::GameFile> first,
                   std::shared_ptr<UICommon::GameFile> second) {
          return first->GetName(m_title_database) < second->GetName(m_title_database);
  });

  
  FilterGamesForCategory();
}

void ImGuiFrontend::FilterGamesForCategory()
{
  m_displayed_games.clear();

  switch (m_ccat)
  {
  case CAll:
    m_displayed_games.insert(m_displayed_games.begin(), m_games.begin(), m_games.end());
    break;
  case CWii:
    for (auto& game : m_games)
    {
      if (game->GetPlatform() != DiscIO::Platform::WiiDisc)
        continue;

      m_displayed_games.push_back(game);
    }
    break;
  case CGC:
    for (auto& game : m_games)
    {
      if (game->GetPlatform() != DiscIO::Platform::GameCubeDisc)
        continue;

      m_displayed_games.push_back(game);
    }
    break;
  case COther:
    for (auto& game : m_games)
    {
      if (game->GetPlatform() == DiscIO::Platform::WiiDisc ||
          game->GetPlatform() == DiscIO::Platform::GameCubeDisc)
        continue;

      m_displayed_games.push_back(game);
    }
    break;
  }
}

void ImGuiFrontend::LoadThemes()
{
  std::string selected_theme = Config::Get(Config::FRONTEND_SELECTED_THEME);

  m_themes.clear();
  m_selected_theme = nullptr;

  RecurseForThemes("Sys/FrontendThemes/");
  RecurseForThemes(File::GetUserPath(D_THEMES_IDX));
  
  if (!m_selected_theme)
  {
    m_selected_theme = &m_themes["Default"];
  }
}

void ImGuiFrontend::RecurseForThemes(std::string path)
{
  std::string selected_theme = Config::Get(Config::FRONTEND_SELECTED_THEME);

  for (auto folder : std::filesystem::directory_iterator(path))
  {
    if (!folder.is_directory())
    {
      continue;
    }

    FrontendTheme theme{};
    if (theme.TryLoad(folder.path().string()))
    {
      m_themes.emplace(theme.GetName(), theme);

      if (theme.GetName() == selected_theme)
      {
        m_selected_theme = &m_themes[theme.GetName()];
      }
    }
  }
}

void ImGuiFrontend::RecurseFolderForGames(std::string path)
{
  try
  {
    for (auto file : std::filesystem::directory_iterator(path))
    {
      if (file.is_directory())
      {
        RecurseFolderForGames(file.path().string());
        continue;
      }

      if (!file.is_regular_file())
        continue;

      std::filesystem::path normalised = std::filesystem::path(file.path().string()).make_preferred();
      std::string game_path = normalised.string();
      std::replace(game_path.begin(), game_path.end(), '\\', '/');
      auto game = new UICommon::GameFile(game_path);

      if (game && game->IsValid())
        m_games.emplace_back(std::move(game));
    }
  }
  catch (std::exception)
  {
    // This folder can't be opened.
  }
}

void ImGuiFrontend::AddGameFolder(std::string path)
{
  m_paths.push_back(path);
  Config::SetIsoPaths(m_paths);
}

bool ImGuiFrontend::TryInput(std::string expression, std::shared_ptr<ciface::Core::Device> device)
{
  auto* input = device->FindInput(expression);
  if (input == nullptr)
    return false;

  return input->GetState() > 0.5f;
}

void DrawSettingsMenu(UIState* state, float frame_scale)
{
  if (ImGui::BeginListBox("##tabs", ImVec2(100 * frame_scale, -1)))
  {
    if (ImGui::Selectable("General", state->selectedTab == General))
    {
      state->selectedTab = General;
    }
    if (ImGui::Selectable("Interface", state->selectedTab == Interface))
    {
      state->selectedTab = Interface;
    }
    if (ImGui::Selectable("Graphics", state->selectedTab == Graphics))
    {
      state->selectedTab = Graphics;
    }
    if (ImGui::Selectable("Controls", state->selectedTab == Controls))
    {
      state->selectedTab = Controls;
    }
    if (ImGui::Selectable("GameCube", state->selectedTab == GC))
    {
      state->selectedTab = GC;
    }
    if (ImGui::Selectable("Wii", state->selectedTab == Wii))
    {
      state->selectedTab = Wii;
    }
    if (ImGui::Selectable("Advanced", state->selectedTab == Advanced))
    {
      state->selectedTab = Advanced;
    }
    if (ImGui::Selectable("Paths", state->selectedTab == Paths))
    {
      state->selectedTab = Paths;
    }
    if (ImGui::Selectable("About", state->selectedTab == About))
    {
      state->selectedTab = About;
    }

    ImGui::EndListBox();
  }

  ImGui::SameLine();
  if (ImGui::BeginChild("##tabview", ImVec2(-1, -1), true))
  {
    switch (state->selectedTab)
    {
    case General:
      CreateGeneralTab(state);
      break;
    case Interface:
      CreateInterfaceTab(state);
      break;
    case Graphics:
      CreateGraphicsTab(state);
      break;
    case Controls:
      CreateControlsTab(state);
      break;
    case GC:
      CreateGameCubeTab(state);
      break;
    case Wii:
      CreateWiiTab(state);
      break;
    case Paths:
      CreatePathsTab(state);
      break;
    case Advanced:
      CreateAdvancedTab(state);
      break;
    case About:
      ImGui::TextWrapped(
          "Dolphin Emulator on UWP - Version 1.1.6\n\n"
          "This is a fork of Dolphin Emulator introducing Xbox support with a big picture "
          "frontend, developed by SirMangler.\n"
          "Support me on Ko-Fi: https://ko-fi.com/sirmangler\n\n"
          "Dolphin Emulator is licensed under GPLv2+ and is not associated with Nintendo.");
      break;
    }

    ImGui::EndChild();
  }
}

std::shared_ptr<AbstractTexture> FrontendTheme::GetBackground(ThemeBG cat)
{
  return m_textures[cat];
}

bool FrontendTheme::TryLoad(std::string path)
{
  const auto SetThemeOrBackup = [=](ThemeBG target, ThemeBG backup, std::string p) {
    if (File::Exists(p))
    {
      auto bg = CreateTextureFromPath(p);
      m_textures[target] = std::move(bg);
    }

    if (!m_textures[target])
    {
      m_textures[target] = m_textures[backup];
    }
  };

  if (!File::Exists(path + "\\carousel_background_all.png") ||
      !File::Exists(path + "\\menu_background.png") ||
      !File::Exists(path + "\\list_ui.png") ||
      !File::Exists(path + "\\carousel_ui.png"))
  {
    return false;
  }

  auto all_bg = CreateTextureFromPath(path + "\\carousel_background_all.png");
  if (!all_bg)
    return false;

  m_textures[ThemeBG::BG_All] = all_bg;

  auto menu_bg = CreateTextureFromPath(path + "\\menu_background.png");
  if (!menu_bg)
    return false;

  m_textures[ThemeBG::BG_Menu] = menu_bg;

  auto list_ui_bg= CreateTextureFromPath(path + "\\list_ui.png");
  if (!list_ui_bg)
    return false;

  m_textures[ThemeBG::BG_List_UI] = list_ui_bg;

  auto carousel_ui_bg = CreateTextureFromPath(path + "\\carousel_ui.png");
  if (!carousel_ui_bg)
    return false;

  m_textures[ThemeBG::BG_Carousel_UI] = carousel_ui_bg;

  SetThemeOrBackup(BG_Wii, BG_All, path + "\\carousel_background_wii.png");
  SetThemeOrBackup(BG_GC, BG_All, path + "\\carousel_background_gamecube.png");
  SetThemeOrBackup(BG_Other, BG_All, path + "\\carousel_background_other.png");
  SetThemeOrBackup(BG_List, BG_Menu, path + "\\menu_background_list.png");
  SetThemeOrBackup(BG_Netplay, BG_Menu, path + "\\menu_background_netplay.png");

  m_name = std::filesystem::path(path).filename().string();

  return true;
}
}  // namespace ImGuiFrontend
