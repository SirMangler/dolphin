#include "ImGuiNetPlay.h"
#include "WinRTKeyboard.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>

#ifdef WINRT_XBOX
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/windows.graphics.display.core.h>
#include <winrt/windows.gaming.input.h>
#include <windows.applicationmodel.h>
#include <gamingdeviceinformation.h>

#include "DolphinWinRT/Host.h"
#include "DolphinWinRT/UWPUtils.h"

using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt;
#endif

#include "Core/Core.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/SyncIdentifier.h"
#include "Core/IOS/FS/FileSystem.h"

#include "Common/WindowSystemInfo.h"
#include "Common/HttpRequest.h"

#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"

#include "VideoCommon/NetPlayChatUI.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace ImGuiFrontend
{
NetPlayDrawResult result = NetPlayDrawResult::Continue;

NetPlay::SyncIdentifier m_current_game_identifier;
std::shared_ptr<const UICommon::GameFile> m_host_selected_game = nullptr;
bool m_traversal = false;
char m_nick_buf[32];
char m_host_buf[32];
char m_search_buf[32];
std::string m_warning_text;
bool m_prompt_warning = false;

std::string m_prev_search;
std::vector<std::shared_ptr<UICommon::GameFile>> m_search_results;
Common::Lazy<std::string> m_external_ip;

ImGuiNetPlay::ImGuiNetPlay(ImGuiFrontend* frontend, std::vector<std::shared_ptr<UICommon::GameFile>> games, float frame_scale)
    : m_frontend(frontend), m_games(games), m_frameScale(frame_scale)
{
  std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);
  strcpy(m_nick_buf, nickname.data());

  std::string type = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  std::string address = Config::Get(type == "traversal" ? Config::NETPLAY_HOST_CODE :
                                        Config::NETPLAY_ADDRESS);
  strcpy(m_host_buf, address.data());
}

void ImGuiNetPlay::DrawLobbyWindow()
{
  ImGui::SetNextWindowSize(ImVec2(540 * m_frameScale, 425 * m_frameScale));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - (540 / 2) * m_frameScale,
                                 ImGui::GetIO().DisplaySize.y / 2 - (425 / 2) * m_frameScale));

  if (ImGui::Begin("Netplay Lobby"))
  {
    if (g_netplay_server != nullptr)
    {
      std::string address;
      if (m_traversal)
      {
        const auto host_id = Common::g_TraversalClient->GetHostID();
        address = Common::g_TraversalClient->IsConnected() ?
                      std::string(host_id.begin(), host_id.end()) :
                      "Connecting..";
      }
      else
      {
        if (!m_external_ip->empty())
        {
          address = m_external_ip->c_str();
        }
        else
        {
          address = "Unknown";
        }
      }

      ImGui::Text(m_traversal ? "Lobby Code: %s" : "External IP: %s", address.c_str());
    }

    auto game = g_netplay_dialog->FindGameFile(m_current_game_identifier);
    if (game)
    {
      ImGui::Text("Selected Game: %s", game->GetName(m_frontend->m_title_database).c_str());
    }

    ImGui::Spacing();
    
    DrawLobbyMenu();

    if (g_netplay_server)
    {
      ImGui::Spacing();

      if (ImGui::Button("Start Game"))
      {
        if (g_netplay_server->RequestStartGame())
        {
          ImGui::End();
          return;
        }
      }

      ImGui::SameLine();

      if (ImGui::Button("Exit Lobby"))
      {
        g_netplay_client->Stop();
        g_netplay_client = nullptr;
        g_netplay_server = nullptr;
      }
    }

    if (m_prompt_warning)
    {
      m_prompt_warning = false;
      ImGui::OpenPopup("Warning");
    }

    if (ImGui::BeginPopupModal("Warning"))
    {
      ImGui::Text(m_warning_text.c_str());
      ImGui::Separator();
      if (ImGui::Button("OK"))
      {
        ImGui::CloseCurrentPopup();
      }

      ImGui::EndPopup();
    }

    ImGui::Dummy(ImVec2(0.0f, 25.0f));
    ImGui::Separator();
    ImGui::TextWrapped("Please note that Xbox NetPlay is still early and may not show all notifications for various things such as synchronising memory cards, and waiting for client's games to start (e.g Compile Shaders Before Starting).\nThis may appear as a black screen until the game boots.");

    ImGui::End();
  }
}

