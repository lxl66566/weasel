#include "stdafx.h"
#include "WeaselTSF.h"
#include "Compartment.h"
#include "CompartmentUtil.h"
#include "EditSession.h"
#include <resource.h>
#include <functional>
#include <ios>
#include <WeaselUtility.h>
#include "ResponseParser.h"
#include "CandidateList.h"
#include "LanguageBar.h"

STDMETHODIMP CCompartmentEventSink::QueryInterface(REFIID riid,
                                                   _Outptr_ void** ppvObj) {
  if (ppvObj == nullptr)
    return E_INVALIDARG;

  *ppvObj = nullptr;

  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfCompartmentEventSink)) {
    *ppvObj = (CCompartmentEventSink*)this;
  }

  if (*ppvObj) {
    AddRef();
    return S_OK;
  }

  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CCompartmentEventSink::AddRef() {
  return ++_refCount;
}

STDMETHODIMP_(ULONG) CCompartmentEventSink::Release() {
  LONG cr = --_refCount;

  assert(_refCount >= 0);

  if (_refCount == 0) {
    delete this;
  }

  return cr;
}

STDMETHODIMP CCompartmentEventSink::OnChange(_In_ REFGUID guidCompartment) {
  return _callback(guidCompartment);
}

HRESULT CCompartmentEventSink::_Advise(_In_ com_ptr<IUnknown> punk,
                                       _In_ REFGUID guidCompartment) {
  HRESULT hr = S_OK;
  ITfCompartmentMgr* pCompartmentMgr = nullptr;
  ITfSource* pSource = nullptr;

  hr = punk->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompartmentMgr);
  if (FAILED(hr)) {
    return hr;
  }

  hr = pCompartmentMgr->GetCompartment(guidCompartment, &_compartment);
  if (SUCCEEDED(hr)) {
    hr = _compartment->QueryInterface(IID_ITfSource, (void**)&pSource);
    if (SUCCEEDED(hr)) {
      hr = pSource->AdviseSink(IID_ITfCompartmentEventSink, this, &_cookie);
      pSource->Release();
    }
  }

  pCompartmentMgr->Release();

  return hr;
}
HRESULT CCompartmentEventSink::_Unadvise() {
  // Advise may have failed before _compartment was set: nothing is advised,
  // so there is nothing to unadvise.
  if (_compartment == nullptr)
    return S_FALSE;

  HRESULT hr = S_OK;
  ITfSource* pSource = nullptr;

  hr = _compartment->QueryInterface(IID_ITfSource, (void**)&pSource);
  if (SUCCEEDED(hr)) {
    hr = pSource->UnadviseSink(_cookie);
    pSource->Release();
  }

  _compartment = nullptr;
  _cookie = 0;

  return hr;
}

BOOL WeaselTSF::_IsKeyboardDisabled() {
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  com_ptr<ITfContext> pContext;

  // 与原实现一致：无焦点文档/无 top context 视为禁用（不缓存：瞬态）
  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) != S_OK) ||
      (pDocMgrFocus == NULL))
    return TRUE;
  if ((pDocMgrFocus->GetTop(&pContext) != S_OK) || (pContext == NULL))
    return TRUE;

  // 焦点 top context 变化（文档切换、push/pop）时重挂 sink 并使缓存失效
  if (pContext != _pDisabledCacheContext)
    _RetargetKeyboardDisabledSinks(pContext);

  // 命中缓存：sink 已挂在当前 context 上，且值未被事件置脏
  if ((_pDisabledCacheContext != nullptr) && !_fKeyboardDisabledDirty)
    return _fKeyboardDisabled;

  BOOL fDisabled = FALSE;
  com_ptr<ITfCompartmentMgr> pCompMgr;
  if (pContext->QueryInterface(IID_ITfCompartmentMgr, (void**)&pCompMgr) ==
      S_OK) {
    /* Check GUID_COMPARTMENT_KEYBOARD_DISABLED */
    {
      com_ptr<ITfCompartment> pCompartmentDisabled;
      if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED,
                                   &pCompartmentDisabled) == S_OK) {
        VARIANT var;
        if (pCompartmentDisabled->GetValue(&var) == S_OK) {
          if (var.vt == VT_I4)  // Even VT_EMPTY, GetValue() can succeed
            fDisabled = (BOOL)var.lVal;
        }
      }
    }

    /* Check GUID_COMPARTMENT_EMPTYCONTEXT */
    {
      com_ptr<ITfCompartment> pCompartmentEmptyContext;
      if (pCompMgr->GetCompartment(GUID_COMPARTMENT_EMPTYCONTEXT,
                                   &pCompartmentEmptyContext) == S_OK) {
        VARIANT var;
        if (pCompartmentEmptyContext->GetValue(&var) == S_OK) {
          if (var.vt == VT_I4)  // Even VT_EMPTY, GetValue() can succeed
            fDisabled = (BOOL)var.lVal;
        }
      }
    }
  }

  // 仅当 sink 就绪（能收到后续置脏事件）才缓存，否则逐键查询
  if (_pDisabledCacheContext != nullptr) {
    _fKeyboardDisabled = fDisabled;
    _fKeyboardDisabledDirty = FALSE;
  }
  return fDisabled;
}

