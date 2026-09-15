#include "stdafx.h"
#include "SwitcherSettingsDialog.h"
#include "Configurator.h"
#include <algorithm>
#include <set>
#include <vector>
#include <rime_levers_api.h>
#include <WeaselUtility.h>
#include "WeaselDeployer.h"

SwitcherSettingsDialog::SwitcherSettingsDialog(RimeSwitcherSettings* settings)
    : settings_(settings), loaded_(false), modified_(false) {
  api_ = (RimeLeversApi*)rime_get_api()->find_module("levers")->get_api();
}

SwitcherSettingsDialog::~SwitcherSettingsDialog() {}

void SwitcherSettingsDialog::Populate() {
  if (!settings_)
    return;
  RimeSchemaList available = {0};
  api_->get_available_schema_list(settings_, &available);
  RimeSchemaList selected = {0};
  api_->get_selected_schema_list(settings_, &selected);
  schema_list_.DeleteAllItems();
  size_t k = 0;
  std::set<RimeSchemaInfo*> recruited;
  for (size_t i = 0; i < selected.size; ++i) {
    const char* schema_id = selected.list[i].schema_id;
    for (size_t j = 0; j < available.size; ++j) {
      RimeSchemaListItem& item(available.list[j]);
      RimeSchemaInfo* info = (RimeSchemaInfo*)item.reserved;
      if (!strcmp(item.schema_id, schema_id) &&
          recruited.find(info) == recruited.end()) {
        recruited.insert(info);
        std::wstring itemwstr = u8tow(item.name);
        schema_list_.AddItem(k, 0, itemwstr.c_str());
        schema_list_.SetItemData(k, (DWORD_PTR)info);
        schema_list_.SetCheckState(k, TRUE);
        ++k;
        break;
      }
    }
  }
  for (size_t i = 0; i < available.size; ++i) {
    RimeSchemaListItem& item(available.list[i]);
    RimeSchemaInfo* info = (RimeSchemaInfo*)item.reserved;
    if (recruited.find(info) == recruited.end()) {
      recruited.insert(info);
      std::wstring itemwstr = u8tow(item.name);
      schema_list_.AddItem(k, 0, itemwstr.c_str());
      schema_list_.SetItemData(k, (DWORD_PTR)info);
      ++k;
    }
  }
  // schema_id/name/reserved 均借用 SwitcherSettings 内部数据，条目数组本身
  // 需由调用方释放；对未填充（失败）或已销毁的列表重复调用是安全的空操作。
  api_->schema_list_destroy(&available);
  api_->schema_list_destroy(&selected);
  auto hotkeys_str = api_->get_hotkeys(settings_);
  if (hotkeys_str) {
    std::wstring txt = u8tow(hotkeys_str);
    hotkeys_.SetWindowTextW(txt.c_str());
  }
  loaded_ = true;
  modified_ = false;
}

void SwitcherSettingsDialog::ShowDetails(RimeSchemaInfo* info) {
  if (!info)
    return;
  std::string details;
  if (const char* name = api_->get_schema_name(info)) {
    details += name;
  }
  if (const char* author = api_->get_schema_author(info)) {
    (details += "\n\n") += author;
  }
  if (const char* description = api_->get_schema_description(info)) {
    (details += "\n\n") += description;
  }
  std::wstring txt = u8tow(details.c_str());
  description_.SetWindowTextW(txt.c_str());
}

LRESULT SwitcherSettingsDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  schema_list_.SubclassWindow(GetDlgItem(IDC_SCHEMA_LIST));
  schema_list_.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT,
                                        LVS_EX_FULLROWSELECT);

  CString schema_name;
  schema_name.LoadStringW(IDS_STR_SCHEMA_NAME);
  schema_list_.AddColumn(schema_name, 0);
  CRect rc;
  schema_list_.GetClientRect(&rc);
  schema_list_.SetColumnWidth(0, rc.Width() - 20);

  description_.Attach(GetDlgItem(IDC_SCHEMA_DESCRIPTION));

  hotkeys_.Attach(GetDlgItem(IDC_HOTKEYS));
  hotkeys_.EnableWindow(FALSE);

  get_schemata_.Attach(GetDlgItem(IDC_GET_SCHEMATA));
  get_schemata_.EnableWindow(TRUE);

  Populate();
  InitCtrlRects();

  CenterWindow();
  BringWindowToTop();
  return TRUE;
}