void ImGuiNetPlay::DrawSetup()
{
  constexpr auto Warning = [](const char* text) {
    m_warning_text = std::string(text);
    ImGui::OpenPopup("Warning");
  };

  ImGui::SetNextWindowSize(ImVec2(540 * m_frameScale, 425 * m_frameScale));
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - (540 / 2) * m_frameScale,
                                 ImGui::GetIO().DisplaySize.y / 2 - (425 / 2) * m_frameScale));

  if (ImGui::Begin("Netplay", nullptr,
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize))
  {
    std::string type = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
    m_traversal = type == "traversal";

    if (ImGui::BeginCombo("Connection Type", type.c_str()))
    {
      if (ImGui::Selectable("Direct Connection", type == "direct"))
      {
        Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, "direct");
        Config::Save();
      }

      if (ImGui::Selectable("Traversal Server", type == "traversal"))
      {
        Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, "traversal");
        Config::Save();
      }

      ImGui::EndCombo();
    }

    if (ImGui::Button("Edit Nickname"))
    {
      UWP::ShowKeyboard();
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SameLine();
    if (ImGui::InputText("##nickname", m_nick_buf, 32))
    {
      printf(m_nick_buf);
    }

    ImGui::Spacing();

    ImGui::BeginTabBar("#connectionTabs");
    if (ImGui::BeginTabItem("Connect"))
    {
      if (ImGui::Button(m_traversal ? "Set Host Code " : "Set Host IP"))
      {
        UWP::ShowKeyboard();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SameLine();
      ImGui::InputText("##address", m_host_buf, 32);


      if (ImGui::Button("Connect"))
      {
        size_t nick_len = strlen(m_nick_buf);
        size_t host_len = strlen(m_host_buf);
        if (nick_len == 0)
        {
          Warning("Please enter a valid nickname!");
        }
        else if (host_len == 0)
        {
          Warning("Please enter a valid IP address / host code!");
        }
        else
        {
          Config::SetBaseOrCurrent(Config::NETPLAY_NICKNAME, std::string(m_nick_buf));
          Config::Save();

          Config::SetBaseOrCurrent(m_traversal ? Config::NETPLAY_HOST_CODE :
                                                 Config::NETPLAY_ADDRESS,
                                   std::string(m_host_buf));
          Config::Save();

          std::string host_ip = m_traversal ? Config::Get(Config::NETPLAY_HOST_CODE) :
                                              Config::Get(Config::NETPLAY_ADDRESS);
          u16 host_port = Config::Get(Config::NETPLAY_CONNECT_PORT);

          const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
          const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
          const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);

          g_netplay_client = std::make_shared<NetPlay::NetPlayClient>(
              host_ip, host_port, this, nickname,
              NetPlay::NetTraversalConfig{m_traversal, traversal_host, traversal_port});
        }
      }

      if (ImGui::BeginPopupModal("Warning"))
      {
        ImGui::Text(m_warning_text.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK"))
        {
          ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
      }

      ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Host"))
    {
      if (ImGui::BeginListBox("Games List"))
      {
        size_t search = strlen(m_search_buf);
        std::vector<std::shared_ptr<UICommon::GameFile>> games;
        if (search > 0)
        {
          std::string search_phrase = std::string(m_search_buf);
          if (search_phrase != m_prev_search)
          {
            m_search_results.clear();
            for (auto& game : m_games)
            {
              std::string name = game->GetLongName();
              if (name == "")
                name = game->GetFileName();

              auto it = std::search(name.begin(), name.end(), search_phrase.begin(),
                                    search_phrase.end(),
                                    [](unsigned char ch1, unsigned char ch2) {
                                      return std::toupper(ch1) == std::toupper(ch2);
                                    });

              if (it != name.end())
              {
                m_search_results.push_back(game);
              }
            }

            m_prev_search = m_search_buf;
          }

          games = m_search_results;
        }
        else
        {
          games = m_games;
        }

        for (auto& game : games)
        {
          std::string name = game->GetLongName();
          if (name == "")
            name = game->GetFileName();

          if (ImGui::Selectable(
                  std::format("{}##{}", name, game->GetFilePath()).c_str(),
                                m_host_selected_game == game))
          {
            m_host_selected_game = game;
          }
        }

        ImGui::EndListBox();
      }

      if (ImGui::Button("Search Game"))
      {
        UWP::ShowKeyboard();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SameLine();
      ImGui::InputText("##gamesearch", m_search_buf, 32);

      ImGui::Spacing();

      bool strict_sync = Config::Get(Config::NETPLAY_STRICT_SETTINGS_SYNC);
      if (ImGui::Checkbox("Strict Settings Synchronisation", &strict_sync))
      {
        Config::SetBaseOrCurrent(Config::NETPLAY_STRICT_SETTINGS_SYNC, strict_sync);
        Config::Save();
      }

      bool upnp = Config::Get(Config::NETPLAY_USE_UPNP);
      if (ImGui::Checkbox("Use UPNP", &upnp))
      {
        Config::SetBaseOrCurrent(Config::NETPLAY_USE_UPNP, upnp);
        Config::Save();
      }

      if (!m_traversal)
      {
        int port = static_cast<int>(Config::Get(Config::NETPLAY_HOST_PORT));
        if (ImGui::InputInt("Host Port", &port))
        {
          Config::SetBaseOrCurrent(Config::NETPLAY_HOST_PORT, static_cast<u16>(port));
          Config::Save();
        }
      }

      if (ImGui::Button("Host Lobby"))
      {
        size_t nick_len = strlen(m_nick_buf);
        if (nick_len == 0)
        {
          Warning("Please enter a valid nickname!");
        }
        else if (m_host_selected_game == nullptr)
        {
          Warning("Please select a game!");
        }
        else
        {
          Config::SetBaseOrCurrent(Config::NETPLAY_NICKNAME, std::string(m_nick_buf));
          Config::Save();

          // Settings
          u16 host_port = Config::Get(Config::NETPLAY_HOST_PORT);
          const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
          const bool is_traversal = traversal_choice == "traversal";
          const bool use_upnp = Config::Get(Config::NETPLAY_USE_UPNP);

          const std::string traversal_host = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
          const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);

          if (is_traversal)
            host_port = Config::Get(Config::NETPLAY_LISTEN_PORT);

          // Create Server
          g_netplay_server = std::make_shared<NetPlay::NetPlayServer>(
              host_port, use_upnp, this,
              NetPlay::NetTraversalConfig{is_traversal, traversal_host, traversal_port});

          if (!g_netplay_server->is_connected)
          {
            Warning("Could not create the netplay server. Is the port already in use?");
            g_netplay_server = nullptr;
          }
          else
          {
            g_netplay_server->ChangeGame(m_host_selected_game->GetSyncIdentifier(),
                m_host_selected_game->GetNetPlayName(m_frontend->m_title_database));

            std::string host_ip = "127.0.0.1";

            const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);
            const std::string network_mode = Config::Get(Config::NETPLAY_NETWORK_MODE);

            g_netplay_client = std::make_shared<NetPlay::NetPlayClient>(
                host_ip, host_port, this, nickname,
                NetPlay::NetTraversalConfig{false, traversal_host, traversal_port});

            m_external_ip = Common::Lazy<std::string>([]() -> std::string {
              Common::HttpRequest request;
              // ENet does not support IPv6, so IPv4 has to be used
              request.UseIPv4();
              Common::HttpRequest::Response response =
                  request.Get("https://ip.dolphin-emu.org/", {{"X-Is-Dolphin", "1"}});

              if (response.has_value())
                return std::string(response->begin(), response->end());
              return "";
            });
          }
        }
      }

      if (ImGui::BeginPopupModal("Warning"))
      {
        ImGui::Text(m_warning_text.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK"))
        {
          ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
      }

      ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    ImGui::End();
  }
}

NetPlayDrawResult ImGuiNetPlay::Draw()
{
  if (g_netplay_client)
  {
    DrawLobbyWindow();
  }
  else
  {
    DrawSetup();
  }

  return result;
}

void ImGuiNetPlay::BootGame(const std::string& filename,
                            std::unique_ptr<BootSessionData> boot_session_data) {
// Todo, make the host handle this.
#ifdef WINRT_XBOX
// Visual Studio / Microsoft Programming moment
#pragma warning(push)
#pragma warning(disable : 4265)
  CoreApplication::MainView().Dispatcher().RunAsync(
      winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
      [filename, data = std::move(boot_session_data)] {
        CoreWindow window = CoreWindow::GetForCurrentThread();
        void* abi = winrt::get_abi(window);

        WindowSystemInfo wsi;
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
            wsi.render_width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
            wsi.render_height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
            // Our UI is based on 1080p, and we're adding a modifier to zoom in by 80%
            wsi.render_surface_scale = ((float)wsi.render_width / 1920.0f) * 1.8f;
          }
        }

        std::unique_ptr<BootParameters> boot =
            BootParameters::GenerateFromFile(filename, std::move(*data));

        if (!BootManager::BootCore(std::move(boot), wsi))
        {
          fprintf(stderr, "Could not boot the specified file\n");
        }
    });
#pragma warning(pop)
#endif

  result = NetPlayDrawResult::BootGame;
};

void ImGuiNetPlay::DisplayMessage(std::string msg, int duration, float r, float g, float b)
{
  if (g_ActiveConfig.bShowNetPlayMessages && Core::IsRunning())
    g_netplay_chat_ui->AppendChat(msg, {r, g, b});
}

void ImGuiNetPlay::Reset()
{
  result = NetPlayDrawResult::Continue;
}

void ImGuiNetPlay::StopGame()
{
  g_netplay_client->StopGame();
}

bool ImGuiNetPlay::IsHosting() const
{
  return g_netplay_server != nullptr;
}

void ImGuiNetPlay::Update()
{
}

void ImGuiNetPlay::AppendChat(const std::string& msg)
{
  DisplayMessage(msg, OSD::Duration::NORMAL, 1.0f, 1.0f,
                 0.0f);
}

void ImGuiNetPlay::OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                                   const std::string& netplay_name)
{
  m_current_game_identifier = sync_identifier;
}

void ImGuiNetPlay::OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config)
{
}

