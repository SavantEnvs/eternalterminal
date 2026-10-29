#include "FuzzableTerminalServer.hpp"

namespace et {

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  return FuzzableTerminalServer::initialize() ? 0 : 1;
}

// Raw bytes on the client-facing etserver socket (ConnectRequest handshake).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzableTerminalServer server;
  server.sendToClientEndpoint(data, size);
  return 0;
}

}  // namespace et
