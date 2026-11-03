#include "stdafx.h"
#include "WeaselServerApp.h"
#include <filesystem>
#include <winsparkle.h>

// WinSparkle contract (include/winsparkle.h, "Configuration functions"):
// win_sparkle_set_appcast_url() may only be called before the first
// win_sparkle_init(), from the main thread. So the feed URL is determined
// exactly once, ahead of init: at the top of Run(), or on the first
// check_update() call from the /update command line (which reaches
// check_update() before Run() calls win_sparkle_init()). After startup the
// one-shot guard makes check_update() a pure
// win_sparkle_check_update_with_ui() call, never a setup call.
static bool s_update_feed_configured = false;

static void SetupUpdateFeed() {
  if (s_update_feed_configured)
    return;
  s_update_feed_configured = true;
  // when checked manually, show testing versions too
  std::string feed_url = GetCustomResource("ManualUpdateFeedURL", "APPCAST");
  std::wstring channel{};
  auto ret = RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\Weasel",
                               L"UpdateChannel", channel);
  if (!ret && channel == L"testing") {
    feed_url = GetCustomResource("TestingManualUpdateFeedURL", "APPCAST");
  }
  if (!feed_url.empty()) {
    win_sparkle_set_appcast_url(feed_url.c_str());
  }
}

bool WeaselServerApp::check_update() {
  SetupUpdateFeed();
  win_sparkle_check_update_with_ui();
  return true;
}

WeaselServerApp::WeaselServerApp()
    : m_handler(std::make_unique<RimeWithWeaselHandler>(&m_ui)),
      tray_icon(m_ui) {
  // m_handler.reset(new RimeWithWeaselHandler(&m_ui));
  m_server.SetRequestHandler(m_handler.get());
  SetupMenuHandlers();
}

WeaselServerApp::~WeaselServerApp() {}

int WeaselServerApp::Run() {
  // Configure the appcast URL before Start() lets any pipe worker dispatch
  // WEASEL_IPC_TRAY_COMMAND, and before win_sparkle_init() below.
  SetupUpdateFeed();

  if (!m_server.Start())
    return -1;

  // win_sparkle_set_appcast_url("http://localhost:8000/weasel/update/appcast.xml");
  win_sparkle_set_registry_path("Software\\Rime\\Weasel\\Updates");
  if (GetThreadUILanguage() ==
      MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL))
    win_sparkle_set_lang("zh-TW");
  else if (GetThreadUILanguage() ==
           MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED))
    win_sparkle_set_lang("zh-CN");
  else
    win_sparkle_set_lang("en");
  win_sparkle_init();
  m_ui.Create(m_server.GetHWnd());

  m_handler->Initialize();
  m_handler->OnUpdateUI([this]() { tray_icon.RequestRefresh(); });

  tray_icon.Create(m_server.GetHWnd());
  m_server.SetTrayRefreshCallback([this]() { tray_icon.ApplyRefresh(); });
  tray_icon.RequestRefresh();

  int ret = m_server.Run();

  tray_icon.DisableRefresh();
  m_handler->Finalize();
  m_ui.Destroy();
  tray_icon.RemoveIcon();
  win_sparkle_cleanup();

  return ret;
}

void WeaselServerApp::SetupMenuHandlers() {
  std::filesystem::path dir = install_dir();
  m_server.AddMenuHandler(ID_WEASELTRAY_QUIT,
                          [this] { return m_server.Stop() == 0; });
  m_server.AddMenuHandler(ID_WEASELTRAY_DEPLOY,
                          std::bind(execute, dir / L"WeaselDeployer.exe",
                                    std::wstring(L"/deploy")));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_SETTINGS,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring()));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_DICT_MANAGEMENT,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring(L"/dict")));
  m_server.AddMenuHandler(
      ID_WEASELTRAY_SYNC,
      std::bind(execute, dir / L"WeaselDeployer.exe", std::wstring(L"/sync")));
  m_server.AddMenuHandler(ID_WEASELTRAY_WIKI,
                          std::bind(open, L"https://rime.im/docs/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_HOMEPAGE,
                          std::bind(open, L"https://rime.im/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_FORUM,
                          std::bind(open, L"https://rime.im/discuss/"));
  m_server.AddMenuHandler(ID_WEASELTRAY_CHECKUPDATE, check_update);
  m_server.AddMenuHandler(ID_WEASELTRAY_INSTALLDIR, std::bind(explore, dir));
  m_server.AddMenuHandler(ID_WEASELTRAY_USERCONFIG,
                          std::bind(explore, WeaselUserDataPath()));
  m_server.AddMenuHandler(ID_WEASELTRAY_LOGDIR,
                          std::bind(explore, WeaselLogPath()));
}
