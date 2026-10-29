#include "FuzzableTerminalServer.hpp"

namespace et {

extern "C" int LLVMFuzzerInitialize(int*, char***) {
  return FuzzableTerminalServer::initialize() ? 0 : 1;
}

// Raw bytes on the etserver router socket (etterminal registration).
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzableTerminalServer server;
  server.sendToRouterEndpoint(data, size);
  return 0;
}

}  // namespace et
