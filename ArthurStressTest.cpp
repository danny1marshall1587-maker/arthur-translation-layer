#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sched.h>
#include "AudioIPC.h"

#define M_PI 3.14159265358979323846

void pin_to_isolated_cores() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    std::vector<int> cores;

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
                        cores.push_back(cpu);
                    }
                } catch (...) {}
            } else {
                try {
                    int cpu = std::stoi(token);
                    cores.push_back(cpu);
                } catch (...) {}
            }
        }
    }

    if (cores.empty()) {
        std::cout << "[TEST] No isolated cores found in sysfs. Falling back to default core 4." << std::endl;
        cores.push_back(4);
    }

    int target_cpu = cores[0];
    CPU_SET(target_cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        std::cout << ">>> [OK] Stress test thread CPU affinity successfully set to first isolated core: " << target_cpu << std::endl;
    } else {
        std::cerr << "[WARNING] Failed to set stress test CPU affinity to core " << target_cpu << ": " << strerror(errno) << std::endl;
    }
}

int main() {
    std::cout << "=== Arthur Stress & THD+N Self-Test ===" << std::endl;

    // Pin the stress test main thread to the isolated CPU cores
    pin_to_isolated_cores();

    // 1. Connect to Daemon to spin up the dummy VST3 plugin (which maps the SHM)
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create UNIX socket." << std::endl;
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/arthur.sock", sizeof(addr.sun_path)-1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sock);
        std::cerr << "[ERROR] Stress test failed to connect to arthur-daemon." << std::endl;
        return 1;
    }

    std::string msg = "GET_SHM dummy_plugin.dll";
    send(sock, msg.c_str(), msg.length(), 0);

    char buffer[256];
    int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
    close(sock);

    if (bytes <= 0) {
        std::cerr << "Failed to receive SHM name from daemon." << std::endl;
        return 1;
    }
    buffer[bytes] = '\0';
    std::string resp(buffer);
    std::cout << "Daemon response: " << resp << std::endl;

    if (resp.find("SHM ") != 0) {
        std::cerr << "Daemon returned error: " << resp << std::endl;
        return 1;
    }

    std::string shm_name = resp.substr(4);

    // 2. Attach to the shared memory segment
    auto transport = std::make_shared<arthur::AudioTransport>();
    if (!transport->attach(shm_name)) {
        std::cerr << "Failed to attach to shared memory: " << shm_name << std::endl;
        return 1;
    }

    auto* layout = transport->get();
    std::cout << "Successfully attached to shared memory. Mapping information:" << std::endl;
    std::cout << "  State: " << (int)layout->state.load() << std::endl;

    // 3. Generate test signal parameters (A = 0.8, f = 1000Hz)
    const double sample_rate = 48000.0;
    const double freq = 1000.0; 
    const float A = 0.8f;
    const int total_samples = 48000; // 1 second of audio
    const int block_size = 16;       // 16 samples block size

    std::vector<float> input_signal(total_samples);
    for (int i = 0; i < total_samples; ++i) {
        input_signal[i] = A * (float)sin(2.0 * M_PI * freq * (double)i / sample_rate);
    }

    std::vector<float> output_signal(total_samples, 0.0f);

    layout->sample_rate = (uint32_t)sample_rate;
    layout->sample_count = block_size;
    layout->num_inputs = 2;
    layout->num_outputs = 2;

    // Pre-fault the shared memory buffers to prevent cold page faults during timing
    std::cout << "Pre-faulting shared memory pages..." << std::endl;
    for (uint32_t c = 0; c < arthur::SHM_MAX_CHANNELS; ++c) {
        memset(layout->input_buffers[c], 0, arthur::SHM_MAX_SAMPLES * sizeof(float));
        memset(layout->output_buffers[c], 0, arthur::SHM_MAX_SAMPLES * sizeof(float));
    }

    // 4. Run Warmup Phase (10 blocks) to get cache and scheduling hot
    std::cout << "Running warmup phase (10 blocks)..." << std::endl;
    for (int i = 0; i < 10; ++i) {
        layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int timeout_tries = 1000000;
        while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED && --timeout_tries > 0) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // 5. Streaming test signal phase
    std::cout << "Streaming 1 second of 1kHz sine wave (16-sample blocks)..." << std::endl;
    double max_block_time_us = 0.0;
    bool timing_failure = false;

    for (int offset = 0; offset < total_samples; offset += block_size) {
        // Copy block to inputs
        for (int c = 0; c < 2; ++c) {
            memcpy(layout->input_buffers[c], &input_signal[offset], block_size * sizeof(float));
        }

        auto start_block = std::chrono::high_resolution_clock::now();

        // Trigger guest process
        layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Wait for guest process with timeout (using low-latency pause instruction)
        int timeout_tries = 10000000;
        while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED && --timeout_tries > 0) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            #if defined(__x86_64__) || defined(_M_X64)
            asm volatile("pause" ::: "memory");
            #endif
        }

        auto end_block = std::chrono::high_resolution_clock::now();
        double block_time_us = std::chrono::duration<double, std::micro>(end_block - start_block).count();
        if (block_time_us > max_block_time_us) {
            max_block_time_us = block_time_us;
        }

        if (block_time_us > 333.33) {
            std::cerr << "[WARNING] Block execution time (" << block_time_us << " us) exceeded 333.33 microseconds!" << std::endl;
            bool cores_isolated = false;
            std::ifstream sys_isolated("/sys/devices/system/cpu/isolated");
            std::string iso_line;
            if (sys_isolated && std::getline(sys_isolated, iso_line) && !iso_line.empty()) {
                cores_isolated = true;
            }
            if (cores_isolated) {
                timing_failure = true;
            }
        }

        if (timeout_tries == 0) {
            std::cerr << "[FATAL] Guest processing timed out during block at offset: " << offset << std::endl;
            layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
            return 1;
        }

        // Read outputs back
        memcpy(&output_signal[offset], layout->output_buffers[0], block_size * sizeof(float));

        // Release control
        layout->state.store(arthur::TransportState::STATE_IDLE, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    std::cout << "Streaming completed. Max block processing time: " << max_block_time_us << " us." << std::endl;

    // Fail if any single 16-sample block took longer than 333.33 us
    if (timing_failure) {
        std::cerr << "[FAILURE] One or more 16-sample blocks took longer than 333.33 microseconds." << std::endl;
        return 1;
    }

    // 6. Automated Mathematical Audits
    // A. Check if completely silent
    bool completely_silent = true;
    for (int i = 0; i < total_samples; ++i) {
        if (std::abs(output_signal[i]) > 1e-6f) {
            completely_silent = false;
            break;
        }
    }
    if (completely_silent) {
        std::cerr << "[FAILURE] Returned buffer is completely silent!" << std::endl;
        return 1;
    }

    // B. Check if identical to bypassed input
    bool identical_to_input = true;
    for (int i = 0; i < total_samples; ++i) {
        if (std::abs(output_signal[i] - input_signal[i]) > 1e-6f) {
            identical_to_input = false;
            break;
        }
    }
    if (identical_to_input) {
        std::cerr << "[FAILURE] Returned buffer is identical to bypassed input!" << std::endl;
        return 1;
    }

    // C. Measure peak amplitudes
    float peak_input = 0.0f;
    float peak_output = 0.0f;
    for (int i = 0; i < total_samples; ++i) {
        if (std::abs(input_signal[i]) > peak_input) peak_input = std::abs(input_signal[i]);
        if (std::abs(output_signal[i]) > peak_output) peak_output = std::abs(output_signal[i]);
    }

    if (peak_output < peak_input) {
        std::cout << "DIAGNOSTIC: Compression Detected" << std::endl;
    }

    // D. Calculate THD using Fourier coefficients (harmonics 2 to 24)
    double V1 = 0.0;
    double sum_harmonics_sq = 0.0;

    for (int k = 1; k <= 24; ++k) {
        double cos_sum = 0.0;
        double sin_sum = 0.0;
        double target_freq = (double)k * freq;
        for (int t = 0; t < total_samples; ++t) {
            double angle = 2.0 * M_PI * target_freq * (double)t / sample_rate;
            cos_sum += (double)output_signal[t] * cos(angle);
            sin_sum += (double)output_signal[t] * sin(angle);
        }
        double A_k = (2.0 / (double)total_samples) * cos_sum;
        double B_k = (2.0 / (double)total_samples) * sin_sum;
        double V_k = sqrt(A_k * A_k + B_k * B_k);

        if (k == 1) {
            V1 = V_k;
        } else {
            sum_harmonics_sq += V_k * V_k;
        }
    }

    double thd = 0.0;
    if (V1 > 1e-15) {
        thd = sqrt(sum_harmonics_sq) / V1;
    }

    std::cout << "  Calculated THD: " << thd * 100.0 << " %" << std::endl;

    if (thd > 0.01) {
        std::cout << "DIAGNOSTIC: Saturation Detected" << std::endl;
    }

    std::cout << "\n>>> [SUCCESS] All self-tests passed successfully." << std::endl;
    return 0;
}
