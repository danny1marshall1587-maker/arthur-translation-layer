#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <filesystem>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <sched.h>
#include <sstream>
#include <cstring>
#include "AudioIPC.h"

namespace fs = std::filesystem;

const char* SOCKET_PATH = "/tmp/arthur.sock";

void set_child_affinity() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    bool set_any = false;

    // Try to read isolated CPU cores from Linux sysfs
    std::ifstream infile("/sys/devices/system/cpu/isolated");
    std::string line;
    if (infile && std::getline(infile, line) && !line.empty()) {
        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.find('-') != std::string::npos) {
                // Range like "4-7"
                size_t dash = token.find('-');
                try {
                    int start = std::stoi(token.substr(0, dash));
                    int end = std::stoi(token.substr(dash + 1));
                    for (int cpu = start; cpu <= end; ++cpu) {
                        CPU_SET(cpu, &cpuset);
                        set_any = true;
                    }
                } catch (...) {}
            } else {
                // Single core like "4"
                try {
                    int cpu = std::stoi(token);
                    CPU_SET(cpu, &cpuset);
                    set_any = true;
                } catch (...) {}
            }
        }
    }

    // Fallback to default cores 4-7 if no isolated cores were parsed
    if (!set_any) {
        std::cout << "[DAEMON] No isolated cores found in sysfs. Falling back to default cores 4-7." << std::endl;
        for (int cpu = 4; cpu <= 7; ++cpu) {
            CPU_SET(cpu, &cpuset);
        }
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        std::cout << "[DAEMON] Spawned child process CPU affinity set successfully." << std::endl;
    } else {
        std::cerr << "[DAEMON WARNING] Failed to set child CPU affinity: " << strerror(errno) << std::endl;
    }
}

struct PluginInstance {
    pid_t guest_pid;
    std::string shm_name;
    std::string plugin_name;
    bool is_preloaded;
    bool is_assigned;
    std::shared_ptr<arthur::AudioTransport> transport;
};

std::map<std::string, PluginInstance> active_plugins;
std::mutex plugins_mutex;

// Helper to find the actual .vst3 file for a plugin name
std::string find_windows_plugin(const std::string& name) {
    std::string home = "/home/dan";
    if (const char* h = getenv("HOME")) {
        home = h;
    }

    // 1. Try reading the metadata file in ~/.vst3/<name>.vst3/arthur.metadata
    fs::path meta_path = fs::path(home) / ".vst3" / (name + ".vst3") / "arthur.metadata";
    if (fs::exists(meta_path)) {
        std::ifstream f(meta_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("guest_dll_path=") == 0) {
                std::string path = line.substr(15);
                if (fs::exists(path)) return path;
            }
        }
    }

    // 2. Check common locations
    std::vector<std::string> search_paths = {
        home + "/.wine/drive_c/Program Files/Common Files/VST3",
        home + "/.wine/drive_c/Program Files (x86)/Common Files/VST3"
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
    std::lock_guard<std::mutex> lock(plugins_mutex);
    for (auto const& [name, instance] : active_plugins) {
        kill(instance.guest_pid, SIGTERM);
    }
    unlink(SOCKET_PATH);
    exit(0);
}

