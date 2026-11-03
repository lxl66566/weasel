#include "stdafx.h"
#include "WeaselServerImpl.h"
#include <PipeServer.h>
#include <mutex>
#include <vector>
#include <Windows.h>
#include <resource.h>
#include <WeaselUtility.h>

using namespace weasel;

extern CAppModule _Module;

// Serializes every RequestHandler call: pipe worker threads run under it
// in Run(); window-message paths (color change, tray menu, logoff) must
// take it too, or they race handler state (session map, rime) mid-call.
static std::mutex g_api_mutex;

// Bounded backoff before Listen retries a failed iteration: without it a
// persistently failing CreateNamedPipe (e.g. handle exhaustion) turns the
// loop into a 100% CPU spin. The sleep itself is interruptible, so shutdown
// (interrupt + WakeListener) is never delayed by the backoff.
static constexpr boost::chrono::milliseconds kListenRetryBackoff{50};

ServerImpl::ServerImpl()
    : m_pRequestHandler(NULL),
      m_darkMode(IsUserDarkMode()),
      channel(std::make_unique<PipeServer>(GetPipeName(), sa.get_attr())) {
  m_hUser32Module = GetModuleHandle(_T("user32.dll"));
  m_pPhysicalToLogicalPointForPerMonitorDPI =
      reinterpret_cast<PPhysicalToLogicalPointForPerMonitorDPI>(
          ::GetProcAddress(m_hUser32Module,
                           "PhysicalToLogicalPointForPerMonitorDPI"));
}

ServerImpl::~ServerImpl() {
  _Finailize();
}

void ServerImpl::_Finailize() {
  // Ordered, idempotent teardown: join the pipe threads before the window
  // goes away, so no worker can touch a destroyed handler or UI.
  _StopPipeServer();

  if (IsWindow()) {
    DestroyWindow();
  }
}

LRESULT ServerImpl::OnColorChange(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  if (IsUserDarkMode() != m_darkMode) {
    m_darkMode = IsUserDarkMode();
    if (m_pRequestHandler) {
      std::lock_guard guard(g_api_mutex);
      m_pRequestHandler->UpdateColorTheme(m_darkMode);
    }
  }
  return 0;
}

LRESULT ServerImpl::OnCreate(UINT uMsg,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL& bHandled) {
  // not neccessary...
  ::SetWindowText(m_hWnd, WEASEL_IPC_WINDOW);
  return 0;
}

LRESULT ServerImpl::OnClose(UINT uMsg,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL& bHandled) {
  Stop();
  return 0;
}

LRESULT ServerImpl::OnDestroy(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  bHandled = FALSE;
  return 1;
}

LRESULT ServerImpl::OnQueryEndSystemSession(UINT uMsg,
                                            WPARAM wParam,
                                            LPARAM lParam,
                                            BOOL& bHandled) {
  return TRUE;
}

LRESULT ServerImpl::OnEndSystemSession(UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL& bHandled) {
  if (m_pRequestHandler) {
    std::lock_guard guard(g_api_mutex);
    m_pRequestHandler->Finalize();
    m_pRequestHandler = nullptr;
  }
  return 0;
}

/* Toggle ascii_mode on the given session. The api lock must be held by
 * the caller (both OnCommand paths lock at their own boundary, D1).
 *
 * lParam semantics differ between the two OnCommand paths (V8): the
 * pipe path (3-param OnCommand, from ClientImpl::TrayCommand) carries
 * the client's session id - 0 is legal there and means "no typing
 * session", e.g. WeaselServer.exe /ascii toggling every session (see
 * RimeWithWeaselHandler::SetOption); the WM_COMMAND path (4-param
 * OnCommand, from TrackPopupMenu) carries the originating HMENU
 * instead. The tray menu has no ascii item today, but if one is ever
 * added, the HMENU value would land here as a bogus session id - so a
 * nonzero sessionId must name a live session before it reaches
 * SetOption. */
void ServerImpl::_OnAsciiModeToggle(UINT uID, DWORD sessionId) {
  if (!m_pRequestHandler)
    return;
  if (sessionId != 0 && !m_pRequestHandler->FindSession(sessionId))
    return;  // not a live session: drop instead of feeding it to rime
  m_pRequestHandler->SetOption(sessionId, "ascii_mode",
                               uID == ID_WEASELTRAY_ENABLE_ASCII);
}

