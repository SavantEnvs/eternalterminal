#ifndef MAYHEM_FUZZABLE_TERMINAL_SERVER_HPP
#define MAYHEM_FUZZABLE_TERMINAL_SERVER_HPP

#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "TerminalServer.hpp"

namespace et {

// One etserver per fuzz input, listening on two unix sockets: the client-facing
// endpoint (ConnectRequest handshake) and the router endpoint. Derived from
// test/FuzzableTerminalServer.hpp, except that the server is created and fully
// torn down for every input (including the router's listening socket, which the
// server never closes itself), so no descriptor survives from one input to the
// next.
class FuzzableTerminalServer {
 public:
  // Call once from LLVMFuzzerInitialize. The log configuration is global but
  // set here once and never changed by an input.
  static bool initialize() {
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToStandardOutput,
                                       "false");
    el::Loggers::reconfigureAllLoggers(el::ConfigurationType::ToFile, "false");

    // An etserver that serves many clients holds many descriptors; let the
    // process use as many as its hard limit allows, as a daemon would.
    rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
      rl.rlim_cur = rl.rlim_max;
      setrlimit(RLIMIT_NOFILE, &rl);
    }

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

  // run() looks at its halt flag every 100 ms; after it returns, the base
  // class joins the client handler threads and closes the client-facing
  // listener, and the router's listener is closed here.
  ~FuzzableTerminalServer() {
    terminalServer->shutdown();
    terminalServerThread.join();
    terminalServer->ServerConnection::shutdown();
    pipeSocketHandler->stopListening(routerEndpoint);
  }

  FuzzableTerminalServer(const FuzzableTerminalServer&) = delete;
  FuzzableTerminalServer& operator=(const FuzzableTerminalServer&) = delete;

  // Input layout: byte 0 is the number of idle connections to hold open, in
  // units of 5, then the rest is sent as one client stream. A remote client
  // can hold as many idle connections to etserver as the server's descriptor
  // limit allows; every one of them is a descriptor in the server process.
  void sendToClientEndpoint(const uint8_t* data, size_t size) {
    const size_t idleCount = size_t(data[0]) * 5;
    std::vector<int> idle;
    for (size_t i = 0; i < idleCount; ++i) {
      const int fd = connectRaw();
      if (fd == -1) {
        break;
      }
      idle.push_back(fd);
    }

    const int fd = connectRaw();
    if (fd != -1) {
      size_t off = 1;
      while (off < size) {
        const ssize_t n = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
        if (n <= 0) {
          break;
        }
        off += size_t(n);
      }
      ::shutdown(fd, SHUT_WR);
    }

    // The server's handler pool is small and each idle connection occupies a
    // handler until it closes, so release them before waiting for the verdict
    // on the real stream.
    for (int idleFd : idle) {
      ::close(idleFd);
    }
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
    ::close(fd);
  }

 private:
  int connectRaw() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
      return -1;
    }
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, serverEndpoint.name().c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

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

  std::shared_ptr<PipeSocketHandler> serverSocketHandler;
  std::shared_ptr<PipeSocketHandler> pipeSocketHandler;

  SocketEndpoint serverEndpoint;
  SocketEndpoint routerEndpoint;

  shared_ptr<TerminalServer> terminalServer;
  thread terminalServerThread;
};

}  // namespace et

#endif
