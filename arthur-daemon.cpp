#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <filesystem>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <sched.h>
#include <sstream>
#include <cstring>
#include "AudioIPC.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Socket path: prefer XDG_RUNTIME_DIR (per-user, tmpfs-backed) over /tmp
// ---------------------------------------------------------------------------
static std::string get_socket_path() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) return std::string(xdg) + "/arthur.sock";
    return "/tmp/arthur.sock";
}

static std::string g_socket_path;

// Global CPU affinity variables computed in parent (for async-signal-safety)
static cpu_set_t g_cpuset;
static bool g_cpuset_valid = false;

void detect_isolated_cores() {
    CPU_ZERO(&g_cpuset);
    bool set_any = false;

    // Try to read isolated CPU cores from Linux sysfs
    std::ifstream infile("/sys/devices/system/cpu/isolated");
    std::string line;
    if (infile && std::getline(infile, line) && !line.empty()) {
        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.find('-') != std::string::npos) {
                size_t dash = token.find('-');
                try {
                    int start = std::stoi(token.substr(0, dash));
                    int end = std::stoi(token.substr(dash + 1));
                    for (int cpu = start; cpu <= end; ++cpu) {
                        CPU_SET(cpu, &g_cpuset);
                        set_any = true;
                    }
                } catch (...) {}
            } else {
                try {
                    int cpu = std::stoi(token);
                    CPU_SET(cpu, &g_cpuset);
                    set_any = true;
                } catch (...) {}
            }
        }
    }

    // Fallback: detect available cores and use the upper half
    if (!set_any) {
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        if (nprocs > 4) {
            std::cout << "[DAEMON] No isolated cores found. Using upper half of " << nprocs << " cores." << std::endl;
            for (long cpu = nprocs / 2; cpu < nprocs; ++cpu) {
                CPU_SET((int)cpu, &g_cpuset);
            }
            set_any = true;
        }
    }

    g_cpuset_valid = set_any;
}

// VDC Slot Allocator Configuration
static constexpr size_t VDC_STRIDE = 2 * 1024 * 1024; // 2MB stride
static constexpr size_t VDC_TOTAL_SIZE = 256 * 1024 * 1024; // 256MB file
static constexpr int VDC_MAX_SLOTS = VDC_TOTAL_SIZE / VDC_STRIDE; // 128 slots

static std::string g_vdc_shm_name = "arthur_vdc_multiplex";
static int g_vdc_shm_fd = -1;
static char* g_vdc_base_ptr = nullptr;
static bool g_vdc_slots_active[VDC_MAX_SLOTS] = {false};
static std::string g_vdc_slots_assigned_shm[VDC_MAX_SLOTS] = {""};
static std::mutex g_vdc_mutex;

bool initialize_vdc_multiplex() {
    std::lock_guard<std::mutex> lock(g_vdc_mutex);
    std::string name = "/" + g_vdc_shm_name;
    g_vdc_shm_fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (g_vdc_shm_fd < 0) {
        std::cerr << "[DAEMON ERROR] Failed to create VDC SHM multiplex file: " << name << std::endl;
        return false;
    }
    if (ftruncate(g_vdc_shm_fd, VDC_TOTAL_SIZE) < 0) {
        std::cerr << "[DAEMON ERROR] Failed to ftruncate VDC SHM multiplex file." << std::endl;
        close(g_vdc_shm_fd);
        g_vdc_shm_fd = -1;
        shm_unlink(name.c_str());
        return false;
    }
    g_vdc_base_ptr = (char*)mmap(nullptr, VDC_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_vdc_shm_fd, 0);
    if (g_vdc_base_ptr == MAP_FAILED) {
        std::cerr << "[DAEMON ERROR] Failed to mmap VDC SHM base pointer." << std::endl;
        g_vdc_base_ptr = nullptr;
        close(g_vdc_shm_fd);
        g_vdc_shm_fd = -1;
        shm_unlink(name.c_str());
        return false;
    }
    
    std::memset(g_vdc_base_ptr, 0, VDC_TOTAL_SIZE);
    std::cout << "[DAEMON OK] Initialized 256MB VDC Multiplex memory space (" << VDC_MAX_SLOTS << " slots)." << std::endl;
    return true;
}

