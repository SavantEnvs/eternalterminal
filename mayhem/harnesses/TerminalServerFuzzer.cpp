#include "FuzzableTerminalServer.hpp"

namespace et {

static std::unique_ptr<FuzzableTerminalServer> sServer;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  sServer.reset(new FuzzableTerminalServer());
  return 0;
}

// Raw bytes on the client-facing etserver socket (ConnectRequest handshake).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  sServer->sendAndDrain(sServer->serverEndpoint, data, size);
  return 0;
}

}  // namespace et
