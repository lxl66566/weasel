#pragma once
#include <WeaselIPCData.h>
#include <WeaselUI.h>
#include "StandardLayout.h"
#include "Layout.h"
#include "GdiplusBlur.h"

#include <tuple>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using namespace weasel;

typedef CWinTraits<WS_POPUP | WS_CLIPSIBLINGS | WS_DISABLED,
                   WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                       WS_EX_LAYERED>
    CWeaselPanelTraits;

enum class BackType {
  TEXT = 0,
  CAND = 1,
  BACKGROUND = 2  // background
};

// 自定义消息：把跨线程的 UI 调用 marshal 回窗口所属线程（UI 线程）执行。
// Direct2D 的 ID2D1RenderTarget 不是线程安全的，必须保证所有绘制 / 资源重建
// 都在同一个线程上串行发生；ctx_/status_/style_ 的写入同样只允许发生在
// UI 线程，避免另一线程在绘制中读到被改写的共享状态。
// 取 WM_APP + 0x1000 起始，避开 WeaselIPC.h 中已使用的 WM_APP+1.. 段。
// WM_WEASEL_UPDATE/SETSTYLE 的 lParam 携带堆分配的快照，由消息处理方释放。
#define WM_WEASEL_REFRESH (WM_APP + 0x1000)
#define WM_WEASEL_REDRAW (WM_APP + 0x1001)
#define WM_WEASEL_MOVETO (WM_APP + 0x1002)
#define WM_WEASEL_UPDATE (WM_APP + 0x1003)
#define WM_WEASEL_SETSTYLE (WM_APP + 0x1004)
// 显隐命令：跨线程调用 Show/Hide/ShowWithTimeout 时经 WM_WEASEL_SHOW 投递，
// wParam 携带命令，lParam 携带超时毫秒值，不涉及堆对象。
#define WM_WEASEL_SHOW (WM_APP + 0x1005)

// 提示窗显隐命令。枚举即命令的全部合法取值，经 WPARAM 传输故显式指定底层
// 类型，避免隐式转换引入未经校验的值。
enum class PanelVisibility : WPARAM {
  Hide = 0,
  Show = 1,
  ShowWithTimeout = 2,
};

// B12a：高亮阴影模糊位图的缓存键——收录参与生成模糊位图像素的全部输入
// （矩形尺寸、位图边距、圆角、模糊半径、阴影偏移、颜色含 alpha）。同键必出
// 同图：样式 / DPI / 布局边距变化自然落入不同的键，无需额外的失效逻辑。
struct ShadowBitmapKey {
  bool operator<(const ShadowBitmapKey& o) const {
    return std::tie(width, height, blur_margin_x, blur_margin_y, radius,
                    shadow_radius, shadow_offset_x, shadow_offset_y,
                    shadow_color) <
           std::tie(o.width, o.height, o.blur_margin_x, o.blur_margin_y,
                    o.radius, o.shadow_radius, o.shadow_offset_x,
                    o.shadow_offset_y, o.shadow_color);
  }
  int width;
  int height;
  int blur_margin_x;
  int blur_margin_y;
  int radius;
  int shadow_radius;
  int shadow_offset_x;
  int shadow_offset_y;
  COLORREF shadow_color;
};