void WeaselTSF::_RetargetKeyboardDisabledSinks(
    com_ptr<ITfContext> pContext) {
  using namespace std::placeholders;

  if (_pKeyboardDisabledSink)
    _pKeyboardDisabledSink->_Unadvise();
  if (_pEmptyContextSink)
    _pEmptyContextSink->_Unadvise();
  _pDisabledCacheContext = nullptr;
  _fKeyboardDisabledDirty = TRUE;

  if (pContext == nullptr)
    return;

  auto callback = std::bind(&WeaselTSF::_OnKeyboardDisabledCompartmentChange,
                            this, _1);
  if (!_pKeyboardDisabledSink)
    _pKeyboardDisabledSink = new CCompartmentEventSink(callback);
  if (!_pEmptyContextSink)
    _pEmptyContextSink = new CCompartmentEventSink(callback);

  HRESULT hrDisabled = _pKeyboardDisabledSink->_Advise(
      (IUnknown*)pContext, GUID_COMPARTMENT_KEYBOARD_DISABLED);
  HRESULT hrEmpty = _pEmptyContextSink->_Advise((IUnknown*)pContext,
                                                GUID_COMPARTMENT_EMPTYCONTEXT);
  if (SUCCEEDED(hrDisabled) && SUCCEEDED(hrEmpty)) {
    _pDisabledCacheContext = pContext;
  }
  // advise 失败：_pDisabledCacheContext 保持空，_IsKeyboardDisabled
  // 不缓存、逐键全量查询，行为与原实现一致
}

HRESULT WeaselTSF::_OnKeyboardDisabledCompartmentChange(
    REFGUID guidCompartment) {
  _fKeyboardDisabledDirty = TRUE;
  return S_OK;
}

BOOL WeaselTSF::_IsKeyboardOpen() {
  com_ptr<ITfCompartmentMgr> pCompMgr;
  BOOL fOpen = FALSE;

  if (_pThreadMgr->QueryInterface(&pCompMgr) == S_OK) {
    com_ptr<ITfCompartment> pCompartment;
    if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                 &pCompartment) == S_OK) {
      VARIANT var;
      if (pCompartment->GetValue(&var) == S_OK) {
        if (var.vt == VT_I4)  // Even VT_EMPTY, GetValue() can succeed
          fOpen = (BOOL)var.lVal;
      }
    }
  }
  return fOpen;
}

HRESULT WeaselTSF::_SetKeyboardOpen(BOOL fOpen) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfCompartmentMgr> pCompMgr;

  if (_pThreadMgr->QueryInterface(&pCompMgr) == S_OK) {
    ITfCompartment* pCompartment;
    if (pCompMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                 &pCompartment) == S_OK) {
      VARIANT var;
      var.vt = VT_I4;
      var.lVal = fOpen;
      hr = pCompartment->SetValue(_tfClientId, &var);
    }
  }

  return hr;
}

namespace {
// 延迟载体：异步编辑会话在 OnChange 通知调用栈退栈后才执行，此时对
// OPENCLOSE compartment 的 SetValue 不再被 E_UNEXPECTED 拒绝。
// 会话只做强制重开，不触碰文档，TF_ES_READ 足够
class CForceKeyboardOpenEditSession : public CEditSession {
 public:
  CForceKeyboardOpenEditSession(com_ptr<WeaselTSF> pTextService,
                                com_ptr<ITfContext> pContext)
      : CEditSession(pTextService, pContext) {}

  STDMETHODIMP DoEditSession(TfEditCookie ec) {
    _pTextService->_ApplyDeferredKeyboardOpen();
    return S_OK;
  }
};
}  // namespace

void WeaselTSF::_RequestKeyboardOpenDeferred() {
  if (_pThreadMgr == NULL)
    return;

  // 借力的文档上下文：优先焦点文档 top（Ctrl+Space 即发生于焦点应用），
  // 其次最近一次键处理的上下文
  com_ptr<ITfContext> pContext;
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
      (pDocMgrFocus != NULL)) {
    pDocMgrFocus->GetTop(&pContext);
  }
  if (pContext == NULL)
    pContext = _pEditSessionContext;
  if (pContext == NULL)
    return;  // 无可借力的上下文：放弃本次强制重开（与原被拒行为一致）

  com_ptr<CForceKeyboardOpenEditSession> pEditSession;
  pEditSession.Attach(new CForceKeyboardOpenEditSession(this, pContext));
  if (pEditSession == NULL)
    return;
  HRESULT hrSession = E_FAIL;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hrSession);
}

