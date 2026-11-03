// TestPipeChannel.cpp : integration tests for the weasel named pipe IPC layer.
//
// Uses a private pipe name (never the live WeaselNamedPipe) so the tests can
// run while the real WeaselServer is active.

#include <windows.h>
#include <atlbase.h>
#include <wtl/atlapp.h>

#include <WeaselIPC.h>
#include <PipeChannel.h>
#include <PipeServer.h>

#include <boost/thread.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>

CAppModule _Module;

static int g_failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::cout << "[FAIL] " << what << std::endl;
  } else {
    std::cout << "[ok] " << what << std::endl;
  }
}

static std::wstring unique_pipe_name(const wchar_t* tag) {
  std::wstring name = weasel::GetPipeName();
  name += L"_UnitTest_";
  name += tag;
  wchar_t pid[16];
  swprintf_s(pid, L"%u", GetCurrentProcessId());
  name += pid;
  return name;
}

using ClientChannel = weasel::PipeChannel<weasel::PipeMessage>;

/* Client-side connect with startup grace: the server listener thread may
 * not have created its pipe instance yet when we first try. */
static bool connect_with_retry(ClientChannel& client, int ms = 2000) {
  for (int waited = 0; waited < ms; waited += 25) {
    if (client.Connect())
      return true;
    Sleep(25);
  }
  return false;
}

/* Smoke test: request/response roundtrip through Listen + Transact */
static void test_roundtrip() {
  std::wstring name = unique_pipe_name(L"smoke");
  weasel::PipeServer* server =
      new weasel::PipeServer(std::wstring(name));
  auto handler = [](weasel::PipeMessage msg,
                    weasel::PipeServer::Respond resp) {
    resp(msg.wParam + 1);
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "smoke: connect");
  weasel::PipeMessage req{WEASEL_IPC_ECHO, 41, 0};
  DWORD resp = client.Transact(req);
  check(resp == 42, "smoke: roundtrip reply");
}

/* Poll pred until it holds or the timeout elapses */
template <typename Pred>
static bool wait_until(Pred pred, int timeout_ms, int step_ms = 10) {
  for (int waited = 0; waited < timeout_ms; waited += step_ms) {
    if (pred())
      return true;
    Sleep(step_ms);
  }
  return pred();
}

/* Expose protected seams for deterministic tests */
class ExposedServer : public weasel::PipeServer {
 public:
  using PipeServer::PipeServer;
  HANDLE ExposedCreate() { return _CreateServerPipe(pname); }
  HANDLE ExposedAccept(HANDLE pipe) { return _AcceptServerPipe(pipe); }
  void ExposedReceive(HANDLE pipe, LPVOID msg, size_t len) {
    _Receive(pipe, msg, len);
  }
  /* number of currently registered connection workers */
  size_t ExposedWorkerCount() {
    std::lock_guard<std::mutex> lock(m_workers_mutex);
    return m_workers.size();
  }
};

/* 1.1: a client that connects between CreateNamedPipe and ConnectNamedPipe
 * (ConnectNamedPipe failing with ERROR_PIPE_CONNECTED) must not be dropped */
static void test_pipe_connected_race() {
  std::wstring name = unique_pipe_name(L"e535");
  ExposedServer server{std::wstring(name)};

  HANDLE instance = server.ExposedCreate();
  check(instance != INVALID_HANDLE_VALUE, "1.1: create instance");

  HANDLE client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_EXISTING, 0, NULL);
  check(client != INVALID_HANDLE_VALUE, "1.1: early client connect");

  bool accepted = false;
  try {
    accepted = (server.ExposedAccept(instance) == instance);
  } catch (DWORD err) {
    std::cout << "  accept threw error " << err << std::endl;
  }
  check(accepted, "1.1: ERROR_PIPE_CONNECTED accepted as connected");

  if (accepted) {
    weasel::PipeMessage req{WEASEL_IPC_ECHO, 7, 0};
    DWORD written = 0;
    check(::WriteFile(client, &req, sizeof(req), &written, NULL) &&
              written == sizeof(req),
          "1.1: client sends on racing connection");
    weasel::PipeMessage got{};
    try {
      server.ExposedReceive(instance, &got, sizeof(got));
      check(got.Msg == WEASEL_IPC_ECHO && got.wParam == 7,
            "1.1: server reads message on racing connection");
    } catch (DWORD err) {
      check(false, "1.1: server reads message on racing connection");
      std::cout << "  receive threw error " << err << std::endl;
    }
  }

  ::CloseHandle(client);
  ::DisconnectNamedPipe(instance);
  ::CloseHandle(instance);
}