void cleanup_vdc_multiplex() {
    std::lock_guard<std::mutex> lock(g_vdc_mutex);
    if (g_vdc_base_ptr) {
        munmap(g_vdc_base_ptr, VDC_TOTAL_SIZE);
        g_vdc_base_ptr = nullptr;
    }
    if (g_vdc_shm_fd >= 0) {
        close(g_vdc_shm_fd);
        g_vdc_shm_fd = -1;
        std::string name = "/" + g_vdc_shm_name;
        shm_unlink(name.c_str());
    }
}

int allocate_vdc_slot(const std::string& shm_name) {
    std::lock_guard<std::mutex> lock(g_vdc_mutex);
    for (int i = 0; i < VDC_MAX_SLOTS; ++i) {
        if (g_vdc_slots_active[i] && g_vdc_slots_assigned_shm[i] == shm_name) {
            return i;
        }
    }
    for (int i = 0; i < VDC_MAX_SLOTS; ++i) {
        if (!g_vdc_slots_active[i]) {
            g_vdc_slots_active[i] = true;
            g_vdc_slots_assigned_shm[i] = shm_name;
            return i;
        }
    }
    return -1;
}

void release_vdc_slot(const std::string& shm_name) {
    std::lock_guard<std::mutex> lock(g_vdc_mutex);
    for (int i = 0; i < VDC_MAX_SLOTS; ++i) {
        if (g_vdc_slots_active[i] && g_vdc_slots_assigned_shm[i] == shm_name) {
            g_vdc_slots_active[i] = false;
            g_vdc_slots_assigned_shm[i] = "";
            break;
        }
    }
}

struct PluginInstance {
    pid_t guest_pid;
    std::string shm_name;
    std::string plugin_name;
    bool is_preloaded;
    bool is_assigned;
    std::shared_ptr<arthur::AudioTransport> transport;
    int slot_idx;
};

std::map<std::string, PluginInstance> active_plugins;
std::mutex plugins_mutex;

// Atomic shutdown flag — safe to set from signal handler
static std::atomic<bool> g_shutdown{false};
static int g_server_fd = -1;

// Helper to find the actual .vst3 file for a plugin name
std::string find_windows_plugin(const std::string& name) {
    const char* h = getenv("HOME");
    if (!h) {
        std::cerr << "[ERROR] HOME environment variable not set." << std::endl;
        return "";
    }
    std::string home(h);

    // Support custom Wine prefixes
    const char* wp = getenv("WINEPREFIX");
    std::string wine_prefix = wp ? wp : (home + "/.wine");

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
        wine_prefix + "/drive_c/Program Files/Common Files/VST3",
        wine_prefix + "/drive_c/Program Files (x86)/Common Files/VST3"
    };

    for (const auto& path : search_paths) {
        fs::path p = fs::path(path) / (name + ".vst3") / "Contents/x86_64-win" / (name + ".vst3");
        if (fs::exists(p)) return p.string();
        
        p = fs::path(path) / (name + ".vst3");
        if (fs::exists(p)) return p.string();
    }
    return "";
}

// Signal handler: ONLY sets atomic flag.
void handle_signal(int sig) {
    g_shutdown.store(true, std::memory_order_relaxed);
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
    }
}

// SIGCHLD background reaper to clean up child processes and avoid zombies
void handle_sigchld(int sig) {
    int saved_errno = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0);
    errno = saved_errno;
}

