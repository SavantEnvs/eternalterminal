#ifndef MAYHEM_FUZZABLE_TERMINAL_SERVER_HPP
#define MAYHEM_FUZZABLE_TERMINAL_SERVER_HPP

#include <sys/socket.h>
#include <unistd.h>

#include "TerminalServer.hpp"

namespace et {

// One long-lived in-process etserver listening on two unix sockets: the
// client-facing endpoint (ConnectRequest handshake) and the router endpoint
// (etterminal registration). Derived from test/FuzzableTerminalServer.hpp.
class FuzzableTerminalServer {
 public:
  FuzzableTerminalServer() {
    // Keep the log formatting code paths but do not flood stdout.
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToStandardOutput,
                                       "false");
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToFile, "false");

    serverSocketHandler.reset(new PipeSocketHandler());
    pipeSocketHandler.reset(new PipeSocketHandler());

    const char* tmp = getenv("TMPDIR");
    string tmpPath = string(tmp && *tmp ? tmp : "/tmp") + "/etserver_fuzzer_XXXXXX";
    const string pipeDirectory = string(mkdtemp(&tmpPath[0]));

    serverEndpoint.set_name(pipeDirectory + "/pipe_server");
    routerEndpoint.set_name(pipeDirectory + "/pipe_router");

    terminalServer.reset(new TerminalServer(serverSocketHandler, serverEndpoint,
                                            pipeSocketHandler, routerEndpoint));
    terminalServerThread = thread([this]() { terminalServer->run(); });
  }

  ~FuzzableTerminalServer() {
    terminalServer->shutdown();
    terminalServerThread.join();
  }

  // Sends `data` as one client stream to `endpoint`, half-closes, then waits for
  // the server to finish with the connection (it closes its side).
  void sendAndDrain(const SocketEndpoint& endpoint, const uint8_t* data,
                    size_t size) {
    PipeSocketHandler clientHandler;
    const int fd = clientHandler.connect(endpoint);
    if (fd == -1) {
      return;
    }
    size_t off = 0;
    while (off < size) {
      const ssize_t n = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
      if (n <= 0) {
        break;
      }
      off += size_t(n);
    }
    ::shutdown(fd, SHUT_WR);
    char sink[512];
    while (::read(fd, sink, sizeof(sink)) > 0) {
    }
    clientHandler.close(fd);
  }

  std::shared_ptr<PipeSocketHandler> serverSocketHandler;
  std::shared_ptr<PipeSocketHandler> pipeSocketHandler;

  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;

  shared_ptr<TerminalServer> terminalServer;
  thread terminalServerThread;
};

}  // namespace et

#endif