/* 1.2: a client that stops reading must not block the server from serving
 * other clients (regression for FlushFileBuffers under the api mutex) */
static void test_hung_client_does_not_freeze_others() {
  std::wstring name = unique_pipe_name(L"flush");
  weasel::PipeServer* server =
      new weasel::PipeServer(std::wstring(name));
  std::mutex api_mutex;  // mirrors ServerImpl's request serialization
  auto handler = [&api_mutex](weasel::PipeMessage msg,
                              weasel::PipeServer::Respond resp) {
    std::lock_guard<std::mutex> lock(api_mutex);
    resp(msg.wParam + 1);
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  // client A: raw pipe client that sends a request and never reads the reply
  HANDLE a = INVALID_HANDLE_VALUE;
  for (int waited = 0; waited < 2000; waited += 25) {
    a = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                      OPEN_EXISTING, 0, NULL);
    if (a != INVALID_HANDLE_VALUE)
      break;
    Sleep(25);
  }
  check(a != INVALID_HANDLE_VALUE, "1.2: hung client connected");
  weasel::PipeMessage req{WEASEL_IPC_ECHO, 1, 0};
  DWORD written = 0;
  ::WriteFile(a, &req, sizeof(req), &written, NULL);

  // client B: ordinary request/response, must complete promptly
  ClientChannel b{std::wstring(name)};
  check(connect_with_retry(b), "1.2: B connect");
  bool b_ok = false;
  boost::thread b_thread([&b, &b_ok] {
    weasel::PipeMessage r{WEASEL_IPC_ECHO, 41, 0};
    try {
      b_ok = (b.Transact(r) == 42);
    } catch (...) {
    }
  });
  bool served_in_time = b_thread.timed_join(boost::posix_time::seconds(3));
  check(served_in_time, "1.2: B served while A hangs");
  check(b_ok, "1.2: B got correct reply");
  if (!served_in_time)
    b_thread.detach();  // worker is stuck on the hung client; drop it
  ::CloseHandle(a);
}

/* 1.3: the server must see the request body from its first line on
 * (START_SESSION bodies historically lost their first 6 characters) */
static void test_start_session_body_offset() {
  std::wstring name = unique_pipe_name(L"body");
  ExposedServer* server = new ExposedServer(std::wstring(name));
  std::promise<std::wstring> body_promise;
  auto body_future = body_promise.get_future();
  bool captured = false;
  auto handler = [server, &body_promise, &captured](
                     weasel::PipeMessage msg,
                     weasel::PipeServer::Respond resp) {
    if (msg.Msg == WEASEL_IPC_START_SESSION && !captured) {
      captured = true;
      body_promise.set_value(std::wstring((LPWSTR)server->ReceiveBuffer()));
    }
    resp(7);
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "1.3: connect");
  client << L"action=session\n";
  client << L"session.client_app=notepad.exe\n";
  client << L".\n";
  weasel::PipeMessage req{WEASEL_IPC_START_SESSION, 0, 0};
  DWORD resp = client.Transact(req);
  check(resp == 7, "1.3: start session reply");

  check(body_future.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready,
        "1.3: body captured");
  if (body_future.valid()) {
    std::wstring body = body_future.get();
    check(body.rfind(L"action=session", 0) == 0,
          "1.3: body starts with 'action=session' line");
    check(body.find(L"session.client_app=notepad.exe") != std::wstring::npos,
          "1.3: client_app line intact");
  }
}