// Graceful shutdown: called from main thread ONLY
static void perform_cleanup() {
    std::lock_guard<std::mutex> lock(plugins_mutex);
    for (auto& [name, instance] : active_plugins) {
        kill(instance.guest_pid, SIGTERM);
    }
    
    // Reap all with a timeout
    auto start = std::chrono::steady_clock::now();
    while (!active_plugins.empty() && 
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000)) {
        for (auto it = active_plugins.begin(); it != active_plugins.end(); ) {
            int status;
            pid_t res = waitpid(it->second.guest_pid, &status, WNOHANG);
            if (res > 0 || (res == -1 && errno == ECHILD)) {
                it = active_plugins.erase(it);
            } else {
                ++it;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Force kill remaining
    for (auto& [name, instance] : active_plugins) {
        std::cerr << "[DAEMON WARNING] Guest PID " << instance.guest_pid << " did not exit on SIGTERM. Escalating to SIGKILL." << std::endl;
        kill(instance.guest_pid, SIGKILL);
        int status;
        waitpid(instance.guest_pid, &status, 0);
    }
    active_plugins.clear();
    unlink(g_socket_path.c_str());
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
    
    // Trim trailing carriage returns, newlines, and spaces
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n' || cmd.back() == ' ' || cmd.back() == '\t')) {
        cmd.pop_back();
    }
    std::cout << "[DAEMON] Received command: " << cmd << std::endl;

    if (cmd.find("LOAD") == 0 || cmd.find("GET_SHM") == 0) {
        std::string plugin_name = "";
        std::string target_shm = "";
        bool is_preloaded = false;
        if (cmd.find("LOAD") == 0) {
            size_t first_space = cmd.find(' ');
            size_t second_space = cmd.find(' ', first_space + 1);
            if (first_space == std::string::npos || second_space == std::string::npos) {
                std::string resp = "ERROR: Malformed LOAD command";
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }
            target_shm = cmd.substr(first_space + 1, second_space - first_space - 1);
            plugin_name = cmd.substr(second_space + 1);
            is_preloaded = (target_shm.find("_slot_") != std::string::npos);
        } else {
            size_t first_space = cmd.find(' ');
            if (first_space == std::string::npos) {
                std::string resp = "ERROR: Malformed GET_SHM command";
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }
            plugin_name = cmd.substr(first_space + 1);
            target_shm = "arthur_" + plugin_name;
            is_preloaded = false;
        }

        // Sanitize SHM name to prevent path traversal vulnerability
        for (char c : target_shm) {
            if (!isalnum(c) && c != '_' && c != '-' && c != '.') {
                std::cerr << "[DAEMON ERROR] Invalid character in SHM name: " << target_shm << std::endl;
                std::string resp = "ERROR: Invalid SHM name";
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }
        }

        // Check if plugin is already running
        {
            std::lock_guard<std::mutex> lock(plugins_mutex);
            if (active_plugins.find(target_shm) != active_plugins.end()) {
                std::cout << ">>> Guest for " << plugin_name << " is already running. Reusing." << std::endl;
                active_plugins[target_shm].is_assigned = true;
                std::string resp = "VDC_SLOT " + g_vdc_shm_name + " " + std::to_string(active_plugins[target_shm].slot_idx) + " " + std::to_string(VDC_STRIDE);
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

        // Allocate a VDC multiplexed slot
        int slot_idx = allocate_vdc_slot(target_shm);
        if (slot_idx < 0) {
            std::cerr << "[ERROR] Daemon failed to allocate VDC slot for: " << target_shm << std::endl;
            std::string resp = "ERROR: VDC Slot Allocator Full";
            send(client_fd, resp.c_str(), resp.length(), 0);
            close(client_fd);
            return;
        }

        std::cout << ">>> Allocated VDC Slot " << slot_idx << " for " << target_shm << std::endl;
        
        // Map the VDC layout for this slot inside the daemon
        arthur::AudioSharedMemory* layout = (arthur::AudioSharedMemory*)(g_vdc_base_ptr + slot_idx * VDC_STRIDE);
        
        // Placement-new to properly initialize version, default values, and atomics
        new (layout) arthur::AudioSharedMemory();

        // Initialize state to STATE_ERROR to indicate not-yet-ready
        layout->state.store(arthur::TransportState::STATE_ERROR, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Precalculate paths and binary strings in parent BEFORE fork (async-signal-safety)
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

        std::string so_path = run_bin + ".so";
        bool is_so = (access(so_path.c_str(), F_OK) == 0);

        std::string run_bin_c = run_bin;
        std::string win_plugin_path_c = win_plugin_path;
        std::string slot_idx_str = std::to_string(slot_idx);
        std::string stride_str = std::to_string(VDC_STRIDE);

        std::cout << ">>> Spawning guest for: " << plugin_name << " (Slot: " << slot_idx << ")" << std::endl;
        pid_t pid = fork();
        if (pid == 0) {
            // --- CHILD PROCESS (RT Safe, no heap allocs) ---
            if (g_cpuset_valid) {
                sched_setaffinity(0, sizeof(cpu_set_t), &g_cpuset);
            }

            if (is_so) {
                execlp(run_bin_c.c_str(), run_bin_c.c_str(), g_vdc_shm_name.c_str(), win_plugin_path_c.c_str(), slot_idx_str.c_str(), stride_str.c_str(), NULL);
            } else {
                execlp("wine", "wine", run_bin_c.c_str(), g_vdc_shm_name.c_str(), win_plugin_path_c.c_str(), slot_idx_str.c_str(), stride_str.c_str(), NULL);
            }
            _exit(1);
        } else if (pid > 0) {
            // --- PARENT PROCESS ---
            // Watchdog loop for handshake connection
            auto start_time = std::chrono::steady_clock::now();
            bool connected = false;

            while (true) {
                auto s = layout->state.load(std::memory_order_seq_cst);
                if (s == arthur::TransportState::STATE_IDLE) {
                    connected = true;
                    break;
                }

                // Check if child has exited prematurely
                int status;
                pid_t res = waitpid(pid, &status, WNOHANG);
                if (res > 0 || (res == -1 && errno == ECHILD)) {
                    std::cerr << "[DAEMON ERROR] Guest process exited prematurely during handshake." << std::endl;
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
                layout->state.store(arthur::TransportState::STATE_ERROR, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                kill(pid, SIGKILL);
                int status;
                waitpid(pid, &status, 0);  // Blocking wait reaps child immediately
                release_vdc_slot(target_shm);

                std::string resp = "ERROR: Shared Memory IPC Connection Timeout";
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                std::cout << ">>> [OK] Handshake completed successfully." << std::endl;
                {
                    std::lock_guard<std::mutex> lock(plugins_mutex);
                    active_plugins[target_shm] = {pid, target_shm, plugin_name, is_preloaded, true, nullptr, slot_idx};
                }
                std::string resp = "VDC_SLOT " + g_vdc_shm_name + " " + std::to_string(slot_idx) + " " + std::to_string(VDC_STRIDE);
                send(client_fd, resp.c_str(), resp.length(), 0);
            }
        } else {
            std::cerr << "[ERROR] Failed to fork guest process: " << strerror(errno) << std::endl;
            release_vdc_slot(target_shm);
            std::string resp = "ERROR: Fork failed";
            send(client_fd, resp.c_str(), resp.length(), 0);
        }
    } else if (cmd.find("RELEASE") == 0) {
        size_t first_space = cmd.find(' ');
        std::string shm_name = cmd.substr(first_space + 1);

        std::lock_guard<std::mutex> lock(plugins_mutex);
        auto it = active_plugins.find(shm_name);
        if (it != active_plugins.end()) {
            if (it->second.is_preloaded) {
                std::cout << ">>> Releasing preloaded instance back to standby: " << shm_name << std::endl;
                it->second.is_assigned = false;
            } else {
                std::cout << ">>> Tearing down dynamic instance: " << shm_name << std::endl;
                pid_t child_pid = it->second.guest_pid;
                kill(child_pid, SIGTERM);
                active_plugins.erase(it);
                release_vdc_slot(shm_name);
                
                auto start = std::chrono::steady_clock::now();
                bool reaped = false;
                while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
                    int status;
                    pid_t res = waitpid(child_pid, &status, WNOHANG);
                    if (res > 0 || (res == -1 && errno == ECHILD)) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!reaped) {
                    kill(child_pid, SIGKILL);
                    int status;
                    waitpid(child_pid, &status, 0);
                }
            }
        }
        std::string resp = "RELEASED";
        send(client_fd, resp.c_str(), resp.length(), 0);
    } else if (cmd.find("UNLOAD") == 0) {
        size_t first_space = cmd.find(' ');
        std::string shm_name = cmd.substr(first_space + 1);

        std::lock_guard<std::mutex> lock(plugins_mutex);
        auto it = active_plugins.find(shm_name);
        if (it != active_plugins.end()) {
            std::cout << ">>> Killing Guest for SHM: " << shm_name << std::endl;
            pid_t child_pid = it->second.guest_pid;
            kill(child_pid, SIGTERM);
            active_plugins.erase(it);
            release_vdc_slot(shm_name);
            
            auto start = std::chrono::steady_clock::now();
            bool reaped = false;
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
                int status;
                pid_t res = waitpid(child_pid, &status, WNOHANG);
                if (res > 0 || (res == -1 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!reaped) {
                kill(child_pid, SIGKILL);
                int status;
                waitpid(child_pid, &status, 0);
            }
        }
        std::string resp = "UNLOADED";
        send(client_fd, resp.c_str(), resp.length(), 0);
    } else if (cmd.find("LIST_SHM") == 0) {
        // Return newline-separated list of all active SHM slot names
        std::lock_guard<std::mutex> lock(plugins_mutex);
        std::string resp;
        for (auto &kv : active_plugins) {
            resp += kv.first + "\n";
        }
        if (resp.empty()) resp = "";
        send(client_fd, resp.c_str(), resp.length(), 0);
    } else if (cmd.find("LIST_PLUGINS") == 0) {
        // Return newline-separated list of all active plugins with their SHM names
        std::lock_guard<std::mutex> lock(plugins_mutex);
        std::string resp;
        for (auto &kv : active_plugins) {
            resp += kv.first + " " + kv.second.plugin_name + "\n";
        }
        if (resp.empty()) resp = "";
        send(client_fd, resp.c_str(), resp.length(), 0);
    } else if (cmd.find("OPEN_EDITOR") == 0 || cmd.find("CLOSE_EDITOR") == 0) {
        // OPEN_EDITOR <shm_name> / CLOSE_EDITOR <shm_name>
        bool open = (cmd.find("OPEN_EDITOR") == 0);
        std::istringstream ss(cmd);
        std::string token, shm_name;
        ss >> token >> shm_name;
        if (shm_name.empty()) {
            std::string resp = "ERROR: Bad command args";
            send(client_fd, resp.c_str(), resp.length(), 0);
        } else {
            std::string full_shm = "/" + shm_name;
            int fd = shm_open(full_shm.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                std::string resp = "ERROR: shm_open failed for " + shm_name;
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                void *ptr = mmap(nullptr, sizeof(arthur::AudioSharedMemory),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
                if (ptr == MAP_FAILED) {
                    std::string resp = "ERROR: mmap failed";
                    send(client_fd, resp.c_str(), resp.length(), 0);
                } else {
                    auto *layout = reinterpret_cast<arthur::AudioSharedMemory*>(ptr);
                    if (layout->version == arthur::AudioSharedMemory::SHM_VERSION) {
                        layout->request_open_editor.store(open ? 1 : 0, std::memory_order_release);
                        std::string resp = "OK";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    } else {
                        std::string resp = "ERROR: SHM version mismatch";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    }
                    munmap(ptr, sizeof(arthur::AudioSharedMemory));
                }
            }
        }
    } else if (cmd.find("CONSOLE_MODE") == 0) {
        // CONSOLE_MODE <shm_name> <mode_0_1_2>
        // Write console_mode field in the shared memory for the named slot
        std::istringstream ss(cmd);
        std::string token, shm_name;
        int mode = 0;
        ss >> token >> shm_name >> mode;
        if (shm_name.empty() || mode < 0 || mode > 2) {
            std::string resp = "ERROR: Bad CONSOLE_MODE args";
            send(client_fd, resp.c_str(), resp.length(), 0);
        } else {
            // Open the SHM and write the mode field
            std::string full_shm = "/" + shm_name;
            int fd = shm_open(full_shm.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                std::string resp = "ERROR: shm_open failed for " + shm_name;
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                void *ptr = mmap(nullptr, sizeof(arthur::AudioSharedMemory),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
                if (ptr == MAP_FAILED) {
                    std::string resp = "ERROR: mmap failed";
                    send(client_fd, resp.c_str(), resp.length(), 0);
                } else {
                    auto *layout = reinterpret_cast<arthur::AudioSharedMemory*>(ptr);
                    if (layout->version == arthur::AudioSharedMemory::SHM_VERSION) {
                        layout->console_mode.store(static_cast<uint32_t>(mode),
                                                   std::memory_order_release);
                        std::string resp = "OK";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    } else {
                        std::string resp = "ERROR: SHM version mismatch";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    }
                    munmap(ptr, sizeof(arthur::AudioSharedMemory));
                }
            }
        }
    } else if (cmd.find("BYPASS_PLUGIN") == 0) {
        // BYPASS_PLUGIN <shm_name> <0_or_1>
        std::istringstream ss(cmd);
        std::string token, shm_name;
        int bypass_val = 0;
        ss >> token >> shm_name >> bypass_val;
        if (shm_name.empty()) {
            std::string resp = "ERROR: Bad BYPASS_PLUGIN args";
            send(client_fd, resp.c_str(), resp.length(), 0);
        } else {
            std::string full_shm = "/" + shm_name;
            int fd = shm_open(full_shm.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                std::string resp = "ERROR: shm_open failed for " + shm_name;
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                void *ptr = mmap(nullptr, sizeof(arthur::AudioSharedMemory),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
                if (ptr == MAP_FAILED) {
                    std::string resp = "ERROR: mmap failed";
                    send(client_fd, resp.c_str(), resp.length(), 0);
                } else {
                    auto *layout = reinterpret_cast<arthur::AudioSharedMemory*>(ptr);
                    if (layout->version == arthur::AudioSharedMemory::SHM_VERSION) {
                        layout->bypass.store(static_cast<uint32_t>(bypass_val),
                                             std::memory_order_release);
                        std::string resp = "OK";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    } else {
                        std::string resp = "ERROR: SHM version mismatch";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    }
                    munmap(ptr, sizeof(arthur::AudioSharedMemory));
                }
            }
        }
    } else if (cmd.find("AUTOGAIN_PLUGIN") == 0) {
        // AUTOGAIN_PLUGIN <shm_name> <0_or_1>
        std::istringstream ss(cmd);
        std::string token, shm_name;
        int ag_val = 0;
        ss >> token >> shm_name >> ag_val;
        if (shm_name.empty()) {
            std::string resp = "ERROR: Bad AUTOGAIN_PLUGIN args";
            send(client_fd, resp.c_str(), resp.length(), 0);
        } else {
            std::string full_shm = "/" + shm_name;
            int fd = shm_open(full_shm.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                std::string resp = "ERROR: shm_open failed for " + shm_name;
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                void *ptr = mmap(nullptr, sizeof(arthur::AudioSharedMemory),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
                if (ptr == MAP_FAILED) {
                    std::string resp = "ERROR: mmap failed";
                    send(client_fd, resp.c_str(), resp.length(), 0);
                } else {
                    auto *layout = reinterpret_cast<arthur::AudioSharedMemory*>(ptr);
                    if (layout->version == arthur::AudioSharedMemory::SHM_VERSION) {
                        layout->auto_gain.store(static_cast<uint32_t>(ag_val),
                                               std::memory_order_release);
                        std::string resp = "OK";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    } else {
                        std::string resp = "ERROR: SHM version mismatch";
                        send(client_fd, resp.c_str(), resp.length(), 0);
                    }
                    munmap(ptr, sizeof(arthur::AudioSharedMemory));
                }
            }
        }
    } else if (cmd.find("GET_EDITOR_XID") == 0) {
        // GET_EDITOR_XID <shm_name>
        // Returns: "XID <xid_decimal> <width> <height>" or "XID 0" if not open
        std::istringstream ss(cmd);
        std::string token, shm_name;
        ss >> token >> shm_name;
        if (shm_name.empty()) {
            std::string resp = "ERROR: Bad GET_EDITOR_XID args";
            send(client_fd, resp.c_str(), resp.length(), 0);
        } else {
            std::string full_shm = "/" + shm_name;
            int fd = shm_open(full_shm.c_str(), O_RDWR, 0600);
            if (fd < 0) {
                std::string resp = "XID 0";
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else {
                void *ptr = mmap(nullptr, sizeof(arthur::AudioSharedMemory),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                close(fd);
                if (ptr == MAP_FAILED) {
                    std::string resp = "XID 0";
                    send(client_fd, resp.c_str(), resp.length(), 0);
                } else {
                    auto *layout = reinterpret_cast<arthur::AudioSharedMemory*>(ptr);
                    uint64_t xid = layout->guest_window_xid.load(std::memory_order_acquire);
                    uint32_t w   = layout->editor_width.load(std::memory_order_acquire);
                    uint32_t h   = layout->editor_height.load(std::memory_order_acquire);
                    munmap(ptr, sizeof(arthur::AudioSharedMemory));
                    std::string resp = "XID " + std::to_string(xid)
                                     + " " + std::to_string(w)
                                     + " " + std::to_string(h);
                    send(client_fd, resp.c_str(), resp.length(), 0);
                }
            }
        }
    } else {

        std::string resp = "ERROR: Unknown command";
        send(client_fd, resp.c_str(), resp.length(), 0);
    }

    close(client_fd);
}

// Connection limiter
static std::atomic<int> g_active_connections{0};
static constexpr int MAX_CONNECTIONS = 32;

int main() {
    detect_isolated_cores();
    initialize_vdc_multiplex();
    g_socket_path = get_socket_path();

    // Register SIGCHLD reaper to auto-reap finished children
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, nullptr);

    // Shutdown signals
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_signal;
    sa_term.sa_flags = 0;
    sigaction(SIGINT, &sa_term, nullptr);
    sigaction(SIGTERM, &sa_term, nullptr);

    unlink(g_socket_path.c_str());

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd == -1) {
        std::cerr << "[ERROR] Failed to create server socket: " << strerror(errno) << std::endl;
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    
    // Prevent truncation buffer overrun
    if (g_socket_path.length() >= sizeof(addr.sun_path)) {
        std::cerr << "[ERROR] Socket path exceeds maximum length of " << sizeof(addr.sun_path) - 1 << " bytes." << std::endl;
        close(g_server_fd);
        return 1;
    }
    strncpy(addr.sun_path, g_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "[ERROR] Failed to bind socket at " << g_socket_path << ": " << strerror(errno) << std::endl;
        close(g_server_fd);
        return 1;
    }
    if (listen(g_server_fd, 5) == -1) {
        std::cerr << "[ERROR] Failed to listen on socket: " << strerror(errno) << std::endl;
        close(g_server_fd);
        return 1;
    }

    std::cout << ">>> Arthur Daemon Started at " << g_socket_path << ". Standing by..." << std::endl;

    while (!g_shutdown.load(std::memory_order_relaxed)) {
        int client_fd = accept(g_server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (g_shutdown.load(std::memory_order_relaxed)) break;
            continue;
        }

        // Apply 1-second timeout to client sockets (prevents DoS exhaustion)
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Connection limiting
        int current = g_active_connections.load(std::memory_order_relaxed);
        if (current >= MAX_CONNECTIONS) {
            std::cerr << "[WARNING] Connection limit reached (" << MAX_CONNECTIONS << "). Rejecting." << std::endl;
            std::string resp = "ERROR: Server busy";
            send(client_fd, resp.c_str(), resp.length(), 0);
            close(client_fd);
            continue;
        }

        g_active_connections.fetch_add(1, std::memory_order_relaxed);
        std::thread([client_fd]() {
            handle_client(client_fd);
            g_active_connections.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
    }

    std::cout << ">>> Arthur Daemon shutting down..." << std::endl;
    perform_cleanup();
    cleanup_vdc_multiplex();
    close(g_server_fd);
    return 0;
}
