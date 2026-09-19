#include "rtc_base/win32_socket_init.h"
#include <cstdio>

namespace w = webrtc;
struct RuntimeNetworkPrefix {
#include "runtime_network_prefix.h"
};

int main() {
  // A fresh native process must not inherit Winsock initialization from a Go
  // runtime, application host, or another DLL before this bridge initializes.
  SOCKET before = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (before != INVALID_SOCKET || WSAGetLastError() != WSANOTINITIALISED) {
    if (before != INVALID_SOCKET)
      closesocket(before);
    std::fprintf(stderr, "runtime socket precondition was not a fresh process\n");
    return 1;
  }
  {
    RuntimeNetworkPrefix runtime;
    (void)runtime;
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
      std::fprintf(stderr, "runtime did not initialize Winsock before network construction: error=%d\n", WSAGetLastError());
      return 1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = bind(udp, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    closesocket(udp);
    if (bound == SOCKET_ERROR) {
      std::fprintf(stderr, "initialized runtime could not bind a loopback UDP socket\n");
      return 1;
    }
  }
  SOCKET after = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (after != INVALID_SOCKET || WSAGetLastError() != WSANOTINITIALISED) {
    if (after != INVALID_SOCKET)
      closesocket(after);
    std::fprintf(stderr, "runtime Winsock lifetime was not balanced\n");
    return 1;
  }
  std::puts("runtime: real Winsock startup before network construction and balanced lifetime PASS");
  return 0;
}
