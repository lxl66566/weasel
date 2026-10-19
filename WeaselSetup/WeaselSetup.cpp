// WeaselSetup.cpp : main source file for WeaselSetup.exe
//

#include "stdafx.h"

#include "resource.h"
#include "WeaselUtility.h"
#include "PerUserReg.h"
#include "SetupUtil.h"
#include <thread>

#include "InstallOptionsDlg.h"

#include <ShellScalingApi.h>
#pragma comment(lib, "Shcore.lib")
CAppModule _Module;

static int Run(LPTSTR lpCmdLine);
static bool IsProcAdmin();
static int RestartAsAdmin(LPTSTR lpCmdLine);

int WINAPI _tWinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPTSTR lpstrCmdLine,
                     int /*nCmdShow*/) {
  HRESULT hRes = ::CoInitialize(NULL);
  ATLASSERT(SUCCEEDED(hRes));
  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

  AtlInitCommonControls(
      ICC_BAR_CLASSES);  // add flags to support other controls

  hRes = _Module.Init(NULL, hInstance);
  ATLASSERT(SUCCEEDED(hRes));

  LANGID langId = get_language_id();
  SetThreadUILanguage(langId);
  SetThreadLocale(langId);

  int nRet = Run(lpstrCmdLine);

  _Module.Term();
  ::CoUninitialize();

  return nRet;
}
int install(const std::wstring& profile, bool silent);
int uninstall(bool silent);
bool has_installed();
// 已安装状态下切换 profile 时同步 TSF 注册（K14）
int switch_registered_profile(const std::wstring& old_profile,
                              const std::wstring& new_profile,
                              bool silent);

static std::wstring install_dir() {
  WCHAR exe_path[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandle(NULL), exe_path, _countof(exe_path));
  std::wstring dir(exe_path);
  size_t pos = dir.find_last_of(L"\\");
  dir.resize(pos);
  return dir;
}

static int CustomInstall(bool installing) {
  std::wstring profile = L"hans";
  bool silent = false;
  std::wstring user_dir;

  const WCHAR KEY[] = L"Software\\Rime\\Weasel";
  HKEY hKey;
  LSTATUS ret =
      RegOpenKeyW(per_user_root(), per_user_subkey(KEY).c_str(), &hKey);
  if (ret == ERROR_SUCCESS) {
    std::wstring stored;
    if (read_reg_sz(hKey, L"RimeUserDir", stored)) {
      user_dir = stored;
    }
    if (read_reg_sz(hKey, L"Profile", stored) && !stored.empty()) {
      profile = stored;
      if (installing)
        silent = true;
    } else {
      DWORD data = 0;
      DWORD len = sizeof(data);
      DWORD type = 0;
      ret = RegQueryValueEx(hKey, L"Hant", NULL, &type, (LPBYTE)&data, &len);
      if (ret == ERROR_SUCCESS && type == REG_DWORD) {
        profile = (data != 0) ? L"hant" : L"hans";
        if (installing)
          silent = true;
      }
    }
    RegCloseKey(hKey);
  }
  // TSF 注册所用的 profile 与此持久化值一致（install() 同时写两者），
  // 作为修改安装时判断 profile 是否变化的基准
  const std::wstring registered_profile = profile;
  bool _has_installed = has_installed();
  if (!silent) {
    InstallOptionsDialog dlg;
    dlg.installed = _has_installed;
    dlg.profile = profile;
    dlg.user_dir = user_dir;
    if (IDOK != dlg.DoModal())
      return 1;  // 用户取消：一律中止，不以默认 profile 继续安装（A13）
    profile = dlg.profile;
    user_dir = dlg.user_dir;
    _has_installed = dlg.installed;
  }
  if (!_has_installed) {
    if (0 != install(profile, silent))
      return 1;
  } else if (profile != registered_profile) {
    // 修改安装换了 profile：同步 TSF 注册，不能只写注册表（K14）
    if (0 != switch_registered_profile(registered_profile, profile, silent))
      return 1;
  }

  if (user_dir.empty())
    user_dir = per_user_default_dir();
  ret = SetRegKeyValue(per_user_root(), per_user_subkey(KEY).c_str(),
                       L"RimeUserDir", user_dir.c_str(), REG_SZ, false);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_BY_IDS(IDS_STR_ERR_WRITE_USER_DIR, IDS_STR_INSTALL_FAILED,
               MB_ICONERROR | MB_OK);
    return 1;
  }
  ret = SetRegKeyValue(per_user_root(), per_user_subkey(KEY).c_str(),
                       L"Profile", profile.c_str(), REG_SZ, false);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_BY_IDS(IDS_STR_ERR_WRITE_PROFILE, IDS_STR_INSTALL_FAILED,
               MB_ICONERROR | MB_OK);
    return 1;
  }

  ret = SetRegKeyValue(per_user_root(), per_user_subkey(KEY).c_str(), L"Hant",
                       (profile == L"hant" ? 1 : 0), REG_DWORD, false);
  if (FAILED(HRESULT_FROM_WIN32(ret))) {
    MSG_BY_IDS(IDS_STR_ERR_WRITE_HANT, IDS_STR_INSTALL_FAILED,
               MB_ICONERROR | MB_OK);
    return 1;
  }
  if (_has_installed) {
    std::wstring dir(install_dir());
    // 服务重启+部署序列与"修改成功"提示并发执行（用户读提示的 1 秒里
    // 服务已在重启），但必须 join：进程先退出会腰斩 detached 线程，
    // 服务停在半重启状态
    std::thread th([dir]() {
      ShellExecuteW(NULL, NULL, (dir + L"\\WeaselServer.exe").c_str(), L"/q",
                    NULL, SW_SHOWNORMAL);
      Sleep(500);
      ShellExecuteW(NULL, NULL, (dir + L"\\WeaselServer.exe").c_str(), L"",
                    NULL, SW_SHOWNORMAL);
      Sleep(500);
      ShellExecuteW(NULL, NULL, (dir + L"\\WeaselDeployer.exe").c_str(),
                    L"/deploy", NULL, SW_HIDE);
    });
    MSG_BY_IDS(IDS_STR_MODIFY_SUCCESS_INFO, IDS_STR_MODIFY_SUCCESS_CAP,
               MB_ICONINFORMATION | MB_OK);
    th.join();
  }

  return 0;
}