LRESULT ServerImpl::OnCommand(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  UINT uID = LOWORD(wParam);
  switch (uID) {
    case ID_WEASELTRAY_ENABLE_ASCII:
    case ID_WEASELTRAY_DISABLE_ASCII: {
      // WM_COMMAND path (TrackPopupMenu): runs on the server message
      // thread with no api lock held, so take it here - exactly once per
      // path, never nested (D1).
      std::lock_guard guard(g_api_mutex);
      _OnAsciiModeToggle(uID, lParam);
      return 0;
    }
    default:;
  }

  std::map<UINT, CommandHandler>::iterator it = m_MenuHandlers.find(uID);
  if (it == m_MenuHandlers.end()) {
    bHandled = FALSE;
    return 0;
  }
  it->second();  // execute command
  return 0;
}

LRESULT ServerImpl::OnServiceNotifyMessage(UINT uMsg,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           BOOL& bHandled) {
  // Runs on the server message thread, NOT on a pipe worker thread and
  // without holding g_api_mutex, so that Shell_NotifyIcon inside the tray
  // update can never deadlock against the taskbar UI thread.
  if (m_trayRefreshCallback) {
    m_trayRefreshCallback();
  }
  return 0;
}

DWORD ServerImpl::OnCommand(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  // Pipe TRAY_COMMAND path: the listener dispatches this message
  // WITHOUT holding g_api_mutex (see Run()), so the ascii branch takes
  // the lock itself - exactly once per path, never nested (D1). lParam
  // is the client's session id here, not an HMENU.
  UINT uID = LOWORD(wParam);
  switch (uID) {
    case ID_WEASELTRAY_ENABLE_ASCII:
    case ID_WEASELTRAY_DISABLE_ASCII: {
      std::lock_guard guard(g_api_mutex);
      _OnAsciiModeToggle(uID, lParam);
      return TRUE;
    }
    default:;
  }

  // Registered menu commands run lock-free: m_MenuHandlers is written
  // only before Start() and read-only at runtime, and a handler may
  // block for seconds inside ShellExecuteW (/deploy, /wiki, ...) - it
  // must never stall every keystroke behind the global api lock (D4).
  std::map<UINT, CommandHandler>::iterator it = m_MenuHandlers.find(uID);
  if (it == m_MenuHandlers.end())
    return FALSE;
  it->second();  // execute command
  return TRUE;
}

HWND ServerImpl::Start() {
  std::wstring instanceName = L"(WEASEL)Furandōru-Sukāretto-";
  instanceName += getUsername();
  HANDLE hMutexOneInstance = ::CreateMutex(NULL, FALSE, instanceName.c_str());
  bool areYouOK = (::GetLastError() == ERROR_ALREADY_EXISTS ||
                   ::GetLastError() == ERROR_ACCESS_DENIED);

  if (areYouOK) {
    LOG(ERROR) << "another WeaselServer instance is already running";
    return 0;  // assure single instance
  }

  HWND hwnd = Create(NULL);
  if (!hwnd)
    LOG(ERROR) << "ServerImpl::Create failed, error: " << GetLastError();

  return hwnd;
}

int ServerImpl::Stop() {
  // DO NOT exit process or finalize here
  // Let WeaselServer handle this
  PostMessage(WM_QUIT);
  return 0;
}

