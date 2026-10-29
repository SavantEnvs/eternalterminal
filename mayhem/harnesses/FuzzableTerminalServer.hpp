#ifndef MAYHEM_FUZZABLE_TERMINAL_SERVER_HPP
#define MAYHEM_FUZZABLE_TERMINAL_SERVER_HPP

#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "TerminalServer.hpp"

namespace et {

// One etserver per fuzz input, listening on two unix sockets: the client-facing
// endpoint (ConnectRequest handshake) and the router endpoint (etterminal
// registration). Derived from test/FuzzableTerminalServer.hpp, except that the
// server is created and fully torn down for every input: nothing it learns from
// one input (client keys, connections, removed ids, unclaimed resumes, router
// registrations) reaches the next, and no server, handler or router thread is
// left running when the process exits (those threads log through the static
// el::base::Storage, which is destroyed during exit).
class FuzzableTerminalServer {
 public:
  // Call once from LLVMFuzzerInitialize. The log configuration is global but
  // set here once and never changed by an input.
  static bool initialize() {
    // Keep the log formatting code paths but do not flood stdout.
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToStandardOutput,
                                       "false");
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToFile, "false");

    const char* tmp = getenv("TMPDIR");
    const string base = string(tmp && *tmp ? tmp : "/tmp") + "/etserver_fuzzer_XXXXXX";
    if (base.size() >= PATH_MAX) {
      return false;
    }
    memcpy(pipeDirectory(), base.c_str(), base.size() + 1);
    if (mkdtemp(pipeDirectory()) == nullptr) {
      return false;
    }
    atexit(removePipeDirectory);
    return true;
  }

  FuzzableTerminalServer() {
    serverEndpoint.set_name(string(pipeDirectory()) + "/pipe_server");
    routerEndpoint.set_name(string(pipeDirectory()) + "/pipe_router");

    serverSocketHandler.reset(new PipeSocketHandler());
    pipeSocketHandler.reset(new PipeSocketHandler());
    terminalServer.reset(new TerminalServer(serverSocketHandler, serverEndpoint,
                                            pipeSocketHandler, routerEndpoint));
    terminalServerThread = thread([this]() { terminalServer->run(); });
  }

  // Stops the accept loop, joins the server thread and every client handler
  // thread, and closes all connections and the router.
  ~FuzzableTerminalServer() {
    terminalServer->shutdown();
    {
      // run() only looks at its halt flag after poll() returns; a throwaway
      // connection wakes it at once.
      PipeSocketHandler wakeHandler;
      const int fd = wakeHandler.connect(routerEndpoint);
      if (fd != -1) {
        wakeHandler.close(fd);
      }
    }
    terminalServerThread.join();
    terminalServer->shutdownConnections();
  }

  FuzzableTerminalServer(const FuzzableTerminalServer&) = delete;
  FuzzableTerminalServer& operator=(const FuzzableTerminalServer&) = delete;

  // Sends `data` as one client stream to the client-facing endpoint. No client
  // key is ever registered on a fresh server, so the handshake always ends with
  // the server closing the connection; wait for that, so the handler has
  // finished with the input when this returns.
  void sendToClientEndpoint(const uint8_t* data, size_t size) {
    PipeSocketHandler clientHandler;
    const int fd = sendAndHalfClose(clientHandler, serverEndpoint, data, size);
    if (fd == -1) {
      return;
    }
    for (;;) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      char sink[512];
      const ssize_t n = ::read(fd, sink, sizeof(sink));
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        break;
      }
    }
    clientHandler.close(fd);
  }

  // Sends `data` as one registration stream to the router endpoint. A valid
  // registration keeps its socket open, so there is no close to wait for; the
  // router instead bumps its pending-registration generation when it accepts
  // the connection and again when it has consumed or dropped it (every stream
  // ends in one or the other, since the client half-closes).
  void sendToRouterEndpoint(const uint8_t* data, size_t size) {
    PipeSocketHandler clientHandler;
    const int fd = sendAndHalfClose(clientHandler, routerEndpoint, data, size);
    if (fd == -1) {
      return;
    }
    for (;;) {
      uint64_t generation = ~uint64_t(0);
      terminalServer->terminalRouter->pendingRegistrationFds(generation);
      if (generation >= 2) {
        break;
      }
      std::this_thread::yield();
    }
    clientHandler.close(fd);
  }

 private:
  static char* pipeDirectory() {
    static char directory[PATH_MAX];
    return directory;
  }

  static void removePipeDirectory() {
    if (DIR* dir = opendir(pipeDirectory())) {
      while (const dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
          unlink((string(pipeDirectory()) + "/" + entry->d_name).c_str());
        }
      }
      closedir(dir);
    }
    rmdir(pipeDirectory());
  }

  static int sendAndHalfClose(PipeSocketHandler& clientHandler,
                              const SocketEndpoint& endpoint,
                              const uint8_t* data, size_t size) {
    const int fd = clientHandler.connect(endpoint);
    if (fd == -1) {
      return -1;
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
    return fd;
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