/* Expose the client-side connect seam */
class ExposedClient : public weasel::PipeChannel<weasel::PipeMessage> {
 public:
  using PipeChannel::PipeChannel;
  HANDLE ExposedConnect(const wchar_t* name) { return _Connect(name); }
};

/* N3: when every pipe instance stays busy forever, _Connect must fail
 * within a bounded time instead of hanging the caller's thread */
static void test_connect_bounded_wait() {
  std::wstring name = unique_pipe_name(L"busy");
  // a single-instance pipe that stays occupied by a holder client
  HANDLE instance =
      ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                         1, 1024, 1024, 0, NULL);
  check(instance != INVALID_HANDLE_VALUE, "N3: create single instance");

  HANDLE holder = INVALID_HANDLE_VALUE;
  for (int waited = 0; waited < 2000 && holder == INVALID_HANDLE_VALUE;
       waited += 25) {
    holder = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (holder == INVALID_HANDLE_VALUE)
      Sleep(25);
  }
  check(holder != INVALID_HANDLE_VALUE, "N3: holder client connected");
  ::ConnectNamedPipe(instance, NULL);  // completes the server side (535 ok)

  ExposedClient victim{std::wstring(name)};
  bool threw = false;
  boost::thread t([&] {
    try {
      victim.ExposedConnect(name.c_str());
    } catch (...) {
      threw = true;
    }
  });
  bool returned = t.timed_join(boost::posix_time::seconds(15));
  check(returned, "N3: _Connect returns (no infinite busy-wait)");
  check(threw, "N3: _Connect reports failure");
  if (!returned)
    t.detach();

  ::CloseHandle(holder);
  ::CloseHandle(instance);
}

/* 1.5: ordered shutdown must interrupt + join the listener and drain every
 * worker within a bounded time, even with an idle client connection whose
 * worker is blocked in ReadFile */
static void test_ordered_shutdown() {
  std::wstring name = unique_pipe_name(L"sd");
  auto* server = new weasel::PipeServer(std::wstring(name));
  auto handler = [](weasel::PipeMessage msg,
                    weasel::PipeServer::Respond resp) { resp(1); };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "1.5: client connect");
  weasel::PipeMessage req{WEASEL_IPC_ECHO, 0, 0};
  check(client.Transact(req) == 1, "1.5: roundtrip");
  // the connection worker is now blocked in ReadFile awaiting the next
  // request that will never come

  listener.interrupt();
  server->WakeListener();
  check(listener.timed_join(boost::posix_time::seconds(5)),
        "1.5: listener exits after interrupt + wake");

  boost::thread drainer([server] { server->DrainWorkers(); });
  check(drainer.timed_join(boost::posix_time::seconds(5)),
        "1.5: idle workers cancelled and joined");
  delete server;
  check(true, "1.5: server destroyed after all pipe threads joined");

  bool failed_fast = false;
  try {
    client.Transact(req);  // broken pipe must throw, not hang
  } catch (...) {
    failed_fast = true;
  }
  check(failed_fast, "1.5: dead connection fails fast");
}

/* 1.6: a dropped connection must fail the in-flight request exactly once
 * (no resend), and the channel must recover on the next call */
static void test_transact_recovery_without_resend() {
  std::wstring name = unique_pipe_name(L"rec");
  auto* server = new weasel::PipeServer(std::wstring(name));
  std::atomic<int> echo_count{0};
  auto handler = [&echo_count](weasel::PipeMessage msg,
                               weasel::PipeServer::Respond resp) {
    if (msg.Msg == WEASEL_IPC_ECHO)
      ++echo_count;
    resp(echo_count.load());
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "1.6: connect");
  weasel::PipeMessage req{WEASEL_IPC_ECHO, 0, 0};
  check(client.Transact(req) == 1, "1.6: first request served");
  check(echo_count.load() == 1, "1.6: exactly one request delivered");

  server->DrainWorkers();  // server drops the connection mid-life

  bool threw = false;
  try {
    client.Transact(req);
  } catch (...) {
    threw = true;
  }
  check(threw, "1.6: request on dropped connection fails");
  check(echo_count.load() == 1, "1.6: failed request not resent");

  // Transact reconnected internally; the next request must succeed
  bool recovered = false;
  try {
    recovered = (client.Transact(req) == 2);
  } catch (...) {
  }
  check(recovered, "1.6: next request succeeds after auto-reconnect");
  check(echo_count.load() == 2, "1.6: recovery delivered exactly once");
}

