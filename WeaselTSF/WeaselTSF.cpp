#include "stdafx.h"

#include <WeaselIPCData.h>
#include <thread>
#include <shellapi.h>
#include <tlhelp32.h>
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "LanguageBar.h"
#include "Compartment.h"

WeaselTSF::WeaselTSF() {
  _cRef = 1;

  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;

  _dwTextEditSinkCookie = TF_INVALID_COOKIE;
  _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
  _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;

  _fCUASWorkaroundTested = _fCUASWorkaroundEnabled = FALSE;

  // Attach (no AddRef): ~WeaselTSF's CComPtr release must be able to drop
  // the reference count to zero (a plain assignment would leak one).
  _cand.Attach(new CCandidateList(this));

  DllAddRef();
}

WeaselTSF::~WeaselTSF() {
  DllRelease();
}

STDMETHODIMP WeaselTSF::QueryInterface(REFIID riid, void** ppvObject) {
  if (ppvObject == NULL)
    return E_INVALIDARG;

  *ppvObject = NULL;

  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppvObject = (ITfTextInputProcessor*)this;
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppvObject = (ITfTextInputProcessorEx*)this;
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppvObject = (ITfThreadMgrEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextEditSink))
    *ppvObject = (ITfTextEditSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
    *ppvObject = (ITfTextLayoutSink*)this;
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppvObject = (ITfKeyEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppvObject = (ITfCompositionSink*)this;
  else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    *ppvObject = (ITfThreadFocusSink*)this;
  else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    *ppvObject = (ITfDisplayAttributeProvider*)this;

  if (*ppvObject) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) WeaselTSF::AddRef() {
  return ++_cRef;
}

STDMETHODIMP_(ULONG) WeaselTSF::Release() {
  LONG cr = --_cRef;

  assert(_cRef >= 0);

  if (_cRef == 0)
    delete this;

  return cr;
}

STDMETHODIMP WeaselTSF::Activate(ITfThreadMgr* pThreadMgr,
                                 TfClientId tfClientId) {
  return ActivateEx(pThreadMgr, tfClientId, 0U);
}

STDMETHODIMP WeaselTSF::Deactivate() {
  m_client.EndSession();

  _InitTextEditSink(com_ptr<ITfDocumentMgr>());

  _UninitThreadMgrEventSink();

  _UninitKeyEventSink();
  _UninitPreservedKey();

  _UninitLanguageBar();

  _UninitCompartment();

  _UninitThreadFocusSink();

  // 停用期间 INPUTMODE_CONVERSION 可能被宿主/其他输入源改写，
  // 上次写入值缓存不再可信
  _conversionFlagsValid = false;

  // While the thread manager is still valid: DestroyAll ends the UIElement,
  // which makes the UIElementMgr drop its reference to the candidate list.
  _cand->DestroyAll();

  _pThreadMgr = NULL;

  _tfClientId = TF_CLIENTID_NULL;

  return S_OK;
}

STDMETHODIMP WeaselTSF::ActivateEx(ITfThreadMgr* pThreadMgr,
                                   TfClientId tfClientId,
                                   DWORD dwFlags) {
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  _activateFlags = dwFlags;

  _pThreadMgr = pThreadMgr;
  _tfClientId = tfClientId;

  if (!_InitThreadMgrEventSink())
    goto ExitError;

  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
      (pDocMgrFocus != NULL)) {
    _InitTextEditSink(pDocMgrFocus);
  }

  if (!_InitKeyEventSink())
    goto ExitError;

  // Init failure is non-fatal: some apps don't provide DisplayAttributeInfo
  // (e.g. OpenGL stuff). The atom stays 0 and the attribute setter skips the
  // write, so no bogus atom reaches GUID_PROP_ATTRIBUTE.
  _InitDisplayAttributeGuidAtom();

  if (!_InitPreservedKey())
    goto ExitError;

  if (!_InitLanguageBar())
    goto ExitError;

  if (!_IsKeyboardOpen())
    _SetKeyboardOpen(TRUE);

  if (!_InitCompartment())
    goto ExitError;
  if (!_InitThreadFocusSink())
    goto ExitError;

  _EnsureServerConnected();

  return S_OK;

ExitError:
  Deactivate();
  return E_FAIL;
}

// 该注册表值仅 WeaselSetup（安装/设置）写入，变更极罕见；限频重读，
// 外部改动最迟 TTL 后的下一次焦点切换生效，与原逐次读取行为一致
static const ULONGLONG kToggleImeRefreshIntervalMs = 10 * 1000;