void handle_client(int client_fd) {
    char buffer[1024];
    int bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);
    if (bytes <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes] = '\0';
    std::string cmd(buffer);
    std::cout << "[DAEMON] Received command: " << cmd << std::endl;

    if (cmd.find("LOAD") == 0 || cmd.find("GET_SHM") == 0) {
        std::string plugin_name = "";
        bool is_preloaded = false;
        if (cmd.find("LOAD") == 0) {
            size_t first_space = cmd.find(' ');
            size_t second_space = cmd.find(' ', first_space + 1);
            std::string shm_name = cmd.substr(first_space + 1, second_space - first_space - 1);
            plugin_name = cmd.substr(second_space + 1);
            is_preloaded = (shm_name.find("_slot_") != std::string::npos);
        } else {
            size_t first_space = cmd.find(' ');
            plugin_name = cmd.substr(first_space + 1);
            is_preloaded = false;
        }

        std::string target_shm = "ArthurAudioIPC";

        // Check if plugin is already running
        {
            std::lock_guard<std::mutex> lock(plugins_mutex);
            if (active_plugins.find(target_shm) != active_plugins.end()) {
                std::cout << ">>> Guest for " << plugin_name << " is already running. Reusing." << std::endl;
                active_plugins[target_shm].is_assigned = true;
                std::string resp = "SHM " + target_shm;
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }
        }

        std::string win_plugin_path = find_windows_plugin(plugin_name);
        if (win_plugin_path.empty()) {
            std::cerr << "[ERROR] Could not find Windows plugin: " << plugin_name << std::endl;
            std::string resp = "ERROR: Plugin not found";
            send(client_fd, resp.c_str(), resp.length(), 0);
            close(client_fd);
            return;
        }

        std::cout << ">>> Creating shared memory: /dev/shm/" << target_shm << std::endl;
        auto transport = std::make_shared<arthur::AudioTransport>();
        if (!transport->create(target_shm)) {
            std::cerr << "[ERROR] Daemon failed to create shared memory: " << target_shm << std::endl;
            std::string resp = "ERROR: Failed to create SHM";
            send(client_fd, resp.c_str(), resp.length(), 0);
            close(client_fd);
            return;
        }

        // Initialize state to STATE_ERROR to indicate not-yet-ready
        transport->get()->state.store(arthur::TransportState::STATE_ERROR, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        std::cout << ">>> Spawning guest for: " << plugin_name << std::endl;
        pid_t pid = fork();
        if (pid == 0) {
            std::string self_dir = ".";
            try {
                self_dir = fs::canonical("/proc/self/exe").parent_path().string();
            } catch (...) {}

            std::string guest_bin = self_dir + "/arthur-guest.exe";
            if (access("/app/bin/arthur-guest.exe", F_OK) == 0) {
                guest_bin = "/app/bin/arthur-guest.exe";
            }
            std::string real_guest_bin = self_dir + "/win_guest_agent.exe";
            if (access("/app/bin/win_guest_agent.exe", F_OK) == 0) {
                real_guest_bin = "/app/bin/win_guest_agent.exe";
            }
            
            std::string run_bin = real_guest_bin;
            if (access(run_bin.c_str(), F_OK) != 0) {
                run_bin = guest_bin;
            }

            execlp("wine", "wine", run_bin.c_str(), target_shm.c_str(), win_plugin_path.c_str(), NULL);
            exit(1);
        } else if (pid > 0) {
            // Watchdog loop for handshake connection
            auto start_time = std::chrono::steady_clock::now();
            bool connected = false;

            while (true) {
                if (transport->get()->state.load(std::memory_order_seq_cst) == arthur::TransportState::STATE_IDLE) {
                    connected = true;
                    break;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time
                ).count();
                if (elapsed >= 5000) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!connected) {
                std::cerr << "ERROR: Shared Memory IPC Connection Timeout" << std::endl;
                transport->get()->state.store(arthur::TransportState::STATE_ERROR, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                kill(pid, SIGKILL);
                int status;
                waitpid(pid, &status, WNOHANG);
                transport->detach();

                std::string resp = "ERROR: Shared Memory IPC Connection Timeout";
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                std::cout << ">>> [OK] Handshake completed successfully." << std::endl;
                {
                    std::lock_guard<std::mutex> lock(plugins_mutex);
                    active_plugins[target_shm] = {pid, target_shm, plugin_name, is_preloaded, true, transport};
                }
                std::string resp = "SHM " + target_shm;
                send(client_fd, resp.c_str(), resp.length(), 0);
            }
        } else {
            std::cerr << "[ERROR] Failed to fork guest process" << std::endl;
            std::string resp = "ERROR: Fork failed";
            send(client_fd, resp.c_str(), resp.length(), 0);
        }
    } else if (cmd.find("RELEASE") == 0) {
        size_t first_space = cmd.find(' ');
        std::string shm_name = cmd.substr(first_space + 1);
        std::string target_shm = "ArthurAudioIPC";

        std::lock_guard<std::mutex> lock(plugins_mutex);
        auto it = active_plugins.find(target_shm);
        if (it != active_plugins.end()) {
            if (it->second.is_preloaded) {
                std::cout << ">>> Releasing preloaded instance back to standby: " << target_shm << std::endl;
                it->second.is_assigned = false;
            } else {
                std::cout << ">>> Tearing down dynamic instance: " << target_shm << std::endl;
                kill(it->second.guest_pid, SIGTERM);
                active_plugins.erase(it);
            }
        }
        std::string resp = "RELEASED";
        send(client_fd, resp.c_str(), resp.length(), 0);
    } else if (cmd.find("UNLOAD") == 0) {
        size_t first_space = cmd.find(' ');
        std::string shm_name = cmd.substr(first_space + 1);
        std::string target_shm = "ArthurAudioIPC";

        std::lock_guard<std::mutex> lock(plugins_mutex);
        auto it = active_plugins.find(target_shm);
        if (it != active_plugins.end()) {
            std::cout << ">>> Killing Guest for SHM: " << target_shm << std::endl;
            kill(it->second.guest_pid, SIGTERM);
            active_plugins.erase(it);
        }
        std::string resp = "UNLOADED";
        send(client_fd, resp.c_str(), resp.length(), 0);
    }

    close(client_fd);
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

        // Process client connection asynchronously in a background thread
        std::thread([client_fd]() {
            handle_client(client_fd);
        }).detach();

        // Clean up zombie processes
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0);
    }
    return 0;
}
