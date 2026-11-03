#pragma once
#include <WeaselUtility.h>
#include <string>
#include <memory>
#include <windows.h>
#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/thread.hpp>
#include <boost/thread/tss.hpp>

namespace weasel {

/* Release a named pipe handle: disconnect the server end (no-op for a
 * client end) and close the kernel handle. Idempotent. */
inline void FinalizePipeHandle(HANDLE& p) {
  if (p != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
  p = INVALID_HANDLE_VALUE;
}

/* Owns one thread's pipe handle. boost's default TSS cleanup is a plain
 * delete, so the handle used to live in a raw HANDLE that was never
 * closed; the owner destructor releases it when the owning thread exits
 * without an explicit Disconnect (server-side handles live in local
 * variables and are finalized by their own paths). */
struct PipeHandleOwner {
  PipeHandleOwner() = default;
  ~PipeHandleOwner() { Finalize(); }
  PipeHandleOwner(const PipeHandleOwner&) = delete;
  PipeHandleOwner& operator=(const PipeHandleOwner&) = delete;

  void Finalize() { FinalizePipeHandle(handle); }

  HANDLE handle = INVALID_HANDLE_VALUE;
};

class PipeChannelBase {
 public:
  using Stream = boost::interprocess::wbufferstream;

  struct ChannelContext {
    std::unique_ptr<char[]> buffer;
    std::unique_ptr<Stream> write_stream;
    bool has_body;
    // bumped each time a fresh response body is received into buffer;
    // lets callers tell an unparsed response from a stale one (request
    // staging or body-less replies reuse the same buffer)
    UINT64 resp_serial;

    ChannelContext(size_t bs)
        : buffer(std::make_unique<char[]>(bs)),
          has_body(false),
          resp_serial(0) {}
  };

  PipeChannelBase(std::wstring&& pn_cmd, size_t bs, SECURITY_ATTRIBUTES* s);
  ~PipeChannelBase();

 protected:
  /* To ensure connection before operation */
  bool _Ensure();
  /* Connect pipe as client */
  HANDLE _Connect(const wchar_t* name);
  /* To reconnect message pipe */
  void _Reconnect();
  /* Try to connect for one time */
  HANDLE _TryConnect();
  size_t _WritePipe(HANDLE p, size_t s, char* b);
  void _Receive(HANDLE pipe, LPVOID msg, size_t rec_len);
  /* Message-mode receive of one fixed-size frame into msg, returning the
   * byte count ReadFile actually delivered. Same contract as _Receive
   * (a frame longer than rec_len stages its trailing body into the
   * shared receive buffer via the ERROR_MORE_DATA path), plus the count
   * so callers can reject short frames: in PIPE_READMODE_MESSAGE a short
   * successful read means the peer wrote a truncated message in one
   * write - a broken or hostile peer, never a legitimate exchange. */
  DWORD _ReceiveFrame(HANDLE pipe, LPVOID msg, size_t rec_len) {
    DWORD lread;
    BOOL success = ::ReadFile(pipe, msg, rec_len, &lread, NULL);
    if (success) {
      _GetContext()->has_body = false;
      return lread;
    }
    DWORD err = ::GetLastError();
    if (err != ERROR_MORE_DATA)
      throw err;
    // The frame is longer than rec_len: its header now fills msg and
    // the trailing body is still pending. Stage the body into the
    // shared receive buffer, exactly as _Receive does.
    auto ctx = _GetContext();
    memset(ctx->buffer.get(), 0, buff_size);
    success = ::ReadFile(pipe, ctx->buffer.get(), buff_size, &lread, NULL);
    if (!success)
      throw ::GetLastError();
    ctx->resp_serial++;  // a fresh response body now occupies the buffer
    ctx->has_body = false;
    return static_cast<DWORD>(rec_len);  // the header was delivered whole
  }
  /* Try to get a connection from client */
  HANDLE _ConnectServerPipe(std::wstring& pn);
  /* Create a new server-side pipe instance */
  HANDLE _CreateServerPipe(std::wstring& pn);
  /* Wait for a client to connect to an instance */
  HANDLE _AcceptServerPipe(HANDLE pipe);
  inline bool _Invalid(HANDLE p) const { return p == INVALID_HANDLE_VALUE; }

  PipeHandleOwner* _GetPipeHandle() const {
    if (!hpipe_ptr.get()) {
      hpipe_ptr.reset(new PipeHandleOwner());
    }
    return hpipe_ptr.get();
  }

  ChannelContext* _GetContext() const {
    if (!context.get()) {
      context.reset(new ChannelContext(buff_size));
    }
    return context.get();
  }

 protected:
  std::wstring pname;
  // Thread-local pipe handle; its owner closes it when the thread exits
  mutable boost::thread_specific_ptr<PipeHandleOwner> hpipe_ptr;
  const size_t buff_size;
  // Thread-local context for buffer and state
  mutable boost::thread_specific_ptr<ChannelContext> context;

 private:
  /* Security attributes */
  SECURITY_ATTRIBUTES* sa;
};

/* Pipe based IPC channel */
template <typename _TyMsg,
          typename _TyRes = DWORD,
          size_t _MsgSize = sizeof(_TyMsg),
          size_t _ResSize = sizeof(_TyRes)>
class PipeChannel : public PipeChannelBase {
 public:
  /* Type definitions */

  using Ptr = std::shared_ptr<PipeChannel>;
  using UPtr = std::unique_ptr<PipeChannel>;
  using Msg = _TyMsg;
  using Res = _TyRes;

  enum class ChannalCommand { NEW_MSG_PIPE, REFRESH };

 public:
  PipeChannel(std::wstring&& pn_cmd,
              SECURITY_ATTRIBUTES* s = NULL,
              size_t bs = 64 * 1024)
      : PipeChannelBase(std::move(pn_cmd), bs, s) {}

 public:
  /* Common pipe operations */

  bool Connect() { return _Ensure(); }
  bool Connected() const {
    return !_Invalid(_GetPipeHandle()->handle);
  }
  // serial of the latest response body received on this thread; changes
  // iff the buffer now holds an unread response
  UINT64 ResponseSerial() { return _GetContext()->resp_serial; }
  void Disconnect() { _GetPipeHandle()->Finalize(); }

  /* Write data to buffer */

  template <typename _TyWrite>
  void Write(_TyWrite cnt) {
    _GetContext()->has_body = true;
    _BufferWriteStream() << cnt;
  }

  /* Write data to buffer */
  template <typename _TyWrite>
  PipeChannel& operator<<(_TyWrite cnt) {
    Write(cnt);
    return *this;
  }

  _TyRes Transact(Msg& msg) {
    PipeHandleOwner* phandle = _GetPipeHandle();
    if (!_Ensure()) {
      // Server unreachable: drop any staged body so the next request starts
      // clean instead of appending to the leftover one.
      ClearBufferStream();
      throw (DWORD)ERROR_FILE_NOT_FOUND;  // server unreachable
    }
    try {
      _Send(phandle->handle, msg);
      return _ReceiveResponse();
    } catch (...) {
      // The connection died mid-request. Drop the request (it may already
      // have been delivered) and re-establish so the next call can proceed.
      // A staged-but-unsent body is dropped too: keeping it would make the
      // next START_SESSION carry two client info blocks, or let it ride
      // along with the next body-less command.
      ClearBufferStream();
      _Reconnect();
      throw;
    }
  }

  void ClearBufferStream() {
    auto ctx = _GetContext();
    ctx->has_body = false;
    if (ctx->write_stream != nullptr) {
      ctx->write_stream.reset(nullptr);
    }
  }

  char* SendBuffer() const { return _GetContext()->buffer.get() + _MsgSize; }

  // Request bodies land at buffer[0] via _Receive's ERROR_MORE_DATA path:
  // the fixed-size header is consumed into the caller's variable first.
  char* ReceiveBuffer() const { return _GetContext()->buffer.get(); }

  template <typename _TyHandler>
  bool HandleResponseData(_TyHandler const& handler) {
    if (!handler) {
      return false;
    }

    // Use whole buffer to receive data in client
    return handler((LPWSTR)_GetContext()->buffer.get(),
                   (UINT)(buff_size * sizeof(char) / sizeof(wchar_t)));
  }

 protected:
  void _Send(HANDLE pipe, Msg& msg) {
    auto ctx = _GetContext();
    char* pbuff = ctx->buffer.get();
    DWORD lwritten = 0;

    *reinterpret_cast<Msg*>(pbuff) = msg;
    size_t body_bytes = 0;
    if (ctx->has_body && ctx->write_stream) {
      std::streampos pos = ctx->write_stream->tellp();
      if (pos == std::streampos(-1)) {
        // The staged body overran the fixed send buffer (wbufferstream
        // failbit): sending now would deliver the header without the body,
        // losing it silently. Fail the transaction explicitly and drop the
        // poisoned staging state instead.
        ClearBufferStream();
        LOG(ERROR) << "IPC request body exceeds the send buffer ("
                   << (buff_size - _MsgSize) << " bytes), dropping it";
        throw (DWORD)ERROR_MORE_DATA;
      }
      body_bytes = static_cast<size_t>(pos) * sizeof(wchar_t);
    }
    // body_bytes is bounded by the buffer capacity, so data_sz never
    // exceeds buff_size here
    size_t data_sz = ctx->has_body ? (_MsgSize + body_bytes) : _MsgSize;

    // No resend on failure: a request may already have been delivered, and
    // duplicating it would process one keystroke twice.
    _WritePipe(pipe, data_sz, pbuff);
    ClearBufferStream();
  }

  _TyRes _ReceiveResponse() {
    _TyRes result{};  // value-init: a truncated reply is never data
    // A short frame can only come from a broken peer: never surface a
    // partially-written response as valid data (D9, symmetric to the
    // server-side check in PipeServer::_ProcessPipeThread).
    if (_ReceiveFrame(_GetPipeHandle()->handle, &result, sizeof(result)) !=
        sizeof(result))
      throw (DWORD)ERROR_MORE_DATA;
    return result;
  }

  Stream& _BufferWriteStream() {
    auto ctx = _GetContext();
    if (ctx->write_stream == nullptr) {
      char* pbuff = (char*)ctx->buffer.get() + _MsgSize;
      memset(pbuff, 0, buff_size - _MsgSize);
      ctx->write_stream =
          std::make_unique<Stream>((wchar_t*)pbuff, _SendBufferSizeW());
    }
    return *ctx->write_stream;
  }

 private:
  inline size_t _SendBufferSizeW() const {
    return (buff_size - _MsgSize) * sizeof(char) / sizeof(wchar_t);
  }

  inline size_t _ReceiveBufferSizeW() const {
    return (buff_size - _ResSize) * sizeof(char) / sizeof(wchar_t);
  }
};
};  // namespace weasel