void ImGuiNetPlay::OnMsgStartGame()
{
  g_netplay_chat_ui =
      std::make_unique<NetPlayChatUI>([this](const std::string& message) {});

  auto game = FindGameFile(m_current_game_identifier);
  if (game)
  {
    g_netplay_client->StartGame(game->GetFilePath());
  }
  else
  {
    m_warning_text = "Could not find the selected game.";
    m_prompt_warning = true;
  }
}

void ImGuiNetPlay::OnMsgStopGame()
{
  g_netplay_client->StopGame();
}

void ImGuiNetPlay::OnMsgPowerButton()
{

}

void ImGuiNetPlay::OnPlayerConnect(const std::string& player)
{
  DisplayMessage(std::format("Player {} has connected", player), OSD::Duration::NORMAL, 1.0f,
                 1.0f, 0.0f);
}

void ImGuiNetPlay::OnPlayerDisconnect(const std::string& player)
{
  DisplayMessage(std::format("Player {} has disconnnected", player), OSD::Duration::NORMAL, 1.0f,
    1.0f, 0.0f);
}

void ImGuiNetPlay::OnPadBufferChanged(u32 buffer)
{
}

void ImGuiNetPlay::OnHostInputAuthorityChanged(bool enabled)
{
}

void ImGuiNetPlay::OnDesync(u32 frame, const std::string& player)
{
  DisplayMessage("Possible desync detected.", OSD::Duration::VERY_LONG, 1.0f, 0.0f, 0.0f);
}

