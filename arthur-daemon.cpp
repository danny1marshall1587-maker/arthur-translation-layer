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
#include "AudioIPC.h"

namespace fs = std::filesystem;

const char* SOCKET_PATH = "/tmp/arthur.sock";

struct PluginInstance {
    pid_t guest_pid;
    std::string shm_name;
    std::string plugin_name;
    bool is_preloaded;
    bool is_assigned;
    std::shared_ptr<arthur::AudioTransport> transport;
};

std::map<std::string, PluginInstance> active_plugins;

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

                bool is_preloaded = (shm_name.find("_slot_") != std::string::npos);

                // If already running (preloaded), update its status and skip spawn
                if (active_plugins.find(shm_name) != active_plugins.end()) {
                    std::cout << ">>> Guest for " << plugin_name << " on " << shm_name << " is already running. Updating status." << std::endl;
                    active_plugins[shm_name].is_preloaded = is_preloaded;
                    close(client_fd);
                    continue;
                }

                std::string win_plugin_path = find_windows_plugin(plugin_name);
                if (win_plugin_path.empty()) {
                    std::cerr << "[ERROR] Could not find Windows plugin: " << plugin_name << std::endl;
                } else {
                    std::cout << ">>> Spawning Guest for: " << plugin_name << " on SHM: " << shm_name << std::endl;

                    // Create the shared memory transport first so guest can open it
                    auto transport = std::make_shared<arthur::AudioTransport>();
                    if (!transport->create(shm_name)) {
                        std::cerr << "[ERROR] Daemon failed to create shared memory: " << shm_name << std::endl;
                    }

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

                        execlp("wine", "wine", run_bin.c_str(), shm_name.c_str(), win_plugin_path.c_str(), NULL);
                        exit(1);
                    } else {
                        active_plugins[shm_name] = {pid, shm_name, plugin_name, is_preloaded, false, transport};
                    }
                }
            } else if (cmd.find("GET_SHM") == 0) {
                size_t first_space = cmd.find(' ');
                std::string plugin_name = cmd.substr(first_space + 1);

                // Look for an unassigned preloaded instance of this plugin
                std::string target_shm = "";
                for (auto& [shm, inst] : active_plugins) {
                    if (inst.plugin_name == plugin_name && inst.is_preloaded && !inst.is_assigned) {
                        inst.is_assigned = true;
                        target_shm = shm;
                        std::cout << ">>> Auto-assigning preloaded instance: " << shm << " to DAW bridge." << std::endl;
                        break;
                    }
                }

                if (target_shm.empty()) {
                    // No preloaded instance available. Spawn a new dynamic one!
                    static int dynamic_counter = 0;
                    target_shm = "arthur_" + plugin_name + "_dyn_" + std::to_string(++dynamic_counter);

                    std::string win_plugin_path = find_windows_plugin(plugin_name);
                    if (win_plugin_path.empty()) {
                        std::cerr << "[ERROR] Could not find Windows plugin for dynamic loading: " << plugin_name << std::endl;
                        std::string resp = "ERROR";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                        close(client_fd);
                        continue;
                    }

                    auto transport = std::make_shared<arthur::AudioTransport>();
                    if (!transport->create(target_shm)) {
                        std::cerr << "[ERROR] Daemon failed to create dynamic shared memory: " << target_shm << std::endl;
                        std::string resp = "ERROR";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                        close(client_fd);
                        continue;
                    }

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
                    } else {
                        active_plugins[target_shm] = {pid, target_shm, plugin_name, false, true, transport};
                        std::cout << ">>> Spawned dynamic guest for " << plugin_name << " on SHM: " << target_shm << std::endl;
                    }
                }

                // Send the SHM name back to the client
                std::string resp = "SHM " + target_shm;
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else if (cmd.find("RELEASE") == 0) {
                size_t first_space = cmd.find(' ');
                std::string shm_name = cmd.substr(first_space + 1);

                auto it = active_plugins.find(shm_name);
                if (it != active_plugins.end()) {
                    if (it->second.is_preloaded) {
                        std::cout << ">>> Releasing preloaded instance back to standby: " << shm_name << std::endl;
                        it->second.is_assigned = false;
                    } else {
                        std::cout << ">>> Tearing down dynamic instance: " << shm_name << std::endl;
                        kill(it->second.guest_pid, SIGTERM);
                        active_plugins.erase(it);
                    }
                }
            } else if (cmd.find("UNLOAD") == 0) {
                size_t first_space = cmd.find(' ');
                std::string shm_name = cmd.substr(first_space + 1);
                
                auto it = active_plugins.find(shm_name);
                if (it != active_plugins.end()) {
                    std::cout << ">>> Killing Guest for SHM: " << shm_name << std::endl;
                    kill(it->second.guest_pid, SIGTERM);
                    active_plugins.erase(it);
                }
            }
        }
        close(client_fd);
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0);
    }
    return 0;
}
