#pragma once

#include "Globals.h"
#include <WeaselIPC.h>
#include <WeaselIPCData.h>
#include <ResponseParser.h>

class CCandidateList;
class CLangBarItemButton;
class CCompartmentEventSink;

class WeaselTSF : public ITfTextInputProcessorEx,
                  public ITfThreadMgrEventSink,
                  public ITfTextEditSink,
                  public ITfTextLayoutSink,
                  public ITfKeyEventSink,
                  public ITfCompositionSink,
                  public ITfThreadFocusSink,
                  public ITfActiveLanguageProfileNotifySink,
                  public ITfDisplayAttributeProvider {
 public:
  WeaselTSF();
  ~WeaselTSF();

  /* IUnknown */
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject);
  STDMETHODIMP_(ULONG) AddRef();
  STDMETHODIMP_(ULONG) Release();

  /* ITfTextInputProcessor */
  STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId);
  STDMETHODIMP Deactivate();

  /* ITfTextInputProcessorEx */
  STDMETHODIMP ActivateEx(ITfThreadMgr* pThreadMgr,
                          TfClientId tfClientId,
                          DWORD dwFlags);

  /* ITfThreadMgrEventSink */
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr);
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr);
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                          ITfDocumentMgr* pDocMgrPrevFocus);
  STDMETHODIMP OnPushContext(ITfContext* pContext);
  STDMETHODIMP OnPopContext(ITfContext* pContext);

  /* ITfTextEditSink */
  STDMETHODIMP OnEndEdit(ITfContext* pic,
                         TfEditCookie ecReadOnly,
                         ITfEditRecord* pEditRecord);

  /* ITfTextLayoutSink */
  STDMETHODIMP OnLayoutChange(ITfContext* pContext,
                              TfLayoutCode lcode,
                              ITfContextView* pContextView);

  /* ITfKeyEventSink */
  STDMETHODIMP OnSetFocus(BOOL fForeground);
  STDMETHODIMP OnTestKeyDown(ITfContext* pContext,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL* pfEaten);
  STDMETHODIMP OnKeyDown(ITfContext* pContext,
                         WPARAM wParam,
                         LPARAM lParam,
                         BOOL* pfEaten);
  STDMETHODIMP OnTestKeyUp(ITfContext* pContext,
                           WPARAM wParam,
                           LPARAM lParam,
                           BOOL* pfEaten);
  STDMETHODIMP OnKeyUp(ITfContext* pContext,
                       WPARAM wParam,
                       LPARAM lParam,
                       BOOL* pfEaten);
  STDMETHODIMP OnPreservedKey(ITfContext* pContext,
                              REFGUID rguid,
                              BOOL* pfEaten);

  // ITfThreadFocusSink
  STDMETHODIMP OnSetThreadFocus();
  STDMETHODIMP OnKillThreadFocus();

  /* ITfCompositionSink */
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite,
                                       ITfComposition* pComposition);

  /* ITfActiveLanguageProfileNotifySink */
  STDMETHODIMP OnActivated(REFCLSID clsid,
                           REFGUID guidProfile,
                           BOOL isActivated);

  // ITfDisplayAttributeProvider
  STDMETHODIMP EnumDisplayAttributeInfo(
      __RPC__deref_out_opt IEnumTfDisplayAttributeInfo** ppEnum);
  STDMETHODIMP GetDisplayAttributeInfo(
      __RPC__in REFGUID guidInfo,
      __RPC__deref_out_opt ITfDisplayAttributeInfo** ppInfo);

  ///* ITfCompartmentEventSink */
  // STDMETHODIMP OnChange(_In_ REFGUID guid);

  /* Compartments */
  BOOL _IsKeyboardDisabled();
  BOOL _IsKeyboardOpen();
  HRESULT _SetKeyboardOpen(BOOL fOpen);
  // 在 OnChange 通知内对同一 compartment 的 SetValue 会被 TSF 以
  // E_UNEXPECTED 拒绝（MSDN《ITfCompartment::SetValue》），强制重开须延迟
  // 到通知返回之后：_RequestKeyboardOpenDeferred 借异步编辑会话做延迟
  // 载体，会话内执行 _ApplyDeferredKeyboardOpen。自写同步触发的下一次
  // OnChange 由 _fSuppressOpenCloseSelfWrite 跳过，避免 ascii_mode 被
  // 强制重开翻转回去
  void _RequestKeyboardOpenDeferred();
  void _ApplyDeferredKeyboardOpen();
  HRESULT _GetCompartmentDWORD(DWORD& value, const GUID guid);
  HRESULT _SetCompartmentDWORD(const DWORD& value, const GUID guid);

  /* Composition */
  void _StartComposition(com_ptr<ITfContext> pContext,
                         BOOL fCUASWorkaroundEnabled);
  void _EndComposition(com_ptr<ITfContext> pContext,
                       BOOL clear,
                       BOOL endUI = TRUE);
  BOOL _ShowInlinePreedit(com_ptr<ITfContext> pContext,
                          const std::shared_ptr<weasel::Context> context);
  void _UpdateComposition(com_ptr<ITfContext> pContext);
  bool _ConsumeResponse(LPWSTR buffer, DWORD length);
  void _ConsumeResponseIfFresh();
  BOOL _IsComposing();
  BOOL _IsCurrentComposition(ITfComposition* pComposition);
  // D3：把已解析的服务器应答应用到文档。由 CUpdateCompositionEditSession
  // （见 EditSession.h）在会话执行时回交构造时捕获的 commit 与目标
  // 上下文；会话不再读 _pendingCommit/_pEditSessionContext 成员快照，
  // 被宿主延迟的 TF_ES_ASYNCDONTCARE 会话不会拿到延迟窗口内新键/新文档
  // 覆盖后的值
  STDMETHODIMP _ApplyCompositionUpdate(TfEditCookie ec,
                                       com_ptr<ITfContext> pContext,
                                       std::wstring commit);
  void _SetComposition(com_ptr<ITfComposition> pComposition);
  void _SetCompositionPosition(const RECT& rc);
  BOOL _UpdateCompositionWindow(com_ptr<ITfContext> pContext);
  void _FinalizeComposition();
  void _AbortComposition(bool clear = true);

  /* Language bar */
  HWND _GetFocusedContextWindow();
  void _HandleLangBarMenuSelect(UINT wID);

  /* IPC */
  bool _EnsureServerConnected();

  /* UI */
  void _UpdateUI(const weasel::Context& ctx, const weasel::Status& status);
  void _StartUI();
  void _EndUI();
  void _ShowUI();
  void _HideUI();
  com_ptr<ITfContext> _GetUIContextDocument();

  /* Display Attribute */
  void _ClearCompositionDisplayAttributes(TfEditCookie ec,
                                          _In_ ITfContext* pContext,
                                          _In_opt_ ITfComposition* pComposition);
  BOOL _SetCompositionDisplayAttributes(TfEditCookie ec,
                                        _In_ ITfContext* pContext,
                                        ITfRange* pRangeComposition);
  BOOL _InitDisplayAttributeGuidAtom();

  com_ptr<ITfThreadMgr> _GetThreadMgr() { return _pThreadMgr; }
  void HandleUICallback(size_t* const sel,
                        size_t* const hov,
                        bool* const next,
                        bool* const scroll_next);

 private:
  /* ui callback functions private */
  void _SelectCandidateOnCurrentPage(const size_t index);
  void _HandleMouseHoverEvent(const size_t index);
  void _HandleMousePageEvent(bool* const nextPage, bool* const scrollNextPage);
  /* TSF Related */
  BOOL _InitThreadMgrEventSink();
  void _UninitThreadMgrEventSink();
  // ITfThreadFocusSink
  BOOL _InitThreadFocusSink();
  void _UninitThreadFocusSink();
  DWORD _dwThreadFocusSinkCookie;

  BOOL _InitTextEditSink(com_ptr<ITfDocumentMgr> pDocMgr);

  BOOL _InitKeyEventSink();
  void _UninitKeyEventSink();
  void _ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten);

  BOOL _InitPreservedKey();
  void _UninitPreservedKey();

  BOOL _InitLanguageBar();
  void _UninitLanguageBar();
  void _UpdateLanguageBar(weasel::Status stat);
  void _ShowLanguageBar(BOOL show);
  void _EnableLanguageBar(BOOL enable);

  BOOL _InsertText(com_ptr<ITfContext> pContext, const std::wstring& ext);

  void _DeleteCandidateList();

  BOOL _InitCompartment();
  void _UninitCompartment();
  HRESULT _HandleCompartment(REFGUID guidCompartment);

  // K21：ToggleImeOnOpenClose 注册表值仅 WeaselSetup 写入，按 TTL 限频
  // 重读，免去每次线程焦点切换的注册表访问
  void _RefreshToggleImeOnOpenClose();
  ULONGLONG _toggleImeReadTick = 0;

  // K23c：_IsKeyboardDisabled 的禁用态缓存。KEYBOARD_DISABLED/
  // EMPTYCONTEXT 是 context 级 compartment，sink 挂在焦点 top context
  // 上，值变化置脏；context 变化（含 push/pop 改变 GetTop）时按身份
  // 比对重挂。advise 失败则不缓存，退回逐键查询
  HRESULT _OnKeyboardDisabledCompartmentChange(REFGUID guidCompartment);
  void _RetargetKeyboardDisabledSinks(com_ptr<ITfContext> pContext);

  void _Reconnect();
  std::wstring _GetRootDir();
  // consecutive failed reconnect attempts (per instance, was a shared
  // file-static raced across threads and TIP instances)
  unsigned int _reconnectRetry = 0;

  bool isImmersive() const {
    return (_activateFlags & TF_TMF_IMMERSIVEMODE) != 0;
  }

  com_ptr<ITfThreadMgr> _pThreadMgr;
  TfClientId _tfClientId;
  DWORD _dwThreadMgrEventSinkCookie;

  com_ptr<ITfContext> _pTextEditSinkContext;
  DWORD _dwTextEditSinkCookie, _dwTextLayoutSinkCookie;
  BYTE _lpbKeyState[256];
  // OnTestKeyDown/OnTestKeyUp 去重状态：pending 表示该键已送服务器、等待配对
  // 的 OnKeyDown/OnKeyUp。键身份只记虚键码：配对回调间 lParam 无契约保证
  // （CUAS/IMM32 兼容桥下 Test 与 Key 的 repeat count/previous state 等位可
  // 不同，如 Qt 应用），且虚键码已是本组件消费的全部键身份；不同键到来即开
  // 新周期，吞掉配对回调的残留 pending 不会吞掉后续键
  struct PendingTestKey {
    BOOL pending = FALSE;
    UINT vkey = 0;
  } _testKeyDownPending, _testKeyUpPending;
  // Caps Lock 模拟的跨按键状态。键事件 sink 按 thread manager 各持一份，
  // 实例化即按线程隔离（原文件级 static 被同进程多实例/多线程共享）
  weasel::KeyEvent _prevKeyEvent;
  BOOL _prevfEaten = FALSE;
  int _keyCountToSimulate = 0;

  // 最近一次键事件/组合更新请求的目标上下文。D3 后不再充当编辑会话的
  // 快照（应用会话 CUpdateCompositionEditSession 自带上下文与 commit），
  // 仅供无自带上下文的路径复用：UI 回调（候选选择/翻页/悬停）的
  // _UpdateComposition 调用、_AbortComposition 的组合收尾、
  // _RequestKeyboardOpenDeferred 的兜底
  com_ptr<ITfContext> _pEditSessionContext;

  com_ptr<CCompartmentEventSink> _pKeyboardCompartmentSink;
  com_ptr<CCompartmentEventSink> _pConvertionCompartmentSink;

  // 自写 OPENCLOSE 的抑制标志：_ApplyDeferredKeyboardOpen 置位期间，
  // _HandleCompartment 跳过该次自写触发的通知（见 Compartments 注释）
  BOOL _fSuppressOpenCloseSelfWrite = FALSE;

  // 禁用态缓存（K23c）：_pDisabledCacheContext 为空表示不缓存
  com_ptr<CCompartmentEventSink> _pKeyboardDisabledSink;
  com_ptr<CCompartmentEventSink> _pEmptyContextSink;
  com_ptr<ITfContext> _pDisabledCacheContext;
  BOOL _fKeyboardDisabled = FALSE;
  BOOL _fKeyboardDisabledDirty = TRUE;

  com_ptr<ITfComposition> _pComposition;

  com_ptr<CLangBarItemButton> _pLangBarButton;

  com_ptr<CCandidateList> _cand;

  LONG _cRef;  // COM ref count

  /* CUAS Candidate Window Position Workaround */
  BOOL _fCUASWorkaroundTested, _fCUASWorkaroundEnabled;

  /* Weasel Related */
  weasel::Client m_client;
  DWORD _activateFlags;

  /* IME status */
  weasel::Status _status;

  // 上次成功写入 INPUTMODE_CONVERSION compartment 的值；稳态（连续击键
  // 状态不变）据此跳过 SetValue。Deactivate 时失效
  DWORD _conversionFlags = 0;
  bool _conversionFlagsValid = false;

  /* 已解析、待编辑会话应用的服务器应答；解析发生在按键/UI 回调线程内
   * （应答缓冲在下一次 Transact 前有效），会话可能异步排队、晚于解析。
   * D3：_UpdateComposition 构造会话对象时即 std::move 整体取走，本成员
   * 只是解析线程到会话构造之间的暂存；被延迟的会话拿到的永远是构造
   * 时的快照，延迟窗口内的新键不会再串写入旧会话 */
  std::wstring _pendingCommit;  // commit 是事件流：应用前按序累积
  std::shared_ptr<weasel::Context> _context =
      std::make_shared<weasel::Context>();
  weasel::Config _config;
  UINT64 _parsedSerial = 0;
  // 复用的响应解析器：deserializer 注册表只建一次（Require 幂等），
  // 每次解析前重指目标；目标指针仅在 _ConsumeResponse 执行期间有效
  weasel::ResponseParser _parser{nullptr, nullptr, nullptr, nullptr,
                                 nullptr};

  // guidatom for the display attibute. 0 = RegisterGUID failed or not yet
  // run; guards the SetValue in _SetCompositionDisplayAttributes.
  TfGuidAtom _gaDisplayAttributeInput = 0;
  BOOL _committed = false;
  BOOL _isToOpenClose = false;
};
