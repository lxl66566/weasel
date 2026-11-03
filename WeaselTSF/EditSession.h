#pragma once

#include "stdafx.h"
#include "WeaselTSF.h"

class CEditSession : public ITfEditSession {
 public:
  CEditSession(com_ptr<WeaselTSF> pTextService, com_ptr<ITfContext> pContext)
      : _pTextService(pTextService), _pContext(pContext) {
    _cRef = 1;
  }

  virtual ~CEditSession() {}

  /* IUnknown */
  STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == NULL)
      return E_INVALIDARG;

    *ppvObject = NULL;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
      *ppvObject = (ITfEditSession*)this;

    if (*ppvObject) {
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() { return ++_cRef; }

  STDMETHODIMP_(ULONG) Release() {
    LONG cr = --_cRef;
    assert(_cRef >= 0);
    if (_cRef == 0)
      delete this;
    return cr;
  }

  /* ITfEditSession */
  virtual STDMETHODIMP DoEditSession(TfEditCookie ec) = 0;

 protected:
  com_ptr<WeaselTSF> _pTextService;
  com_ptr<ITfContext> _pContext;

 private:
  LONG _cRef; /* COM reference count */
};

// D3：_UpdateComposition 排队的应用会话。commit 与目标上下文在构造时
// 捕获（commit 整体 std::move，不与后续会话共享同一成员字符串）：
// TF_ES_ASYNCDONTCARE 会话可能被宿主延迟（如 Word 持文档锁），若执行
// 时才读 WeaselTSF 的共享成员，延迟窗口内的新键/新文档已把
// _pendingCommit/_pEditSessionContext 覆盖成新值，旧会话便会把旧文档的
// commit 写进新文档。捕获后各会话只消费自己的应答；同步场景（会话在
// _UpdateComposition 调用内即执行）行为与旧实现一致
// （旧：WeaselTSF 自身实现 ITfEditSession，读成员快照）
class CUpdateCompositionEditSession : public CEditSession {
 public:
  CUpdateCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                                com_ptr<ITfContext> pContext,
                                std::wstring commit)
      : CEditSession(pTextService, pContext), _commit(std::move(commit)) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _commit;
};