#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <filesystem>
#include "ipc/shm_buffer.h"

namespace fs = std::filesystem;

const char* SOCKET_PATH = "/tmp/arthur.sock";

struct PluginInstance {
    pid_t guest_pid;
    std::string shm_name;
};

std::map<std::string, PluginInstance> active_plugins;

// Helper to find the actual .vst3 file for a plugin name
std::string find_windows_plugin(const std::string& name) {
    // Check common locations
    std::vector<std::string> search_paths = {
        "/home/dan/.wine/drive_c/Program Files/Common Files/VST3",
        "/home/dan/.wine/drive_c/Program Files (x86)/Common Files/VST3"
    };

    for (const auto& path : search_paths) {
        fs::path p = fs::path(path) / (name + ".vst3") / "Contents/x86_64-win" / (name + ".vst3");
        if (fs::exists(p)) return p.string();
        
        // Try without the inner folder structure (some plugins are just .vst3 files)
        p = fs::path(path) / (name + ".vst3");
        if (fs::exists(p)) return p.string();
    }
    return "";
}

void handle_signal(int sig) {
    for (auto const& [name, instance] : active_plugins) {
        kill(instance.guest_pid, SIGTERM);
    }
    unlink(SOCKET_PATH);
    exit(0);
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    unlink(SOCKET_PATH);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) return 1;
    if (listen(server_fd, 5) == -1) return 1;

    std::cout << ">>> Arthur Daemon Started. Standing by..." << std::endl;

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) continue;

        char buffer[1024];
        int bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            std::string cmd(buffer);
            
            if (cmd.find("LOAD") == 0) {
                size_t first_space = cmd.find(' ');
                size_t second_space = cmd.find(' ', first_space + 1);
                std::string shm_name = cmd.substr(first_space + 1, second_space - first_space - 1);
                std::string plugin_name = cmd.substr(second_space + 1);

                std::string win_plugin_path = find_windows_plugin(plugin_name);
                if (win_plugin_path.empty()) {
                    std::cerr << "[ERROR] Could not find Windows plugin: " << plugin_name << std::endl;
                } else {
                    std::cout << ">>> Spawning Guest for: " << plugin_name << std::endl;
                    pid_t pid = fork();
                    if (pid == 0) {
                        std::string guest_bin = "./build/arthur-guest.exe";
                        execlp("wine", "wine", guest_bin.c_str(), shm_name.c_str(), win_plugin_path.c_str(), NULL);
                        exit(1);
                    } else {
                        active_plugins[shm_name] = {pid, shm_name};
                    }
                }
            }
        }
        close(client_fd);
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0);
    }
    return 0;
}
