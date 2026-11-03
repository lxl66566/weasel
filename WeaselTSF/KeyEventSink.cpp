#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    // 模拟期间的键不转发服务器，但 *pfEaten 必须有确定值
    // （下文无条件存入 _prevfEaten，且要上报 TSF）
    *pfEaten = FALSE;
    if (!_keyCountToSimulate)
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);

    if (ke.keycode == ibus::Caps_Lock) {
      if (_prevKeyEvent.keycode == ibus::Caps_Lock && _prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!_keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            _keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (_keyCountToSimulate)
        _keyCountToSimulate--;
    }

    _prevfEaten = *pfEaten;
    _prevKeyEvent = ke;
  }
}

STDMETHODIMP WeaselTSF::OnSetFocus(BOOL fForeground) {
  // 焦点切换后，任何等待配对回调的 pending 状态都已失效
  _testKeyDownPending.pending = FALSE;
  _testKeyUpPending.pending = FALSE;
  if (fForeground)
    m_client.FocusIn();
  else {
    m_client.FocusOut();
    _AbortComposition();
    // V1：Caps Lock 模拟的配对状态跨窗口失效。合成态下在 A 窗口按下
    // Caps（被吃）后焦点切走，_AbortComposition 已置 _committed = TRUE，
    // 若保留配对（前一键为被吃的 Caps 按下），在 B 窗口的释放会误判
    // 满足模拟条件，把合成的双事件注入 B 窗口。按下/释放的配对只在同
    // 窗口的连续序列内有意义，随焦点一并作废。
    // _keyCountToSimulate 是注入事件的吞噬护栏，FALSE 到来时前台已易
    // 主，SendInput 的在途模拟键会随焦点投递到新前台线程、由那边自己的
    // 键事件 sink 按真实键处理；本地残留的计数只会让焦点切回后的普通键
    // 因“计数非零期间不转发服务器”而漏送（对非 Caps 键计数永不递减，
    // 相当于输入法失效），一并清零
    _prevKeyEvent = weasel::KeyEvent();
    _prevfEaten = FALSE;
    _keyCountToSimulate = 0;
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *  Some swallows the key after an eaten OnTestKeyDown(), so the paired
 * OnKeyDown() never comes.
 *
 * We use the pending state to omit multiple OnTestKeyDown() calls, and for
 * OnKeyDown() to check if the key has already been sent to the server. The
 * pending state is keyed by virtual-key code only: the paired Test/Key
 * callbacks are not guaranteed to carry identical lParam (for non-TSF apps
 * the caller is the CUAS IMM32-bridge, which may pass different lParam bits
 * between the two calls), while the vkey is the whole key identity this
 * component consumes (see KeyEvent.cpp). Any event for a different key
 * starts a new cycle, so a stale pending flag (its paired call was
 * swallowed) cannot eat the next key.
 */

STDMETHODIMP WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL* pfEaten) {
  _testKeyUpPending.pending = FALSE;
  if (_testKeyDownPending.pending && _testKeyDownPending.vkey == wParam) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  _testKeyDownPending.pending = *pfEaten;
  _testKeyDownPending.vkey = static_cast<UINT>(wParam);
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyDown(ITfContext* pContext,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL* pfEaten) {
  _testKeyUpPending.pending = FALSE;
  BOOL fDuplicate =
      _testKeyDownPending.pending && _testKeyDownPending.vkey == wParam;
  _testKeyDownPending.pending = FALSE;
  if (fDuplicate) {
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL* pfEaten) {
  _testKeyDownPending.pending = FALSE;
  if (_testKeyUpPending.pending && _testKeyUpPending.vkey == wParam) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  _testKeyUpPending.pending = *pfEaten;
  _testKeyUpPending.vkey = static_cast<UINT>(wParam);
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnKeyUp(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _testKeyDownPending.pending = FALSE;
  BOOL fDuplicate =
      _testKeyUpPending.pending && _testKeyUpPending.vkey == wParam;
  _testKeyUpPending.pending = FALSE;
  if (fDuplicate) {
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDMETHODIMP WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                       REFGUID rguid,
                                       BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}