LPCTSTR GetParamByPrefix(LPCTSTR lpCmdLine, LPCTSTR prefix) {
  return (wcsncmp(lpCmdLine, prefix, wcslen(prefix)) == 0)
             ? (lpCmdLine + wcslen(prefix))
             : 0;
}

static int Run(LPTSTR lpCmdLine) {
  // "/origsid:<S-...>" is appended by RestartAsAdmin so the elevated process
  // can address the invoking user's hive. Strip the trailing parameter
  // before any command-line matching.
  apply_orig_sid_param(lpCmdLine);
  constexpr bool silent = true;
  // parameter /? or /help to show commandline args
  if (!wcscmp(L"/?", lpCmdLine) || !wcscmp(L"/help", lpCmdLine)) {
    WCHAR msg[1024] = {0};
    if (LoadString(GetModuleHandle(NULL), IDS_STR_HELP, msg,
                   sizeof(msg) / sizeof(TCHAR))) {
      MessageBox(NULL, msg, L"WeaselSetup", MB_ICONINFORMATION | MB_OK);
    } else {
      MessageBox(
          NULL,
          L"Usage: WeaselSetup.exe [options]\n"
          L"/? or /help    - Show this help message\n"
          L"/u             - Uninstall Weasel\n"
          L"/i             - Install Weasel\n"
          L"/s             - Install Weasel (Simplified Chinese)\n"
          L"/t             - Install Weasel (Traditional Chinese)\n"
          L"/hk            - Install Weasel (Traditional Chinese, Hong Kong)\n"
          L"/mc            - Install Weasel (Traditional Chinese, Macao)\n"
          L"/sg            - Install Weasel (Simplified Chinese, Singapore)\n"
          L"/ls            - Set Weasel language to Simplified Chinese\n"
          L"/lt            - Set Weasel language to Traditional Chinese\n"
          L"/le            - Set Weasel language to English\n"
          L"/eu            - Enable automatic update check\n"
          L"/du            - Disable automatic update check\n"
          L"/toggleime     - Toggle IME on open/close(ctrl+space)\n"
          L"/toggleascii   - Toggle ASCII on open/close(ctrl+space)\n"
          L"/testing       - Set update channel to testing\n"
          L"/release       - Set update channel to release\n"
          L"/userdir:<dir> - Set user directory\n",
          L"WeaselSetup", MB_ICONINFORMATION | MB_OK);
    }
    return 0;
  }
  bool uninstalling = !wcscmp(L"/u", lpCmdLine);
  if (uninstalling) {
    if (IsProcAdmin())
      return uninstall(silent);
    else
      return RestartAsAdmin(lpCmdLine);
  }

  if (auto res = GetParamByPrefix(lpCmdLine, L"/userdir:")) {
    // 剥离 /userdir:"..." 的引号；返回明确的成功/失败退出码（K15）
    const LSTATUS ret =
        SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                       L"RimeUserDir", unquote_argument(res), REG_SZ);
    return ret == ERROR_SUCCESS ? 0 : 1;
  }

  if (!wcscmp(L"/ls", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"Language", L"chs", REG_SZ);
  } else if (!wcscmp(L"/lt", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"Language", L"cht", REG_SZ);
  } else if (!wcscmp(L"/le", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"Language", L"eng", REG_SZ);
  }

  if (!wcscmp(L"/eu", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel\\Updates",
                          L"CheckForUpdates", L"1", REG_SZ);
  }
  if (!wcscmp(L"/du", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel\\Updates",
                          L"CheckForUpdates", L"0", REG_SZ);
  }

  if (!wcscmp(L"/toggleime", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"ToggleImeOnOpenClose", L"yes", REG_SZ);
  }
  if (!wcscmp(L"/toggleascii", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"ToggleImeOnOpenClose", L"no", REG_SZ);
  }
  if (!wcscmp(L"/testing", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"UpdateChannel", L"testing", REG_SZ);
  }
  if (!wcscmp(L"/release", lpCmdLine)) {
    return SetRegKeyValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                          L"UpdateChannel", L"release", REG_SZ);
  }

  if (!IsProcAdmin()) {
    return RestartAsAdmin(lpCmdLine);
  }

  bool hans = !wcscmp(L"/s", lpCmdLine);
  if (hans)
    return install(L"hans", silent);
  bool hant = !wcscmp(L"/t", lpCmdLine);
  if (hant)
    return install(L"hant", silent);
  bool hongkong = !wcscmp(L"/hk", lpCmdLine);
  if (hongkong)
    return install(L"hongkong", silent);
  bool macau = !wcscmp(L"/mc", lpCmdLine);
  if (macau)
    return install(L"macau", silent);
  bool singapore = !wcscmp(L"/sg", lpCmdLine);
  if (singapore)
    return install(L"singapore", silent);
  bool installing = !wcscmp(L"/i", lpCmdLine);
  return CustomInstall(installing);
}