LRESULT SwitcherSettingsDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

// rime-install.bat 的最长等待：超时后放弃等待并继续，避免卡死 UI 线程
static constexpr DWORD kInstallSchemataTimeoutMs = 60u * 1000;

LRESULT SwitcherSettingsDialog::OnGetSchemata(WORD, WORD, HWND hWndCtl, BOOL&) {
  LPCWSTR hPath = is_wow64() ? L"Software\\WOW6432Node\\Rime\\Weasel"
                             : L"Software\\Rime\\Weasel";
  // RegGetStringValue 内部走 RegGetValueW，保证结果以 NUL 结尾
  std::wstring weaselRoot;
  if (RegGetStringValue(HKEY_LOCAL_MACHINE, hPath, L"WeaselRoot",
                        weaselRoot) != ERROR_SUCCESS) {
    LOG(ERROR) << "Error reading registry value WeaselRoot.";
    return 0;
  }
  WCHAR parameters[MAX_PATH + 37];
  wcscpy_s<_countof(parameters)>(
      parameters,
      (L"/k \"" + weaselRoot + L"\\rime-install.bat\"").c_str());
  SHELLEXECUTEINFOW cmd = {sizeof(SHELLEXECUTEINFO),
                           SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
                           hWndCtl,
                           L"open",
                           L"cmd",
                           parameters,
                           NULL,
                           SW_SHOW,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           NULL};
  if (!ShellExecuteExW(&cmd)) {
    LOG(ERROR) << "Error launching rime-install.bat.";
    return 0;
  }
  if (cmd.hProcess) {
    if (WaitForSingleObject(cmd.hProcess, kInstallSchemataTimeoutMs) !=
        WAIT_OBJECT_0) {
      LOG(WARNING) << "Timed out waiting for rime-install.bat; continuing.";
    }
    CloseHandle(cmd.hProcess);
  }
  api_->load_settings(reinterpret_cast<RimeCustomSettings*>(settings_));
  Populate();
  return 0;
}

LRESULT SwitcherSettingsDialog::OnOK(WORD, WORD code, HWND, BOOL&) {
  if (modified_ && settings_ && schema_list_.GetItemCount() != 0) {
    std::vector<const char*> selection;
    selection.reserve(schema_list_.GetItemCount());
    for (int i = 0; i < schema_list_.GetItemCount(); ++i) {
      if (!schema_list_.GetCheckState(i))
        continue;
      RimeSchemaInfo* info = (RimeSchemaInfo*)(schema_list_.GetItemData(i));
      const char* schema_id = info ? api_->get_schema_id(info) : NULL;
      if (schema_id)
        selection.push_back(schema_id);
    }
    if (selection.empty()) {
      // MessageBox(_T("至少要選用一項吧。"), _T("小狼毫不是這般用法"), MB_OK |
      // MB_ICONEXCLAMATION);
      MSG_BY_IDS(IDS_STR_ERR_AT_LEAST_ONE_SEL, IDS_STR_NOT_REGULAR,
                 MB_OK | MB_ICONEXCLAMATION);
      return 0;
    }
    api_->select_schemas(settings_, selection.data(), (int)selection.size());
  }
  EndDialog(code);
  return 0;
}

LRESULT SwitcherSettingsDialog::OnSchemaListItemChanged(int, LPNMHDR p, BOOL&) {
  LPNMLISTVIEW lv = reinterpret_cast<LPNMLISTVIEW>(p);
  if (!loaded_ || !lv || lv->iItem < 0 ||
      lv->iItem >= schema_list_.GetItemCount())
    return 0;
  if ((lv->uNewState & LVIS_STATEIMAGEMASK) !=
      (lv->uOldState & LVIS_STATEIMAGEMASK)) {
    modified_ = true;
  } else if ((lv->uNewState & LVIS_SELECTED) &&
             !(lv->uOldState & LVIS_SELECTED)) {
    ShowDetails((RimeSchemaInfo*)(schema_list_.GetItemData(lv->iItem)));
  }
  return 0;
}
