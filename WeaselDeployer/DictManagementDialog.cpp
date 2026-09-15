#include "stdafx.h"
#include "DictManagementDialog.h"
#include "Configurator.h"
#include <WeaselUtility.h>
#include <rime_api.h>
#include "WeaselDeployer.h"

void static OpenFolderAndSelectItem(std::wstring filepath) {
  filepath = std::filesystem::path(filepath).make_preferred().wstring();
  std::wstring directory = std::filesystem::path(filepath).parent_path();

  // 主线程为 STA（_tWinMain 已 CoInitialize），MTA 初始化在此返回
  // RPC_E_CHANGED_MODE 且不加计数；仅真正加了计数（S_OK/S_FALSE）才配对
  // CoUninitialize，否则每次调用拆一层主线程 COM 计数
  HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);

  auto folder = ILCreateFromPath(directory.c_str());
  std::vector<LPITEMIDLIST> v;
  v.push_back(ILCreateFromPath(filepath.c_str()));

  SHOpenFolderAndSelectItems(folder, v.size(), (LPCITEMIDLIST*)v.data(), 0);

  for (auto idl : v) {
    ILFree(idl);
  }
  ILFree(folder);
  if (SUCCEEDED(hr)) {
    CoUninitialize();
  }
}

template <typename T, typename U>
inline static std::wstring DoFileDialog(HWND hwndOwner,
                                        LPCWSTR title,
                                        UINT filterSize,
                                        COMDLG_FILTERSPEC filter[],
                                        LPCWSTR filename,
                                        LPCWSTR defExt) {
  std::wstring path;
  CoInitialize(NULL);
  CComPtr<T> spFileDialog;
  if (SUCCEEDED(spFileDialog.CoCreateInstance(__uuidof(U)))) {
    spFileDialog->SetFileTypes(filterSize, filter);
    spFileDialog->SetTitle(title);
    if (filename)
      spFileDialog->SetFileName(filename);

    spFileDialog->SetDefaultExtension(defExt);
    if (SUCCEEDED(spFileDialog->Show(hwndOwner))) {
      CComPtr<IShellItem> spResult;
      if (SUCCEEDED(spFileDialog->GetResult(&spResult))) {
        wchar_t* name;
        if (SUCCEEDED(spResult->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
          path = name;
          CoTaskMemFree(name);
        }
      }
    }
  }
  CoUninitialize();
  return path;
}

DictManagementDialog::DictManagementDialog() {
  // levers 模块缺失（部署损坏）时 find_module 返回空，不能解引用；
  // 留空 api_，由 OnInitDialog 提示并关闭
  RimeModule* levers = rime_get_api()->find_module("levers");
  api_ = levers ? (RimeLeversApi*)levers->get_api() : nullptr;
  if (!api_) {
    LOG(ERROR) << "Rime levers module not found; dict management is unavailable.";
  }
}

DictManagementDialog::~DictManagementDialog() {}

void DictManagementDialog::Populate() {
  user_dict_list_.ResetContent();
  RimeUserDictIterator iter = {0};
  api_->user_dict_iterator_init(&iter);
  while (const char* dict = api_->next_user_dict(&iter)) {
    std::wstring txt = u8tow(dict);
    user_dict_list_.AddString(txt.c_str());
  }
  api_->user_dict_iterator_destroy(&iter);
  user_dict_list_.SetCurSel(-1);
  // 程序化 SetCurSel(-1) 不触发 LBN_SELCHANGE，须手动复位按钮状态
  backup_.EnableWindow(FALSE);
  export_.EnableWindow(FALSE);
  import_.EnableWindow(FALSE);
}

LRESULT DictManagementDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  if (!api_) {
    MSG_BY_IDS(IDS_STR_ERR_LEVERS_MISSING, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    EndDialog(IDCANCEL);
    return FALSE;
  }
  user_dict_list_.Attach(GetDlgItem(IDC_USER_DICT_LIST));
  backup_.Attach(GetDlgItem(IDC_BACKUP));
  backup_.EnableWindow(FALSE);
  restore_.Attach(GetDlgItem(IDC_RESTORE));
  restore_.EnableWindow(TRUE);
  export_.Attach(GetDlgItem(IDC_EXPORT));
  export_.EnableWindow(FALSE);
  import_.Attach(GetDlgItem(IDC_IMPORT));
  import_.EnableWindow(FALSE);

  Populate();

  InitCtrlRects();
  CenterWindow();
  BringWindowToTop();
  return TRUE;
}

LRESULT DictManagementDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

// LB_GETTEXT 不截断整串拷贝，须先查长度再动态分配，防止栈溢出
static std::wstring GetSelectedDictName(CListBox& list, int sel) {
  int len = list.GetTextLen(sel);
  if (len == LB_ERR)
    return std::wstring();
  std::vector<wchar_t> buf(len + 1, L'\0');
  if (list.GetText(sel, buf.data()) == LB_ERR)
    return std::wstring();
  return std::wstring(buf.data());
}

LRESULT DictManagementDialog::OnBackup(WORD, WORD code, HWND, BOOL&) {
  int sel = user_dict_list_.GetCurSel();
  if (sel < 0 || sel >= user_dict_list_.GetCount()) {
    MSG_BY_IDS(IDS_STR_SEL_EXPORT_DICT_NAME, IDS_STR_SAD,
               MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  std::wstring path;
  {
    char dir[MAX_PATH] = {0};
    rime_get_api()->get_user_data_sync_dir(dir, _countof(dir));
    // rime 返回的同步目录为 UTF-8 编码，CP_ACP 解码必乱（同 A16）
    path = u8tow(dir);
  }
  if (_waccess_s(path.c_str(), 0) != 0 &&
      !CreateDirectoryW(path.c_str(), NULL) &&
      GetLastError() == ERROR_PATH_NOT_FOUND) {
    MSG_BY_IDS(IDS_STR_ERREXPORT_SYNC_UV, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    return 0;
  }
  std::wstring dict_name = GetSelectedDictName(user_dict_list_, sel);
  path += std::wstring(L"\\") + dict_name + L".userdb.txt";
  std::string dict_name_str = wtou8(dict_name);
  if (!api_->backup_user_dict(dict_name_str.c_str())) {
    MSG_BY_IDS(IDS_STR_ERR_EXPORT_UNKNOWN, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    return 0;
  } else if (_waccess(path.c_str(), 0) != 0) {
    MSG_BY_IDS(IDS_STR_ERR_EXPORT_SNAP_LOST, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    return 0;
  }
  OpenFolderAndSelectItem(path);
  return 0;
}

LRESULT DictManagementDialog::OnRestore(WORD, WORD code, HWND, BOOL&) {
  CString open_str, dict_snapshot_str, kcss_dict_snapshot_str, all_files_str;
  open_str.LoadStringW(IDS_STR_OPEN);
  dict_snapshot_str.LoadStringW(IDS_STR_DICT_SNAPSHOT);
  kcss_dict_snapshot_str.LoadStringW(IDS_STR_KCSS_DICT_SNAPSHOT);
  all_files_str.LoadStringW(IDS_STR_ALL_FILES);

  const std::wstring dict_snapshot_name =
      dict_snapshot_str + L" (*.userdb.txt)";
  const std::wstring kcss_dict_snapshot_name =
      kcss_dict_snapshot_str + L" (*.userdb.kct.snapshot)";
  const std::wstring all_files_name = all_files_str;

  COMDLG_FILTERSPEC filter[3] = {
      {dict_snapshot_name.c_str(), L"*.userdb.txt"},
      {kcss_dict_snapshot_name.c_str(), L"*.userdb.kct.snapshot"},
      {all_files_name.c_str(), L"*.*"}};

  std::wstring selected_path = DoFileDialog<IFileOpenDialog, FileOpenDialog>(
      m_hWnd, open_str, ARRAYSIZE(filter), filter, NULL, L"snapshot");
  if (!selected_path.empty()) {
    char path[MAX_PATH] = {0};
    WideCharToMultiByte(CP_UTF8, 0, selected_path.c_str(), -1, path,
                        _countof(path), NULL, NULL);
    if (!api_->restore_user_dict(path)) {
      MSG_BY_IDS(IDS_STR_ERR_UNKNOWN, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    } else {
      Populate();  // 词典内容已变，重载列表反映恢复结果
      MSG_BY_IDS(IDS_STR_ERR_SUCCESS, IDS_STR_HAPPY,
                 MB_OK | MB_ICONINFORMATION);
    }
  }
  return 0;
}

LRESULT DictManagementDialog::OnExport(WORD, WORD code, HWND, BOOL&) {
  CString save_as_str, exported_str, record_count_str, all_files_str,
      txt_files_str;
  save_as_str.LoadStringW(IDS_STR_SAVE_AS);
  exported_str.LoadStringW(IDS_STR_EXPORTED);
  record_count_str.LoadStringW(IDS_STR_RECORD_COUNT);
  txt_files_str.LoadStringW(IDS_STR_TXT_FILES);
  all_files_str.LoadStringW(IDS_STR_ALL_FILES);
  const std::wstring txt_files_name = txt_files_str + L" (*.txt)";
  const std::wstring all_files_name = all_files_str;

  int sel = user_dict_list_.GetCurSel();
  if (sel < 0 || sel >= user_dict_list_.GetCount()) {
    MSG_BY_IDS(IDS_STR_SEL_EXPORT_DICT_NAME, IDS_STR_SAD,
               MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  std::wstring dict_name = GetSelectedDictName(user_dict_list_, sel);
  std::wstring file_name = dict_name + L"_export.txt";

  COMDLG_FILTERSPEC filter[2] = {{txt_files_name.c_str(), L"*.txt"},
                                 {all_files_name.c_str(), L"*.*"}};

  OutputDebugString(filter[0].pszName);
  std::wstring selected_path = DoFileDialog<IFileSaveDialog, FileSaveDialog>(
      m_hWnd, save_as_str, ARRAYSIZE(filter), filter, file_name.c_str(),
      L"txt");
  if (!selected_path.empty()) {
    char path[MAX_PATH] = {0};
    WideCharToMultiByte(CP_UTF8, 0, selected_path.c_str(), -1, path,
                        _countof(path), NULL, NULL);
    std::string dict_name_str = wtou8(dict_name);
    int result = api_->export_user_dict(dict_name_str.c_str(), path);
    if (result < 0) {
      MSG_BY_IDS(IDS_STR_ERR_UNKNOWN, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    } else if (_waccess(selected_path.c_str(), 0) != 0) {
      MSG_BY_IDS(IDS_STR_ERR_EXPORT_FILE_LOST, IDS_STR_SAD,
                 MB_OK | MB_ICONERROR);
    } else {
      std::wstring report(std::wstring(exported_str) + L" " +
                          std::to_wstring(result) + L" " +
                          std::wstring(record_count_str));
      MSG_ID_CAP(report.c_str(), IDS_STR_HAPPY, MB_OK | MB_ICONINFORMATION);
      OpenFolderAndSelectItem(selected_path);
    }
  }
  return 0;
}

LRESULT DictManagementDialog::OnImport(WORD, WORD code, HWND, BOOL&) {
  CString open_str, imported_str, record_count_str, all_files_str,
      txt_files_str;
  open_str.LoadStringW(IDS_STR_OPEN);
  imported_str.LoadStringW(IDS_STR_IMPORTED);
  record_count_str.LoadStringW(IDS_STR_RECORD_COUNT);
  txt_files_str.LoadStringW(IDS_STR_TXT_FILES);
  all_files_str.LoadStringW(IDS_STR_ALL_FILES);
  const std::wstring txt_files_name = txt_files_str + L" (*.txt)";
  const std::wstring all_files_name = all_files_str;

  int sel = user_dict_list_.GetCurSel();
  if (sel < 0 || sel >= user_dict_list_.GetCount()) {
    MSG_BY_IDS(IDS_STR_SEL_IMPORT_DICT_NAME, IDS_STR_SAD,
               MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  std::wstring dict_name = GetSelectedDictName(user_dict_list_, sel);
  std::wstring file_name = dict_name + L"_export.txt";

  COMDLG_FILTERSPEC filter[2] = {{txt_files_name.c_str(), L"*.txt"},
                                 {all_files_name.c_str(), L"*.*"}};

  OutputDebugString(filter[0].pszName);
  std::wstring selected_path = DoFileDialog<IFileOpenDialog, FileOpenDialog>(
      m_hWnd, open_str, ARRAYSIZE(filter), filter, file_name.c_str(), L"txt");
  if (!selected_path.empty()) {
    char path[MAX_PATH] = {0};
    WideCharToMultiByte(CP_UTF8, 0, selected_path.c_str(), -1, path,
                        _countof(path), NULL, NULL);
    int result = api_->import_user_dict(wtou8(dict_name).c_str(), path);
    if (result < 0) {
      MSG_BY_IDS(IDS_STR_ERR_UNKNOWN, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    } else {
      std::wstring report(std::wstring(imported_str) + L" " +
                          std::to_wstring(result) + L" " +
                          std::wstring(record_count_str));
      MSG_ID_CAP(report.c_str(), IDS_STR_HAPPY, MB_OK | MB_ICONINFORMATION);
    }
  }
  return 0;
}

LRESULT DictManagementDialog::OnUserDictListSelChange(WORD, WORD, HWND, BOOL&) {
  int index = user_dict_list_.GetCurSel();
  BOOL enabled = index < 0 ? FALSE : TRUE;
  backup_.EnableWindow(enabled);
  export_.EnableWindow(enabled);
  import_.EnableWindow(enabled);
  return 0;
}
