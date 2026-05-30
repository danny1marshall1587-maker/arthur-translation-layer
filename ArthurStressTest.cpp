#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "AudioIPC.h"

#define M_PI 3.14159265358979323846

int main() {
    std::cout << "=== Arthur Stress & THD+N Self-Test ===" << std::endl;

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

    // 3. Generate test signal parameters
    const double sample_rate = 48000.0;
    const double freq = 1000.0; // 1kHz sine wave
    const int total_samples = 48000; // 1 second of audio
    const int block_size = 16;       // 16 samples block size

    std::vector<float> input_signal(total_samples);
    for (int i = 0; i < total_samples; ++i) {
        input_signal[i] = (float)sin(2.0 * M_PI * freq * (double)i / sample_rate);
    }

    std::vector<float> output_signal(total_samples, 0.0f);

    // 4. Stream audio blocks through the isolated DSP cores
    std::cout << "Streaming 1 second of 1kHz sine wave (16-sample blocks)..." << std::endl;

    layout->sample_rate = (uint32_t)sample_rate;
    layout->sample_count = block_size;
    layout->num_inputs = 2;
    layout->num_outputs = 2;

    auto start_test = std::chrono::high_resolution_clock::now();

    for (int offset = 0; offset < total_samples; offset += block_size) {
        // Copy block to inputs
        for (int c = 0; c < 2; ++c) {
            memcpy(layout->input_buffers[c], &input_signal[offset], block_size * sizeof(float));
        }

        // Trigger guest process
        layout->state.store(arthur::TransportState::STATE_HOST_WRITTEN, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Wait for guest process with timeout (100ms)
        int timeout_tries = 10000;
        while (layout->state.load(std::memory_order_seq_cst) != arthur::TransportState::STATE_GUEST_PROCESSED && --timeout_tries > 0) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
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

    auto end_test = std::chrono::high_resolution_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end_test - start_test).count();
    std::cout << "Streaming completed in " << duration_ms << " ms." << std::endl;

    // 5. Calculate THD+N and Peak Compression
    // Since dummy VST3 applies a constant gain of 0.5f:
    const float expected_gain = 0.5f;
    double sum_signal_sq = 0.0;
    double sum_noise_sq = 0.0;
    float peak_input = 0.0f;
    float peak_output = 0.0f;

    for (int i = 0; i < total_samples; ++i) {
        float in = input_signal[i];
        float out = output_signal[i];
        float expected_out = in * expected_gain;

        sum_signal_sq += expected_out * expected_out;
        sum_noise_sq += (out - expected_out) * (out - expected_out);

        if (std::abs(in) > peak_input) peak_input = std::abs(in);
        if (std::abs(out) > peak_output) peak_output = std::abs(out);
    }

    double thd_n = sqrt(sum_noise_sq / sum_signal_sq);
    double thd_n_db = 20.0 * log10(thd_n + 1e-15);
    float observed_gain = peak_output / (peak_input + 1e-15f);
    float compression_db = -20.0f * log10(observed_gain / expected_gain + 1e-15f);

    std::cout << "\n=== ANALYSIS RESULTS ===" << std::endl;
    std::cout << "  Peak Input Amplitude:  " << peak_input << std::endl;
    std::cout << "  Peak Output Amplitude: " << peak_output << std::endl;
    std::cout << "  Observed Gain:         " << observed_gain << " (Expected: " << expected_gain << ")" << std::endl;
    std::cout << "  Peak Compression:      " << compression_db << " dB" << std::endl;
    std::cout << "  Total Harmonic Distortion + Noise (THD+N): " << thd_n * 100.0 << " % (" << thd_n_db << " dB)" << std::endl;

    if (thd_n < 1e-5 && std::abs(compression_db) < 0.01) {
        std::cout << "\n>>> [SUCCESS] Isolated DSP core meets low-latency transparent routing specifications." << std::endl;
        return 0;
    } else {
        std::cerr << "\n>>> [FAILURE] Distortion or compression exceeds acceptable DSP thresholds!" << std::endl;
        return 1;
    }
}