void WeaselTSF::_ApplyDeferredKeyboardOpen() {
  // 异步会话可能晚于 Deactivate（TSF 会丢弃已停用客户端的排队会话，
  // 此处再兜底一次）
  if (_pThreadMgr == NULL)
    return;
  // SetValue 的通知是同步派发的：置位让 _HandleCompartment 跳过这次
  // 自写触发的 OnChange；值未变化（已开）时不触发通知，复位即可
  _fSuppressOpenCloseSelfWrite = TRUE;
  _SetKeyboardOpen(TRUE);
  _fSuppressOpenCloseSelfWrite = FALSE;
}

HRESULT WeaselTSF::_GetCompartmentDWORD(DWORD& value, const GUID guid) {
  return GetCompartmentDWORD(_pThreadMgr, _tfClientId, value, guid);
}

HRESULT WeaselTSF::_SetCompartmentDWORD(const DWORD& value, const GUID guid) {
  return SetCompartmentDWORD(_pThreadMgr, _tfClientId, value, guid);
}

BOOL WeaselTSF::_InitCompartment() {
  using namespace std::placeholders;

  auto callback = std::bind(&WeaselTSF::_HandleCompartment, this, _1);
  _pKeyboardCompartmentSink = new CCompartmentEventSink(callback);
  if (!_pKeyboardCompartmentSink)
    return FALSE;
  HRESULT hrKeyboard = _pKeyboardCompartmentSink->_Advise(
      (IUnknown*)_pThreadMgr, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);

  _pConvertionCompartmentSink = new CCompartmentEventSink(callback);
  if (!_pConvertionCompartmentSink)
    return FALSE;
  HRESULT hrConversion = _pConvertionCompartmentSink->_Advise(
      (IUnknown*)_pThreadMgr, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);

  // Keep both results separately (the first used to be overwritten by the
  // second). On failure the sink stays around: _Unadvise() is null-safe, so
  // Deactivate() cleans it up when activation aborts.
  if (FAILED(hrKeyboard))
    LOG(ERROR) << "keyboard compartment advise failed: 0x" << std::hex
               << hrKeyboard;
  if (FAILED(hrConversion))
    LOG(ERROR) << "conversion compartment advise failed: 0x" << std::hex
               << hrConversion;
  return SUCCEEDED(hrKeyboard) && SUCCEEDED(hrConversion);
}

void WeaselTSF::_UninitCompartment() {
  if (_pKeyboardCompartmentSink) {
    _pKeyboardCompartmentSink->_Unadvise();
    _pKeyboardCompartmentSink = NULL;
  }
  if (_pConvertionCompartmentSink) {
    _pConvertionCompartmentSink->_Unadvise();
    _pConvertionCompartmentSink = NULL;
  }
  if (_pKeyboardDisabledSink) {
    _pKeyboardDisabledSink->_Unadvise();
    _pKeyboardDisabledSink = NULL;
  }
  if (_pEmptyContextSink) {
    _pEmptyContextSink->_Unadvise();
    _pEmptyContextSink = NULL;
  }
  _pDisabledCacheContext = NULL;
  _fKeyboardDisabledDirty = TRUE;
}

HRESULT WeaselTSF::_HandleCompartment(REFGUID guidCompartment) {
  if (IsEqualGUID(guidCompartment, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
    if (_isToOpenClose) {
      BOOL isOpen = _IsKeyboardOpen();
      // clear composition when close keyboard
      if (!isOpen && _pEditSessionContext) {
        m_client.ClearComposition();
        _EndComposition(_pEditSessionContext, true);
      }
      _EnableLanguageBar(isOpen);
      _UpdateLanguageBar(_status);
    } else {
      // 自写强制重开触发的通知（见 _ApplyDeferredKeyboardOpen）：跳过，
      // 否则 ascii_mode 会被这次自写翻转回去
      if (_fSuppressOpenCloseSelfWrite) {
        _fSuppressOpenCloseSelfWrite = FALSE;
        return S_OK;
      }
      _status.ascii_mode = !_status.ascii_mode;
      // 通知内对同一 compartment 的 SetValue 会被 TSF 以 E_UNEXPECTED
      // 拒绝，强制重开延迟到通知返回之后执行
      _RequestKeyboardOpenDeferred();
      if (_pLangBarButton && _pLangBarButton->IsLangBarDisabled())
        _EnableLanguageBar(true);
      _HandleLangBarMenuSelect(_status.ascii_mode
                                   ? ID_WEASELTRAY_ENABLE_ASCII
                                   : ID_WEASELTRAY_DISABLE_ASCII);
      if (_pEditSessionContext)
        m_client.ClearComposition();
      _UpdateLanguageBar(_status);
    }
  } else if (IsEqualGUID(guidCompartment,
                         GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
    BOOL isOpen = _IsKeyboardOpen();
    if (isOpen) {
      weasel::ResponseParser parser(NULL, NULL, &_status, NULL,
                                    &_cand->style());
      bool ok = m_client.GetResponseData(std::ref(parser));
      _UpdateLanguageBar(_status);
    }
  }
  return S_OK;
}