/* N5: a mouse-click command must get its response body back in the same
 * transaction (SelectCandidateOnCurrentPage historically sent no body, so
 * the commit text only arrived with the next keystroke) */
static void test_command_response_body() {
  std::wstring name = unique_pipe_name(L"sel");
  auto* server = new weasel::PipeServer(std::wstring(name));
  // mirrors ServerImpl's eat lambda feeding the handler's response lines
  auto handler = [server](weasel::PipeMessage msg,
                          weasel::PipeServer::Respond resp) {
    if (msg.Msg == WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE) {
      *server << L"commit=你好\n";
      *server << L"status.composing=0\n";
    }
    resp(0);
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "N5: connect");
  weasel::PipeMessage req{WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE, 0, 1};
  client.Transact(req);

  // the client parses the click's own response, as DoEditSession does
  std::wstring commit, status_line;
  weasel::ResponseHandler parse = [&](LPWSTR data, UINT) -> bool {
    std::wstring body(data);
    size_t pos = body.find(L"commit=");
    if (pos != std::wstring::npos)
      commit = body.substr(pos + 7, body.find(L'\n', pos) - pos - 7);
    pos = body.find(L"status.composing=");
    if (pos != std::wstring::npos)
      status_line = body.substr(pos + 17, 1);
    return true;
  };
  client.HandleResponseData(parse);
  check(commit == L"你好", "N5: commit text arrives with the click response");
  check(status_line == L"0", "N5: status says not composing");
}

/* B17: a listener whose pipe instance can never be created must back off
 * between retries instead of busy-spinning a core, and shutdown (interrupt
 * + wake) must still complete promptly */
static void test_failed_listener_backs_off() {
  // an unprefixed name makes CreateNamedPipe fail with ERROR_INVALID_PARAMETER
  // on every iteration (deterministically reproduced; precondition checked)
  std::wstring bad_name = L"weasel_ut_badpipe_";
  wchar_t pid[16];
  swprintf_s(pid, L"%u", GetCurrentProcessId());
  bad_name += pid;
  ExposedServer server{std::wstring(bad_name)};
  check(server.ExposedCreate() == INVALID_HANDLE_VALUE,
        "B17: unprefixed pipe name makes CreateNamedPipe fail");

  std::atomic<DWORD> listener_tid{0};
  auto handler = [](weasel::PipeMessage msg,
                    weasel::PipeServer::Respond resp) { resp(1); };
  boost::thread listener([&] {
    listener_tid = GetCurrentThreadId();
    server.Listen(handler);
  });
  while (listener_tid.load() == 0)
    Sleep(5);

  // open the thread handle before it exits: needed for GetThreadTimes later
  HANDLE hthread =
      ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, listener_tid.load());
  check(hthread != NULL, "B17: opened listener thread for CPU query");

  const int kSpinWindowMs = 600;
  Sleep(kSpinWindowMs);
  listener.interrupt();
  server.WakeListener();
  check(listener.timed_join(boost::posix_time::seconds(2)),
        "B17: failing listener exits promptly on interrupt");

  if (hthread) {
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    if (::GetThreadTimes(hthread, &ft_create, &ft_exit, &ft_kernel,
                         &ft_user)) {
      auto to_ms = [](const FILETIME& ft) -> long long {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart / 10000;  // 100ns units -> ms
      };
      long long cpu_ms = to_ms(ft_kernel) + to_ms(ft_user);
      // a busy spin burns most of the spin window; a 50ms backoff burns ~0
      check(cpu_ms < kSpinWindowMs / 2,
            "B17: failing listener backs off instead of busy-spinning");
      std::cout << "  listener CPU during " << kSpinWindowMs << "ms window: "
                << cpu_ms << "ms" << std::endl;
    } else {
      check(false, "B17: GetThreadTimes on listener thread");
    }
    ::CloseHandle(hthread);
  }
}