void ImGuiNetPlay::OnConnectionLost()
{
}

void ImGuiNetPlay::OnConnectionError(const std::string& message)
{
  m_warning_text = std::string(message);
  m_prompt_warning = true;

  g_netplay_client = nullptr;
}

void ImGuiNetPlay::OnTraversalError(Common::TraversalClient::FailureReason error)
{
  DisplayMessage("A traversal error has occurred", OSD::Duration::VERY_LONG, 1.0f, 0.0f, 0.0f);
}

void ImGuiNetPlay::OnTraversalStateChanged(Common::TraversalClient::State state)
{

}

void ImGuiNetPlay::OnGameStartAborted()
{
  if (Core::IsRunningAndStarted())
  {
#ifdef WINRT_XBOX
    // todo make the host manage this
    UWP::g_shutdown_requested.Set();
#endif
  }
}

void ImGuiNetPlay::OnGolferChanged(bool is_golfer,
                                   const std::string& golfer_name)
{
}

bool ImGuiNetPlay::IsRecording()
{
  return false;
}

std::shared_ptr<const UICommon::GameFile>
ImGuiNetPlay::FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
             NetPlay::SyncIdentifierComparison* found)
{
  NetPlay::SyncIdentifierComparison temp;
  if (!found)
    found = &temp;

  *found = NetPlay::SyncIdentifierComparison::DifferentGame;

  for (auto& game : m_games)
  {
    *found = std::min(*found, game->CompareSyncIdentifier(sync_identifier));
    if (*found == NetPlay::SyncIdentifierComparison::SameGame)
      return game;
  }
  
  return nullptr;
}

std::string ImGuiNetPlay::FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                           int device_number)
{
  return "";
}

void ImGuiNetPlay::ShowGameDigestDialog(const std::string& title)
{
}

void ImGuiNetPlay::SetGameDigestProgress(int pid, int progress)
{
}

void ImGuiNetPlay::SetGameDigestResult(int pid, const std::string& r)
{
}

void ImGuiNetPlay::AbortGameDigest()
{
}

