#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <sys/socket.h>
  #include <unistd.h>
#endif
#include "control-protocol.h"
#include <cstring>

namespace Arthur {
    // Basic implementation for CI to pass
    void ControlProtocol::initialize() {
        // Stub
    }
}