/* B18: a connection worker may exit (_RemoveWorker + close its pipe) at any
 * instant after its thread starts; its registration must never be overtaken
 * by that removal or m_workers keeps a closed handle. Fast-dropping clients
 * churn the register/remove window; afterwards the registry must be empty
 * and the server must still serve new connections. */
static void test_fast_disconnect_leaves_no_stale_worker() {
  std::wstring name = unique_pipe_name(L"b18");
  ExposedServer* server = new ExposedServer(std::wstring(name));
  auto handler = [](weasel::PipeMessage msg,
                    weasel::PipeServer::Respond resp) { resp(msg.wParam + 1); };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  const int kRounds = 30;
  int connected = 0;
  for (int i = 0; i < kRounds; ++i) {
    HANDLE c = INVALID_HANDLE_VALUE;
    for (int waited = 0; waited < 2000 && c == INVALID_HANDLE_VALUE;
         waited += 10) {
      c = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
      if (c == INVALID_HANDLE_VALUE)
        Sleep(10);
    }
    if (c == INVALID_HANDLE_VALUE)
      break;
    ++connected;
    // drop immediately: the worker fails its first read and exits at once
    ::CloseHandle(c);
  }
  check(connected == kRounds, "B18: fast-drop clients all connected");

  // every churned worker must have unregistered itself; a stale entry would
  // be an already-closed handle lingering in m_workers
  check(wait_until([&] { return server->ExposedWorkerCount() == 0; }, 5000),
        "B18: no stale worker entries after fast disconnects");

  // the server keeps serving well-behaved clients after the churn
  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "B18: server accepts new connection");
  weasel::PipeMessage req{WEASEL_IPC_ECHO, 41, 0};
  bool served = false;
  try {
    served = (client.Transact(req) == 42);
  } catch (...) {
  }
  check(served, "B18: new client served after churn");

  listener.interrupt();
  server->WakeListener();
  check(listener.timed_join(boost::posix_time::seconds(5)),
        "B18: listener exits");
  boost::thread drainer([server] { server->DrainWorkers(); });
  check(drainer.timed_join(boost::posix_time::seconds(5)),
        "B18: DrainWorkers completes");
  delete server;
}

/* B16: a staged body larger than the send buffer must fail the transaction
 * explicitly. The wbufferstream failbit used to make tellp() return -1, so
 * body_bytes became 0 and only the header was sent: the body (client info on
 * START_SESSION, a whole cinfo line) was silently lost. */
static void test_oversized_body_fails_loudly() {
  std::wstring name = unique_pipe_name(L"b16");
  auto* server = new weasel::PipeServer(std::wstring(name));
  std::atomic<int> requests{0};
  auto handler = [&requests](weasel::PipeMessage msg,
                             weasel::PipeServer::Respond resp) {
    ++requests;
    resp(msg.wParam + 1);
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "B16: connect");

  // stage a body past the send capacity ((buff_size - header) / 2 wchars)
  const size_t capacity_w =
      (64 * 1024 - sizeof(weasel::PipeMessage)) / sizeof(wchar_t);
  client << std::wstring(capacity_w + 64, L'x');

  bool threw = false;
  try {
    weasel::PipeMessage req{WEASEL_IPC_ECHO, 41, 0};
    client.Transact(req);
  } catch (...) {
    threw = true;
  }
  check(threw, "B16: oversized body fails the transaction");
  check(requests.load() == 0, "B16: no header-only request delivered");

  // the channel must stay usable for normal-sized bodies afterwards
  client << L"body-ok\n";
  weasel::PipeMessage req2{WEASEL_IPC_ECHO, 41, 0};
  bool served = false;
  try {
    served = (client.Transact(req2) == 42);
  } catch (...) {
  }
  check(served, "B16: channel usable after oversized body");

  listener.interrupt();
  server->WakeListener();
  check(listener.timed_join(boost::posix_time::seconds(5)),
        "B16: listener exits");
  boost::thread drainer([server] { server->DrainWorkers(); });
  check(drainer.timed_join(boost::posix_time::seconds(5)), "B16: drain");
  delete server;
}