void WeaselTSF::_RefreshToggleImeOnOpenClose() {
  ULONGLONG now = GetTickCount64();
  if (_toggleImeReadTick != 0 &&
      now - _toggleImeReadTick < kToggleImeRefreshIntervalMs)
    return;
  _toggleImeReadTick = now;
  std::wstring value;
  RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                    L"ToggleImeOnOpenClose", value);
  _isToOpenClose = (value == L"yes");
}

STDMETHODIMP WeaselTSF::OnSetThreadFocus() {
  _RefreshToggleImeOnOpenClose();
  // Echo + ProcessKeyEvent(0) 两轮 IPC 为承重路径，保留（K21 考证）：
  // - Echo 是会话有效性闸门（服务端 FindSession）。若省掉而直接发
  //   ProcessKeyEvent(0)，会话已失效时服务端会对孤儿会话跑 process_key
  //   并把 m_active_session 指向死会话 id，改变服务端可见行为。
  // - ProcessKeyEvent(0)（keycode=0，librime 各 processor 均不处理）的
  //   作用是借 _Respond 拿回全量 status 快照：tray 菜单的中/英切换（作用于
  //   服务端 m_active_session）与 global_ascii_mode 的跨会话联动都可能
  //   在本客户端无通知的情况下改变本会话状态，焦点切换时须重新同步
  //   _status 与语言栏图标，省掉会让图标陈旧至下一次击键。
  if (m_client.Echo()) {
    m_client.ProcessKeyEvent(0);
    _ConsumeResponseIfFresh();
  }
  return S_OK;
}
STDMETHODIMP WeaselTSF::OnKillThreadFocus() {
  _AbortComposition();
  return S_OK;
}
BOOL WeaselTSF::_InitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return FALSE;
  if (FAILED(pSource->AdviseSink(IID_ITfThreadFocusSink,
                                 (ITfThreadFocusSink*)this,
                                 &_dwThreadFocusSinkCookie)))
    return FALSE;
  return TRUE;
}
void WeaselTSF::_UninitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return;
  if (FAILED(pSource->UnadviseSink(_dwThreadFocusSinkCookie)))
    return;
}

STDMETHODIMP WeaselTSF::OnActivated(REFCLSID clsid,
                                    REFGUID guidProfile,
                                    BOOL isActivated) {
  if (!IsEqualCLSID(clsid, c_clsidTextService)) {
    return S_OK;
  }

  if (isActivated) {
    _ShowLanguageBar(TRUE);
    _UpdateLanguageBar(_status);
  } else {
    _DeleteCandidateList();
    _ShowLanguageBar(FALSE);
  }
  return S_OK;
}

void WeaselTSF::_Reconnect() {
  m_client.Disconnect();
  m_client.Connect();
  m_client.StartSession();
  _ConsumeResponseIfFresh();
}

static int count_server_process() {
  int count = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);
  if (Process32First(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"WeaselServer.exe") == 0)
        count++;
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return count;
}

bool WeaselTSF::_EnsureServerConnected() {
  // Trust an established session and skip the Echo roundtrip per key: a
  // dead server breaks the pipe, the failed transact invalidates the
  // session on the client, and only then do we pay for a full reconnect.
  if (m_client.IsSessionActive())
    return true;

  if (!m_client.Echo()) {
    _Reconnect();
    if (!m_client.IsSessionActive()) {
      if (++_reconnectRetry >= 6) {
        HANDLE hMutex =
            CreateMutex(NULL, TRUE, L"WeaselDeployerExclusiveMutex");
        // Read GetLastError right after CreateMutex: any later pipe call
        // would overwrite the ERROR_ALREADY_EXISTS indicator.
        bool alreadyLaunching = (GetLastError() == ERROR_ALREADY_EXISTS);
        if (!alreadyLaunching && count_server_process() == 0) {
          std::wstring dir = _GetRootDir();
          // The detached thread must not capture this: it can outlive the
          // TIP object while the host app tears down. It only starts the
          // service; the next keystroke's reconnect path completes login.
          std::thread th([dir]() {
            ShellExecuteW(NULL, L"open", (dir + L"\\start_service.bat").c_str(),
                          NULL, dir.c_str(), SW_HIDE);
          });
          th.detach();
        }
        if (hMutex) {
          CloseHandle(hMutex);
        }
        _reconnectRetry = 0;
      }
      return false;
    }
    _reconnectRetry = 0;
    return true;
  }
  return true;
}
