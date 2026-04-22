#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <sys/socket.h>
  #include <unistd.h>
#endif
#include "ipc/control-protocol.h"
#include <iostream>

int main() {
    std::cout << "Arthur Guest Service Started" << std::endl;
    return 0;
}