/* B15: a failed Transact must leave the thread's channel state clean.
 * A staged body that never made it onto the pipe used to survive the
 * failure, so the next START_SESSION appended to it and delivered two
 * client info blocks. Both failure modes are exercised: server
 * unreachable (_Ensure throws before the try block) and a connection
 * dropped mid-flight (_WritePipe/_Receive fail inside it). */
static void test_failed_transact_leaves_no_staged_body() {
  std::wstring name = unique_pipe_name(L"b15");
  auto client_info = [](const wchar_t* app) {
    std::wstring body = L"action=session\nsession.client_app=";
    body += app;
    body += L".exe\nsession.client_type=tsf\n.\n";
    return body;
  };

  auto* server1 = new weasel::PipeServer(std::wstring(name));
  std::mutex bodies_mutex;
  std::vector<std::wstring> bodies;  // every START_SESSION body received
  auto handler = [server1, &bodies_mutex, &bodies](
                     weasel::PipeMessage msg,
                     weasel::PipeServer::Respond resp) {
    if (msg.Msg == WEASEL_IPC_START_SESSION) {
      std::lock_guard<std::mutex> lock(bodies_mutex);
      bodies.push_back(std::wstring((LPWSTR)server1->ReceiveBuffer()));
    }
    resp(7);
  };
  boost::thread listener1([server1, &handler] { server1->Listen(handler); });

  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "B15: connect");
  const std::wstring block_a = client_info(L"appA");
  client << block_a;
  weasel::PipeMessage start{WEASEL_IPC_START_SESSION, 0, 0};
  check(client.Transact(start) == 7, "B15: first session request served");
  {
    std::lock_guard<std::mutex> lock(bodies_mutex);
    check(bodies.size() == 1 && bodies[0] == block_a,
          "B15: first body delivered intact");
  }

  // --- server unreachable: _Ensure throws before anything is sent ---
  listener1.interrupt();
  server1->WakeListener();
  check(listener1.timed_join(boost::posix_time::seconds(5)),
        "B15: server1 listener exits");
  {
    boost::thread drainer([server1] { server1->DrainWorkers(); });
    check(drainer.timed_join(boost::posix_time::seconds(5)),
          "B15: server1 workers drained");
  }
  delete server1;
  client.Disconnect();  // force the next Transact through _Ensure

  client << client_info(L"appB");
  bool threw_unreachable = false;
  try {
    client.Transact(start);
  } catch (...) {
    threw_unreachable = true;
  }
  check(threw_unreachable, "B15: request with server down fails");

  auto* server2 = new weasel::PipeServer(std::wstring(name));
  auto handler2 = [server2, &bodies_mutex, &bodies](
                      weasel::PipeMessage msg,
                      weasel::PipeServer::Respond resp) {
    if (msg.Msg == WEASEL_IPC_START_SESSION) {
      std::lock_guard<std::mutex> lock(bodies_mutex);
      bodies.push_back(std::wstring((LPWSTR)server2->ReceiveBuffer()));
    }
    resp(7);
  };
  boost::thread listener2([server2, &handler2] { server2->Listen(handler2); });

  const std::wstring block_c = client_info(L"appC");
  bool retried = false;
  for (int waited = 0; waited < 3000 && !retried; waited += 25) {
    // restage each attempt, as ClientImpl::StartSession rewrites its client
    // info for every try; a failed Transact drops the staged body
    client << block_c;
    try {
      retried = (client.Transact(start) == 7);
    } catch (...) {
      Sleep(25);  // server2's listener may not have created its pipe yet
    }
  }
  check(retried, "B15: session retry succeeds after server restart");
  {
    std::lock_guard<std::mutex> lock(bodies_mutex);
    check(bodies.size() == 2 && bodies[1] == block_c,
          "B15: retry carries exactly one client info block");
  }

  // --- connection dropped mid-flight: _WritePipe/_Receive throw ---
  server2->DrainWorkers();

  client << client_info(L"appD");
  bool threw_dropped = false;
  try {
    client.Transact(start);
  } catch (...) {
    threw_dropped = true;
  }
  check(threw_dropped, "B15: request on dropped connection fails");

  const std::wstring block_e = client_info(L"appE");
  bool retried2 = false;
  for (int waited = 0; waited < 3000 && !retried2; waited += 25) {
    client << block_e;  // restage, mirroring StartSession retry semantics
    try {
      retried2 = (client.Transact(start) == 7);
    } catch (...) {
      Sleep(25);
    }
  }
  check(retried2, "B15: session retry succeeds after dropped connection");
  {
    std::lock_guard<std::mutex> lock(bodies_mutex);
    check(bodies.size() == 3 && bodies[2] == block_e,
          "B15: retry after drop carries exactly one client info block");
  }

  listener2.interrupt();
  server2->WakeListener();
  check(listener2.timed_join(boost::posix_time::seconds(5)),
        "B15: server2 listener exits");
  {
    boost::thread drainer([server2] { server2->DrainWorkers(); });
    check(drainer.timed_join(boost::posix_time::seconds(5)),
          "B15: server2 workers drained");
  }
  delete server2;
}