// https://learn.microsoft.com/zh-cn/windows/win32/api/securitybaseapi/nf-securitybaseapi-checktokenmembership
bool IsProcAdmin() {
  BOOL b = FALSE;
  SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
  PSID AdministratorsGroup;
  b = AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &AdministratorsGroup);

  if (b) {
    if (!CheckTokenMembership(NULL, AdministratorsGroup, &b)) {
      b = FALSE;
    }
    FreeSid(AdministratorsGroup);
  }

  return (b);
}

int RestartAsAdmin(LPTSTR lpCmdLine) {
  SHELLEXECUTEINFO execInfo{0};
  TCHAR path[MAX_PATH];
  GetModuleFileName(GetModuleHandle(NULL), path, _countof(path));
  std::wstring params = with_orig_sid_param(lpCmdLine);
  execInfo.lpFile = path;
  execInfo.lpParameters = params.c_str();
  execInfo.lpVerb = _T("runas");
  execInfo.cbSize = sizeof(execInfo);
  execInfo.nShow = SW_SHOWNORMAL;
  execInfo.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
  execInfo.hwnd = NULL;
  execInfo.hProcess = NULL;
  if (::ShellExecuteEx(&execInfo) && execInfo.hProcess != NULL) {
    ::WaitForSingleObject(execInfo.hProcess, INFINITE);
    DWORD dwExitCode = 0;
    ::GetExitCodeProcess(execInfo.hProcess, &dwExitCode);
    ::CloseHandle(execInfo.hProcess);
    return dwExitCode;
  }
  return -1;
}
