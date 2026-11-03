#include "stdafx.h"
#include <WeaselUI.h>
#include "WeaselPanel.h"

using namespace weasel;

class weasel::UIImpl {
 public:
  WeaselPanel panel;

  explicit UIImpl(weasel::UI& ui) : panel(ui) {}
  ~UIImpl() {}
  // 窗口类操作（显隐、定时器、绘制）统一由 panel marshal 回 UI 线程执行，
  // 本层不再直接触碰 HWND：跨线程 ShowWindow 是同步 send，窗口线程若正
  // 阻塞在 g_api_mutex 上会与之互等死锁。
  void Refresh() {
    if (!panel.IsWindow())
      return;
    // 与旧路径一致：自动隐藏倒计时进行中时先结束倒计时（Hide 一并停表）
    if (panel.IsCountingDown())
      panel.Hide();
    panel.Refresh();
  }
  void Update(Context const& ctx, Status const& status) {
    // 旧实现中 Update 开头的 Hide + KillTimer 前置逻辑由 panel 在落快照前
    // 于 UI 线程完成（_ApplyUpdate），顺序与行为不变
    panel.ApplyUpdate(ctx, status);
  }
  void SetStyle(UIStyle const& style) { panel.ApplyStyle(style); }
  void Show() { panel.Show(); }
  void Hide() { panel.Hide(); }
  void ShowWithTimeout(size_t millisec) {
    panel.ShowWithTimeout(static_cast<UINT>(millisec));
  }
  bool IsShown() const { return panel.IsShown(); }
  bool IsCountingDown() const { return panel.IsCountingDown(); }
};

bool UI::Create(HWND parent) {
  if (pimpl_) {
    pimpl_->panel.Create(
        parent, 0, 0, WS_POPUP,
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        0U, 0);
    if (!pimpl_->panel.IsWindow())
      return false;
    return true;
  }

  pimpl_ = new UIImpl(*this);
  if (!pimpl_)
    return false;

  pimpl_->panel.Create(
      parent, 0, 0, WS_POPUP,
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      0U, 0);
  if (!pimpl_->panel.IsWindow())
    return false;
  return true;
}

void UI::Destroy(bool full) {
  if (pimpl_) {
    // destroy panel
    if (pimpl_->panel.IsWindow()) {
      pimpl_->panel.DestroyWindow();
    }
    if (full) {
      delete pimpl_;
      pimpl_ = 0;
      pDWR.reset();
    }
  }
}

bool UI::GetIsReposition() {
  if (pimpl_)
    return pimpl_->panel.GetIsReposition();
  else
    return false;
}

void UI::Show() {
  if (pimpl_) {
    pimpl_->Show();
  }
}

void UI::Hide() {
  if (pimpl_) {
    pimpl_->Hide();
  }
}

void UI::ShowWithTimeout(size_t millisec) {
  if (pimpl_) {
    pimpl_->ShowWithTimeout(millisec);
  }
}

bool UI::IsCountingDown() const {
  return pimpl_ && pimpl_->IsCountingDown();
}

bool UI::IsShown() const {
  return pimpl_ && pimpl_->IsShown();
}

void UI::Refresh() {
  if (pimpl_) {
    pimpl_->Refresh();
  }
}

void UI::UpdateInputPosition(RECT const& rc) {
  if (pimpl_ && pimpl_->panel.IsWindow()) {
    pimpl_->panel.MoveTo(rc);
  }
}

void UI::Update(const Context& ctx, const Status& status) {
  if (!pimpl_) {
    // 窗口尚未创建（TSF 首次组合前）：不存在并发读者，直接落状态
    ctx_ = ctx;
    status_ = status;
    return;
  }
  // 写 ctx_/status_ 与 dedup 判断都收敛到 UI 线程（panel 内部 marshal），
  // 调用线程（服务端为 IPC 工作线程）不再触碰这些共享状态
  pimpl_->Update(ctx, status);
}

void UI::SetStyle(const UIStyle& style) {
  if (!pimpl_) {
    style_ = style;
    return;
  }
  pimpl_->SetStyle(style);
}