/* B6: a client thread that exits without Disconnect() must have its
 * thread-local pipe handle closed by the TSS owner. The old raw-HANDLE TSS
 * was merely deleted, leaking one kernel handle per thread and keeping the
 * peer's connection worker blocked in ReadFile until process exit. */
static void test_thread_exit_closes_pipe_handle() {
  std::wstring name = unique_pipe_name(L"b6");
  ExposedServer* server = new ExposedServer(std::wstring(name));
  auto handler = [](weasel::PipeMessage msg,
                    weasel::PipeServer::Respond resp) { resp(msg.wParam + 1); };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  // one shared channel on the main thread, as one ClientImpl serves every
  // UI thread of the host process; only the per-thread TSS handle differs
  ClientChannel client{std::wstring(name)};

  // warmup: bring the listener up, then let its worker settle so the
  // handle count baseline excludes startup churn
  check(connect_with_retry(client), "B6: warmup connect");
  weasel::PipeMessage req{WEASEL_IPC_ECHO, 1, 0};
  check(client.Transact(req) == 2, "B6: warmup roundtrip");
  client.Disconnect();
  check(wait_until([&] { return server->ExposedWorkerCount() == 0; }, 5000),
        "B6: warmup worker gone");

  DWORD handles_before = 0;
  check(::GetProcessHandleCount(::GetCurrentProcess(), &handles_before),
        "B6: handle count baseline");

  const int kThreads = 8;
  {
    boost::thread_group group;
    for (int i = 0; i < kThreads; ++i) {
      group.create_thread([&] {
        // each thread gets its own TSS connection on the shared channel
        if (connect_with_retry(client)) {
          weasel::PipeMessage r{WEASEL_IPC_ECHO, 41, 0};
          try {
            client.Transact(r);
          } catch (...) {
          }
        }
        // exit without Disconnect: the TSS owner must close the handle
      });
    }
    group.join_all();
  }  // destroy the group so its own resources leave the handle count

  // a leaked-open client handle keeps its worker blocked in ReadFile, so
  // the registry returning to empty proves every client end was closed
  check(wait_until([&] { return server->ExposedWorkerCount() == 0; }, 5000),
        "B6: workers released after client threads exited");

  // direct leak evidence: each leaked TSS handle would keep count +1
  bool handles_released = wait_until([&] {
    DWORD now = 0;
    return ::GetProcessHandleCount(::GetCurrentProcess(), &now) &&
           now <= handles_before;
  }, 5000);
  check(handles_released, "B6: thread exit closed its pipe handle");

  listener.interrupt();
  server->WakeListener();
  check(listener.timed_join(boost::posix_time::seconds(5)),
        "B6: listener exits");
  boost::thread drainer([server] { server->DrainWorkers(); });
  check(drainer.timed_join(boost::posix_time::seconds(5)), "B6: drain");
  delete server;
}