int ServerImpl::Run() {
  // This workaround causes a VC internal error:
  // void PipeServer::Listen(ServerHandler handler);
  //
  // auto handler = boost::bind(&ServerImpl::HandlePipeMessage, this);
  // auto listener = boost::bind(&PipeServer::Listen, channel.get(), handler);
  //
  auto listener = [this](PipeMessage msg, PipeServer::Respond resp) -> void {
    // Defer the response write until the api lock is released: a slow or
    // hung client must not extend the global critical section.
    PipeServer::Msg result = 0;
    if (msg.Msg == WEASEL_IPC_TRAY_COMMAND) {
      // Tray commands dispatch OUTSIDE the api lock: their menu handlers
      // may block for seconds inside ShellExecuteW and must never stall
      // every keystroke behind it (D4); the ascii branch takes the lock
      // itself inside OnCommand (D1). Every other message touches
      // RequestHandler state and keeps the lock.
      HandlePipeMessage(msg, [&result](PipeServer::Msg r) { result = r; });
    } else {
      std::lock_guard guard(g_api_mutex);
      HandlePipeMessage(msg, [&result](PipeServer::Msg r) { result = r; });
    }
    resp(result);
  };
  // Capture the listener by value: a stack reference would dangle as soon
  // as Run() returns while the pipe thread is still draining requests.
  pipeThread = std::make_unique<boost::thread>(
      [this, listener]() { channel->Listen(listener); });

  CMessageLoop theLoop;
  _Module.AddMessageLoop(&theLoop);
  int nRet = theLoop.Run();
  _Module.RemoveMessageLoop();
  // Stop the pipe server before the caller (WeaselServerApp::Run) starts
  // destroying the handler and UI: late requests would touch freed objects.
  _StopPipeServer();
  return nRet;
}

void ServerImpl::_StopPipeServer() {
  if (!pipeThread)
    return;
  pipeThread->interrupt();
  channel->WakeListener();
  if (pipeThread->joinable())
    pipeThread->join();
  channel->DrainWorkers();
  pipeThread.reset();
}

DWORD ServerImpl::OnEcho(WEASEL_IPC_COMMAND uMsg, DWORD wParam, DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->FindSession(lParam);
}

DWORD ServerImpl::OnStartSession(WEASEL_IPC_COMMAND uMsg,
                                 DWORD wParam,
                                 DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->AddSession(
      reinterpret_cast<LPWSTR>(channel->ReceiveBuffer()),
      [this](std::wstring& msg) -> bool {
        *channel << msg;
        return true;
      });
}

DWORD ServerImpl::OnEndSession(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->RemoveSession(lParam);
}

DWORD ServerImpl::OnKeyEvent(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler /* || !m_pSharedMemory*/)
    return 0;

  auto eat = [this](std::wstring& msg) -> bool {
    *channel << msg;
    return true;
  };
  return m_pRequestHandler->ProcessKeyEvent(KeyEvent(wParam), lParam, eat);
}

DWORD ServerImpl::OnShutdownServer(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  Stop();
  return 0;
}

DWORD ServerImpl::OnFocusIn(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusIn(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnFocusOut(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusOut(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnUpdateInputPosition(WEASEL_IPC_COMMAND uMsg,
                                        DWORD wParam,
                                        DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  /*
   * 移位标志 = 1bit == 0
   * height: 0~127 = 7bit
   * top:-2048~2047 = 12bit（有符号）
   * left:-2048~2047 = 12bit（有符号）
   *
   * 高解析度下：
   * 移位标志 = 1bit == 1
   * height: 0~254 = 7bit（舍弃低1位）
   * top: -4096~4094 = 12bit（有符号，舍弃低1位）
   * left: -4096~4094 = 12bit（有符号，舍弃低1位）
   */
  RECT rc;
  int hi_res = (wParam >> 31) & 0x01;
  rc.left = ((wParam & 0x7ff) - (wParam & 0x800)) << hi_res;
  rc.top = (((wParam >> 12) & 0x7ff) - ((wParam >> 12) & 0x800)) << hi_res;
  const int width = 6;
  int height = ((wParam >> 24) & 0x7f) << hi_res;
  rc.right = rc.left + width;
  rc.bottom = rc.top + height;

  // PhysicalToLogicalPointForPerMonitorDPI exists since Win8.1 (the process
  // gate in WeaselServer.cpp enforces it); if the lookup still failed, skip
  // the conversion instead of calling a null pointer.
  if (m_pPhysicalToLogicalPointForPerMonitorDPI) {
    POINT lt = {rc.left, rc.top};
    POINT rb = {rc.right, rc.bottom};
    m_pPhysicalToLogicalPointForPerMonitorDPI(NULL, &lt);
    m_pPhysicalToLogicalPointForPerMonitorDPI(NULL, &rb);
    rc = {lt.x, lt.y, rb.x, rb.y};
  }

  m_pRequestHandler->UpdateInputPosition(rc, lParam);
  return 0;
}

DWORD ServerImpl::OnStartMaintenance(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->StartMaintenance();
  return 0;
}

DWORD ServerImpl::OnEndMaintenance(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->EndMaintenance();
  return 0;
}

DWORD ServerImpl::OnCommitComposition(WEASEL_IPC_COMMAND uMsg,
                                      DWORD wParam,
                                      DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->CommitComposition(lParam, eat);
  }
  return 0;
}

DWORD ServerImpl::OnClearComposition(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->ClearComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnSelectCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                 DWORD wParam,
                                                 DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->SelectCandidateOnCurrentPage(wParam, lParam, eat);
  }
  return 0;
}

DWORD ServerImpl::OnHighlightCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                    DWORD wParam,
                                                    DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->HighlightCandidateOnCurrentPage(wParam, lParam, eat);
  }
  return 0;
}

