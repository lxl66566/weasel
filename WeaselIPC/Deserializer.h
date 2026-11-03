#pragma once
#include <ResponseParser.h>
#include <WeaselUtility.h>
#include <boost/archive/text_wiarchive.hpp>
#include <functional>
#include <sstream>

namespace weasel {

// Parses one archive line from an IPC response. Runs on the host
// application's input thread: malformed data must be logged and dropped,
// never shown as a modal dialog (freezes all input) or thrown out.
// Archive construction included: it throws on bad header data too.
template <typename T>
void TryDeserialize(std::wstring const& value, T& t) {
  try {
    std::wstringstream ss(value);
    boost::archive::text_wiarchive ia(ss);
    ia >> t;
  } catch (const boost::archive::archive_exception& e) {
    LOG(ERROR) << "IPC response archive error: " << e.what();
  }
}
class Deserializer {
 public:
  typedef std::vector<std::wstring> KeyType;
  typedef std::shared_ptr<Deserializer> Ptr;
  typedef std::function<Ptr(ResponseParser* pTarget)> Factory;

  Deserializer(ResponseParser* pTarget) : m_pTarget(pTarget) {}
  virtual ~Deserializer() {}
  virtual void Store(KeyType const& key, std::wstring const& value) {}

  static void Initialize(ResponseParser* pTarget);
  static bool Require(std::wstring const& action, ResponseParser* pTarget);

 protected:
  ResponseParser* m_pTarget;
};

}  // namespace weasel
