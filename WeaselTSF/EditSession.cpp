#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "CandidateList.h"

STDMETHODIMP CUpdateCompositionEditSession::DoEditSession(TfEditCookie ec) {
  // 只回交构造时捕获的快照，不读共享成员（D3，见 EditSession.h）
  return _pTextService->_ApplyCompositionUpdate(ec, _pContext,
                                                std::move(_commit));
}

STDMETHODIMP WeaselTSF::_ApplyCompositionUpdate(TfEditCookie ec,
                                                com_ptr<ITfContext> pContext,
                                                std::wstring commit) {
  // 应答已在 _UpdateComposition 中解析（_status/_context/_config 及语言栏），
  // 本会话只把结果应用到文档；commit 与目标上下文由会话对象在构造时
  // 捕获（D3），取走即清，重复排队的会话天然幂等
  bool compositionEnded = false;
  // composing 帧只暂存 UI 数据，等位置更新后由 _SetCompositionPosition
  // 末尾的 _FlushPendingUI() 发布（cecb9e8 移植）；非 composing 帧
  // 仍在会话末尾立即发布。暂存的是 shared_ptr 拷贝，与 D3 的会话
  // 状态捕获（commit/context 由会话对象构造时取走）正交
  if (_status.composing) {
    _pendingUIContext = _context;
    _pendingUIStatus = _status;
    _hasPendingUI = true;
  }
  if (!commit.empty()) {
    // For auto-selecting, commit and preedit can both exist.
    // Commit the old TSF composition. If Rime immediately has a new
    // preedit (top-word input), _EndComposition() drops the local pointer
    // synchronously, so the following state check starts a new TSF
    // composition instead of observing the old one.
    if (!_IsComposing()) {
      _StartComposition(pContext,
                        _fCUASWorkaroundEnabled && !_config.inline_preedit);
    }
    _InsertText(pContext, commit);
    // Keep the candidate UI alive while the replacement composition is
    // being created; otherwise the key-down path destroys the old window
    // and the new one cannot be positioned until key-up.
    _EndComposition(pContext, false, !_status.composing);
    compositionEnded = true;
    _committed = TRUE;
  } else {
    _committed = FALSE;
  }
  if (_status.composing && (compositionEnded || !_IsComposing())) {
    _StartComposition(pContext,
                      _fCUASWorkaroundEnabled && !_config.inline_preedit);
  } else if (!_status.composing && _IsComposing()) {
    _EndComposition(pContext, true);
  }
  if (_IsComposing() && _config.inline_preedit) {
    _ShowInlinePreedit(pContext, _context);
  }

  if (!compositionEnded)
    _UpdateCompositionWindow(pContext);
  // Keep the existing candidate window alive during top-word input, but
  // publish the new candidates in this key-down edit session. Positioning is
  // still updated by the queued read session after the new composition is
  // created.
  //
  // Wait for the current composition's real text extent before painting
  // new candidates. This avoids one frame at the previous caret position
  // (cecb9e8 移植)。合成帧改由 extent 会话里的 _FlushPendingUI() 在
  // _SetCompositionPosition 更新位置后发布；commit/结束帧仍立即发布。
  if (!(_status.composing && _IsComposing()))
    _UpdateUI(*_context, _status);

  return S_OK;
}