DWORD ServerImpl::OnChangePage(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->ChangePage(wParam, lParam, eat);
  }
  return 0;
}

#define MAP_PIPE_MSG_HANDLE(__msg, __wParam, __lParam) \
  {                                                    \
    auto lParam = __lParam;                            \
    auto wParam = __wParam;                            \
    LRESULT _result = 0;                               \
    switch (__msg) {
#define PIPE_MSG_HANDLE(__msg, __func)       \
  case __msg:                                \
    _result = __func(__msg, wParam, lParam); \
    break;

#define END_MAP_PIPE_MSG_HANDLE(__result) \
  }                                       \
  __result = _result;                     \
  }

template <typename _Resp>
void ServerImpl::HandlePipeMessage(PipeMessage pipe_msg, _Resp resp) {
  DWORD result;

  MAP_PIPE_MSG_HANDLE(pipe_msg.Msg, pipe_msg.wParam, pipe_msg.lParam)
  PIPE_MSG_HANDLE(WEASEL_IPC_ECHO, OnEcho)
  PIPE_MSG_HANDLE(WEASEL_IPC_START_SESSION, OnStartSession)
  PIPE_MSG_HANDLE(WEASEL_IPC_END_SESSION, OnEndSession)
  PIPE_MSG_HANDLE(WEASEL_IPC_PROCESS_KEY_EVENT, OnKeyEvent)
  PIPE_MSG_HANDLE(WEASEL_IPC_SHUTDOWN_SERVER, OnShutdownServer)
  PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_IN, OnFocusIn)
  PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_OUT, OnFocusOut)
  PIPE_MSG_HANDLE(WEASEL_IPC_UPDATE_INPUT_POS, OnUpdateInputPosition)
  PIPE_MSG_HANDLE(WEASEL_IPC_START_MAINTENANCE, OnStartMaintenance)
  PIPE_MSG_HANDLE(WEASEL_IPC_END_MAINTENANCE, OnEndMaintenance)
  PIPE_MSG_HANDLE(WEASEL_IPC_COMMIT_COMPOSITION, OnCommitComposition)
  PIPE_MSG_HANDLE(WEASEL_IPC_CLEAR_COMPOSITION, OnClearComposition);
  PIPE_MSG_HANDLE(WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE,
                  OnSelectCandidateOnCurrentPage);
  PIPE_MSG_HANDLE(WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
                  OnHighlightCandidateOnCurrentPage);
  PIPE_MSG_HANDLE(WEASEL_IPC_CHANGE_PAGE, OnChangePage);
  PIPE_MSG_HANDLE(WEASEL_IPC_TRAY_COMMAND, OnCommand);
  END_MAP_PIPE_MSG_HANDLE(result);

  resp(result);
}

PipeServer::PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s)
    : PipeChannel(std::move(pn_cmd), s) {}

void PipeServer::Listen(ServerHandler const& handler) {
  for (;;) {
    boost::this_thread::interruption_point();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    try {
      pipe = _ConnectServerPipe(pname);
      _LaunchWorker(pipe, handler);
    } catch (...) {  // pipe errors and thread spawn failures alike
      FinalizePipeHandle(pipe);
      boost::this_thread::sleep_for(kListenRetryBackoff);
    }
    boost::this_thread::interruption_point();
  }
}