class WeaselPanel
    : public CWindowImpl<WeaselPanel, CWindow, CWeaselPanelTraits>,
      CDoubleBufferImpl<WeaselPanel> {
 public:
  BEGIN_MSG_MAP(WeaselPanel)
  MESSAGE_HANDLER(WM_CREATE, OnCreate)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
  MESSAGE_HANDLER(WM_TIMER, OnAutoHideTimer)
  MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
  MESSAGE_HANDLER(WM_LBUTTONUP, OnLeftClickedUp)
  MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLeftClickedDown)
  MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
  MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
  MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
  MESSAGE_HANDLER(WM_WEASEL_REFRESH, OnRefreshPanel)
  MESSAGE_HANDLER(WM_WEASEL_REDRAW, OnRedrawWindow)
  MESSAGE_HANDLER(WM_WEASEL_MOVETO, OnMoveTo)
  MESSAGE_HANDLER(WM_WEASEL_UPDATE, OnApplyUpdate)
  MESSAGE_HANDLER(WM_WEASEL_SETSTYLE, OnApplyStyle)
  MESSAGE_HANDLER(WM_WEASEL_SHOW, OnShowCommand)
  CHAIN_MSG_MAP(CDoubleBufferImpl<WeaselPanel>)
  END_MSG_MAP()

  LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnDpiChanged(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnRefreshPanel(UINT uMsg,
                         WPARAM wParam,
                         LPARAM lParam,
                         BOOL& bHandled);
  LRESULT OnRedrawWindow(UINT uMsg,
                         WPARAM wParam,
                         LPARAM lParam,
                         BOOL& bHandled);
  LRESULT OnMoveTo(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnApplyUpdate(UINT uMsg,
                        WPARAM wParam,
                        LPARAM lParam,
                        BOOL& bHandled);
  LRESULT OnApplyStyle(UINT uMsg,
                       WPARAM wParam,
                       LPARAM lParam,
                       BOOL& bHandled);
  LRESULT OnShowCommand(UINT uMsg,
                        WPARAM wParam,
                        LPARAM lParam,
                        BOOL& bHandled);
  LRESULT OnAutoHideTimer(UINT uMsg,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL& bHandled);
  LRESULT OnMouseActivate(UINT uMsg,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL& bHandled);
  LRESULT OnLeftClickedUp(UINT uMsg,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL& bHandled);
  LRESULT OnLeftClickedDown(UINT uMsg,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL& bHandled);
  LRESULT OnMouseWheel(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnMouseMove(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnMouseLeave(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

  WeaselPanel(weasel::UI& ui);
  ~WeaselPanel();

  void MoveTo(RECT const& rc);
  void Refresh();
  void DoPaint(CDCHandle dc);
  bool GetIsReposition() { return m_istorepos; }
  void RedrawWindow();
  // 跨线程更新入口：非 UI 线程调用时把快照投递回 UI 线程再落地
  void ApplyUpdate(Context const& ctx, Status const& status);
  void ApplyStyle(UIStyle const& style);

  // 界面显隐入口：跨线程（IPC 工作线程）调用时经 WM_WEASEL_SHOW marshal 回
  // UI 线程执行。跨线程直接 ShowWindow 是同步 send，若窗口线程正阻塞在
  // g_api_mutex 等待上会与之互等死锁。
  void Show();
  void Hide();
  void ShowWithTimeout(UINT millisec);
  bool IsShown() const { return m_shown; }
  bool IsCountingDown() const { return m_autohide_counting; }

  static VOID CALLBACK OnTimer(_In_ HWND hwnd,
                               _In_ UINT uMsg,
                               _In_ UINT_PTR idEvent,
                               _In_ DWORD dwTime);
  static const int AUTOREV_TIMER = 20240315;
  static UINT_PTR ptimer;
  // 提示窗自动隐藏定时器（ShowWithTimeout 启动，到期经 WM_TIMER 隐藏）
  static const UINT AUTOHIDE_TIMER = 20121220;

 private:
  template <typename T>
  int DPI_SCALE(T t) {
    return (int)(t * dpiScaleLayout);
  }
  // 判断当前线程是否为窗口所属线程（UI 线程）。所有触碰 HWND / D2D 资源的
  // 绘制类操作都必须在 UI 线程上执行，否则 ID2D1RenderTarget 的并发访问会
  // 触发 d2d1.dll 内部崩溃（0xC0000005）。
  bool _IsUiThread() const;
  void _InitFontRes(bool forced = false);
  // 绘制 / 布局失败后的恢复（UI 线程调用）：清 m_octx 强制下次重绘、
  // 重建 DWrite 资源、按剩余预算投递 WM_WEASEL_REFRESH 完整重算
  void _RequestPaintRecovery();
  // 清空阴影位图缓存并复位像素计数（样式落地等旧位图作废时调用）
  void _ClearShadowBitmapCache();
  void _ApplyUpdate(Context const& ctx, Status const& status);
  // 显隐命令的落地与 marshal（仅 _ApplyVisibility 允许触碰窗口 API）
  void _SetVisibility(PanelVisibility cmd, UINT millisec);
  void _ApplyVisibility(PanelVisibility cmd, UINT millisec);
  void _CancelAutoHide();
  void _CaptureRect(CRect& rect);
  bool m_mouse_entry = false;
  CPoint m_lastMousePos = {-1, -1};
  void _CreateLayout();
  // 全屏布局自适应字号对共享 pDWR 的借用边界（见实现处注释）
  void _ScopeFullscreenFont(bool fullscreen);
  void _ResizeWindow();
  void _RepositionWindow(const bool& adj = false);
  bool _DrawPreedit(const Text& text, CDCHandle dc, const CRect& rc);
  bool _DrawPreeditBack(const Text& text, CDCHandle dc, const CRect& rc);
  bool _DrawCandidates(CDCHandle& dc, bool back = false);
  void _HighlightText(CDCHandle& dc,
                      const CRect& rc,
                      const COLORREF& color,
                      const COLORREF& shadowColor,
                      const int& radius,
                      const BackType& type,
                      const IsToRoundStruct& rd,
                      const COLORREF& bordercolor);
  void _TextOut(const CRect& rc,
                const std::wstring& psz,
                const size_t& cch,
                const int& inColor,
                IDWriteTextFormat1* const pTextFormat = NULL);

  void _LayerUpdate(const CRect& rc, CDCHandle dc);

  weasel::Layout* m_layout;
  weasel::Context& m_ctx;
  weasel::Context& m_octx;
  weasel::Status& m_status;
  weasel::UIStyle& m_style;
  weasel::UIStyle& m_ostyle;
  const bool& m_in_server;

  CRect m_inputPos;
  // offset y for candidates when vertical layout over bottom
  // 就地初始化：首次绘制 / 命中测试可能先于 _RepositionWindow 写入发生，
  // 不能读未初始化内存
  int m_offsetys[MAX_CANDIDATES_COUNT] = {};
  int m_offsety_preedit = 0;
  int m_offsety_aux = 0;
  bool m_istorepos = false;

  CIcon m_iconDisabled;
  CIcon m_iconEnabled;
  CIcon m_iconAlpha;
  CIcon m_iconFull;
  CIcon m_iconHalf;
  std::wstring m_current_zhung_icon;
  std::wstring m_current_ascii_icon;
  std::wstring m_current_half_icon;
  std::wstring m_current_full_icon;
  // for gdiplus drawings
  Gdiplus::GdiplusStartupInput _m_gdiplusStartupInput;
  ULONG_PTR _m_gdiplusToken;

  UINT dpi;

  CRect rcw;
  BYTE m_candidateCount;
  BYTE m_lastCandidateCount;

  bool hide_candidates;
  bool m_sticky;
  // 提示窗显隐状态（UI 线程写）：m_autohide_counting 表示自动隐藏倒计时进行中
  bool m_shown = false;
  bool m_autohide_counting = false;
  // for multi font_face & font_point
  PDWR pDWR;
  // 全屏布局（FullScreenLayout::AdjustFontPoint）在布局期间会改写共享 pDWR
  // 的文本格式字号以自适应铺满工作区。进入全屏布局前快照原始格式，换回
  // 普通布局时恢复，把字号漂移限定在全屏布局对象的存活期内，不泄漏给
  // 后续布局（_InitFontRes 仅在 style/DPI 变化时重建 pDWR，不会自行复位）
  struct TextFormatsSnapshot {
    ComPtr<IDWriteTextFormat1> text;
    ComPtr<IDWriteTextFormat1> preedit;
    ComPtr<IDWriteTextFormat1> label;
    ComPtr<IDWriteTextFormat1> comment;
  };
  bool m_fullscreen_font_active = false;
  TextFormatsSnapshot m_fullscreen_font_snapshot;
  std::function<void(size_t* const, size_t* const, bool* const, bool* const)>&
      _UICallback;
  float bar_scale_ = 1.0;
  float dpiScaleLayout = 1.0f;
  int m_hoverIndex = -1;
  HMONITOR m_hMonitor = NULL;
  bool m_redraw_by_monitor_change = false;
  // 自动恢复重试的剩余投递预算：完整绘成一帧后复位；耗尽后停止自动重试
  // （等待下一次外部刷新），防止持续失败时消息自旋
  static constexpr BYTE MAX_PAINT_RECOVERY = 2;
  BYTE m_paint_recovery_left = MAX_PAINT_RECOVERY;

  // B12a：阴影模糊位图缓存（键见 ShadowBitmapKey）。位图生成 = GDI+ 几何
  // 绘制 + 确定性盒模糊，是键的纯函数，命中即可直接复用；消费方仅
  // DrawImage 读取、不改动不持有。FIFO 有界，样式落地时整体清空；只在
  // UI 线程（DoPaint 路径）访问，无锁。条目是 GDI+ 对象，必须在
  // GdiplusShutdown 之前释放（见 ~WeaselPanel）。
  static constexpr size_t SHADOW_BITMAP_CACHE_CAP = 16;
  // D12：缓存总像素二级上限，像素数等价 SHADOW_BITMAP_CACHE_PIXEL_WINDOWS
  // 张全窗口位图（按绘制时 rcw 计）。仅按条目数封顶时，BACKGROUND 类条目
  // 覆盖整个内容矩形，4K/DPI 200% 下单条可达数 MB，最坏可累计数十 MB 常驻。
  // 超预算从最旧逐出；单条即超预算的超大条目不入缓存（仅当帧绘制）
  static constexpr size_t SHADOW_BITMAP_CACHE_PIXEL_WINDOWS = 8;
  // 缓存内条目的像素总量（与键推算值一致地增减，见 _HighlightText）
  size_t m_shadowBitmapCachePixels = 0;
  std::vector<std::pair<ShadowBitmapKey, std::unique_ptr<Gdiplus::Bitmap>>>
      m_shadowBitmapCache;
};