void ImGuiNetPlay::OnIndexAdded(bool success, std::string error)
{
}

void ImGuiNetPlay::OnIndexRefreshFailed(std::string error)
{
}

void ImGuiNetPlay::ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                             const std::vector<int>& players)
{
}

void ImGuiNetPlay::HideChunkedProgressDialog()
{
}

void ImGuiNetPlay::SetChunkedProgress(int pid, u64 progress)
{
}

void ImGuiNetPlay::SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder)
{
  if (g_netplay_client)
    g_netplay_client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
}

void DrawLobbyMenu()
{
  if (g_netplay_client == nullptr)
    return;

  auto players = g_netplay_client->GetPlayers();
  if (ImGui::BeginTable("players", 2))
  {
    ImGui::TableSetupColumn("Player");
    ImGui::TableSetupColumn("Latency");
    ImGui::TableHeadersRow();
    for (auto& player : players)
    {
      ImGui::TableNextColumn();
      ImGui::Text("%s", player->name.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%d", player->ping);
    }

    ImGui::EndTable();
  }

  if (g_netplay_server != nullptr)
  {
    ImGui::Spacing();

    int pad_buffer = Config::Get(Config::NETPLAY_BUFFER_SIZE);
    if (ImGui::InputInt("Pad Buffer", &pad_buffer))
    {
      Config::SetBaseOrCurrent(Config::NETPLAY_BUFFER_SIZE, pad_buffer);
      Config::Save();
      g_netplay_server->AdjustPadBufferSize(static_cast<unsigned int>(pad_buffer));
    }

    if (ImGui::BeginTable("gc-slots", 4))
    {
      ImGui::TableNextColumn();
      ImGui::Text("GC Pad 1");
      ImGui::TableNextColumn();
      ImGui::Text("GC Pad 2");
      ImGui::TableNextColumn();
      ImGui::Text("GC Pad 3");
      ImGui::TableNextColumn();
      ImGui::Text("GC Pad 4");

      ImGui::TableNextRow();

      auto gc_mapping = g_netplay_server->GetPadMapping();
      for (uint32_t port = 0; port < 4; port++)
      {
        ImGui::TableNextColumn();
        std::string selected_player = "None";
        for (auto player : players)
        {
          if (gc_mapping[port] == player->pid)
          {
            selected_player = player->name;
            break;
          }
        }

        if (ImGui::BeginCombo(std::format("##port-{}", port).c_str(), selected_player.c_str()))
        {
          for (auto& player : players)
          {
            if (ImGui::Selectable(player->name.c_str(), gc_mapping[port] == player->pid))
            {
              gc_mapping[port] = player->pid;
              g_netplay_server->SetPadMapping(gc_mapping);
            }
          }

          if (ImGui::Selectable("None", gc_mapping[port] == 0))
          {
            gc_mapping[port] = 0;
            g_netplay_server->SetPadMapping(gc_mapping);
          }

          ImGui::EndCombo();
        }
      }

      ImGui::EndTable();
    }

    ImGui::Spacing();

    if (ImGui::BeginTable("wii-slots", 4))
    {
      ImGui::TableNextColumn();
      ImGui::Text("Wii Pad 1");
      ImGui::TableNextColumn();
      ImGui::Text("Wii Pad 2");
      ImGui::TableNextColumn();
      ImGui::Text("Wii Pad 3");
      ImGui::TableNextColumn();
      ImGui::Text("Wii Pad 4");

      ImGui::TableNextRow();

      auto wii_mapping = g_netplay_server->GetWiimoteMapping();
      for (uint32_t port = 0; port < 4; port++)
      {
        std::string selected_player = "None";
        for (auto player : players)
        {
          if (wii_mapping[port] == player->pid)
          {
            selected_player = player->name;
            break;
          }
        }

        ImGui::TableNextColumn();
        if (ImGui::BeginCombo(std::format("##wiiport-{}", port).c_str(), selected_player.c_str()))
        {
          for (auto& player : g_netplay_client->GetPlayers())
          {
            if (ImGui::Selectable(player->name.c_str(), wii_mapping[port] == player->pid))
            {
              wii_mapping[port] = player->pid;
              g_netplay_server->SetWiimoteMapping(wii_mapping);
            }
          }

          if (ImGui::Selectable("None", wii_mapping[port] == 0))
          {
            wii_mapping[port] = 0;
            g_netplay_server->SetWiimoteMapping(wii_mapping);
          }

          ImGui::EndCombo();
        }
      }

      ImGui::EndTable();
    }

    ImGui::Spacing();
  }
}

}  // namespace ImGuiFrontend