/* D9: a truncated command frame - fewer bytes than sizeof(PipeMessage)
 * written as one message - reads short but successfully in message mode;
 * the connection must be dropped via the existing error path instead of
 * dispatching the frame's garbage fields. */
static void test_truncated_frame_drops_connection() {
  std::wstring name = unique_pipe_name(L"d9");
  ExposedServer* server = new ExposedServer(std::wstring(name));
  std::atomic<int> handled{0};
  auto handler = [&handled](weasel::PipeMessage msg,
                            weasel::PipeServer::Respond resp) {
    ++handled;
    resp(msg.wParam + 1);
  };
  boost::thread listener([server, &handler] { server->Listen(handler); });

  // raw client that connects and writes a truncated frame as one message
  HANDLE c = INVALID_HANDLE_VALUE;
  bool written = false;
  for (int waited = 0; waited < 2000 && !written; waited += 25) {
    if (c == INVALID_HANDLE_VALUE) {
      c = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
      if (c == INVALID_HANDLE_VALUE) {
        Sleep(25);
        continue;
      }
    }
    // the listener may not have accepted yet; retry until the write lands
    weasel::PipeMessage req{WEASEL_IPC_ECHO, 41, 0};
    DWORD written_bytes = 0;
    written = ::WriteFile(c, &req, sizeof(req) - 4, &written_bytes, NULL) &&
              written_bytes == sizeof(req) - 4;
    if (!written)
      Sleep(25);
  }
  check(written, "D9: truncated frame written");

  // the worker unregisters itself as it drops the connection
  check(wait_until([&] { return server->ExposedWorkerCount() == 0; }, 5000),
        "D9: short frame dropped the connection");
  check(handled.load() == 0, "D9: truncated frame never dispatched");

  // the server keeps serving well-formed clients afterwards
  ClientChannel client{std::wstring(name)};
  check(connect_with_retry(client), "D9: server accepts new client");
  weasel::PipeMessage r{WEASEL_IPC_ECHO, 41, 0};
  bool served = false;
  try {
    served = (client.Transact(r) == 42);
  } catch (...) {
  }
  check(served, "D9: well-formed client served after the drop");

  listener.interrupt();
  server->WakeListener();
  check(listener.timed_join(boost::posix_time::seconds(5)),
        "D9: listener exits");
  boost::thread drainer([server] { server->DrainWorkers(); });
  check(drainer.timed_join(boost::posix_time::seconds(5)), "D9: drain");
  delete server;
}

int main() {
  test_roundtrip();
  test_pipe_connected_race();
  test_hung_client_does_not_freeze_others();
  test_start_session_body_offset();
  test_connect_bounded_wait();
  test_ordered_shutdown();
  test_transact_recovery_without_resend();
  test_command_response_body();
  test_failed_listener_backs_off();
  test_fast_disconnect_leaves_no_stale_worker();
  test_oversized_body_fails_loudly();
  test_failed_transact_leaves_no_staged_body();
  test_thread_exit_closes_pipe_handle();
  test_truncated_frame_drops_connection();
  std::cout << (g_failures ? "FAILED: " : "PASSED: ") << g_failures
            << " failure(s)" << std::endl;
  // The listener threads are still blocked in ConnectNamedPipe on purpose;
  // skip static destructors and exit immediately.
  std::cout.flush();
  ExitProcess(g_failures ? 1 : 0);
}