void PipeServer::WakeListener() {
  // A client-side connection to our own pipe makes ConnectNamedPipe return
  // so an interrupted listener can reach its interruption point and exit.
  HANDLE wake = ::CreateFileW(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              NULL, OPEN_EXISTING, 0, NULL);
  if (wake != INVALID_HANDLE_VALUE)
    ::CloseHandle(wake);
}

void PipeServer::DrainWorkers() {
  std::vector<std::shared_ptr<boost::thread>> workers;
  {
    std::lock_guard<std::mutex> lock(m_workers_mutex);
    for (auto& pair : m_workers) {
      // Abort pending reads and break the connection so blocked workers
      // fail their next I/O instead of waiting for their clients forever.
      ::CancelIoEx(pair.first, NULL);
      ::DisconnectNamedPipe(pair.first);
      workers.push_back(pair.second);
    }
    m_workers.clear();
  }
  for (auto& worker : workers)
    if (worker->joinable())
      worker->join();
}

void PipeServer::_LaunchWorker(HANDLE pipe, ServerHandler const& handler) {
  // The lock must cover thread creation AND registration: a worker whose
  // client dropped instantly may call _RemoveWorker the moment it starts,
  // and that removal must queue behind this critical section - otherwise it
  // erases nothing (the entry is not there yet) and the registration that
  // follows lingers in m_workers as an already-closed handle, which
  // DrainWorkers would later misuse for CancelIoEx/DisconnectNamedPipe.
  std::lock_guard<std::mutex> lock(m_workers_mutex);
  // Copy the handler into the worker: it must not reference anything
  // on this thread's stack.
  auto worker = std::make_shared<boost::thread>(
      [this, pipe, handler] { _ProcessPipeThread(pipe, handler); });
  m_workers[pipe] = std::move(worker);
}

void PipeServer::_RemoveWorker(HANDLE pipe) {
  std::lock_guard<std::mutex> lock(m_workers_mutex);
  m_workers.erase(pipe);
}

PipeServer::ServerRunner PipeServer::GetServerRunner(
    ServerHandler const& handler) {
  return [&handler, this]() { Listen(handler); };
}

void PipeServer::_ProcessPipeThread(HANDLE pipe, ServerHandler const& handler) {
  try {
    for (;;) {
      Res msg{};  // value-init: a truncated frame carries no stack garbage
      // Message mode: a short successful read means the client wrote a
      // truncated command frame - a well-formed frame is exactly one
      // message of sizeof(msg) bytes. Drop the connection via the
      // existing error path (unregister + close) instead of letting the
      // frame's garbage fields reach the dispatcher (D9).
      if (_ReceiveFrame(pipe, &msg, sizeof(msg)) != sizeof(msg))
        throw (DWORD)ERROR_MORE_DATA;
      handler(msg, [this, pipe](Msg resp) { _Send(pipe, resp); });
    }
  } catch (...) {
  }
  // Unregister before closing the handle so the registry never lends a
  // closed (possibly reused) handle value to DrainWorkers.
  _RemoveWorker(pipe);
  FinalizePipeHandle(pipe);
}

// weasel::Server

Server::Server() : m_pImpl(new ServerImpl) {}

Server::~Server() {
  if (m_pImpl)
    delete m_pImpl;
}

HWND Server::Start() {
  return m_pImpl->Start();
}

int Server::Stop() {
  return m_pImpl->Stop();
}

int Server::Run() {
  return m_pImpl->Run();
}

void Server::SetRequestHandler(RequestHandler* pHandler) {
  m_pImpl->SetRequestHandler(pHandler);
}

void Server::AddMenuHandler(UINT uID, CommandHandler handler) {
  m_pImpl->AddMenuHandler(uID, handler);
}

void Server::SetTrayRefreshCallback(std::function<void()> callback) {
  m_pImpl->SetTrayRefreshCallback(callback);
}

HWND Server::GetHWnd() {
  return m_pImpl->m_hWnd;
}
