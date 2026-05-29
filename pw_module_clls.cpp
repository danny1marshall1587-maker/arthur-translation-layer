#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>

// Define MLS order and period (Order 10 = 1023 samples)
constexpr int MLS_ORDER = 10;
constexpr int MLS_PERIOD = 1023;
constexpr int HISTORY_SIZE = 8192;
constexpr float TARGET_RTT = 256.0f; // Target hardware round-trip in samples

// Core data structure for the CLLS filter
struct CLLSData {
    struct pw_main_loop *loop;
    struct pw_filter *filter;
    struct spa_hook listener;

    // Ports
    void *in_loopback_port;
    void *out_loopback_port;
    void *in_audio_L_port;
    void *in_audio_R_port;
    void *out_audio_L_port;
    void *out_audio_R_port;

    // DSP Buffers
    std::vector<float> mls_ref;
    std::vector<float> loopback_ring;
    int loopback_write_idx;

    // History Buffers for fractional delay lines
    float history_L[HISTORY_SIZE];
    float history_R[HISTORY_SIZE];
    int history_write_idx;

    // Latency tracking states
    std::atomic<float> current_measured_rtt;
    std::atomic<float> target_delay; // The delay to apply to align the inputs

    // Threading for background correlation
    std::thread worker_thread;
    std::mutex worker_mutex;
    std::condition_variable worker_cv;
    std::atomic<bool> worker_running;
    std::vector<float> correlation_input;
    bool new_data_ready;
};

// Generate Maximum Length Sequence (Order 10)
std::vector<float> generate_mls_10() {
    uint32_t state = 0x3FF; // 10-bit linear feedback shift register
    std::vector<float> seq;
    seq.reserve(MLS_PERIOD);
    for (int i = 0; i < MLS_PERIOD; ++i) {
        uint32_t b9 = (state >> 9) & 1;
        uint32_t b2 = (state >> 2) & 1;
        uint32_t feedback = b9 ^ b2;
        state = ((state << 1) | feedback) & 0x3FF;
        seq.push_back(b9 ? 1.0f : -1.0f);
    }
    return seq;
}

// Parabolic interpolation for sub-sample accuracy
float calculate_subsample_peak(const std::vector<float>& corr) {
    float peak_val = -1e9f;
    int peak_idx = -1;
    for (int i = 0; i < MLS_PERIOD; ++i) {
        if (corr[i] > peak_val) {
            peak_val = corr[i];
            peak_idx = i;
        }
    }
    if (peak_idx == -1) return 0.0f;

    float y_m1 = corr[(peak_idx - 1 + MLS_PERIOD) % MLS_PERIOD];
    float y_0  = corr[peak_idx];
    float y_p1 = corr[(peak_idx + 1) % MLS_PERIOD];

    float denom = 2.0f * (y_m1 - 2.0f * y_0 + y_p1);
    float delta = 0.0f;
    if (std::abs(denom) > 1e-9f) {
        delta = (y_m1 - y_p1) / denom;
    }

    float measured_rtt = peak_idx + delta;
    if (measured_rtt > MLS_PERIOD / 2.0f) {
        measured_rtt -= MLS_PERIOD;
    }
    return measured_rtt;
}

// Background thread function to compute cross-correlation
void correlation_worker(CLLSData *d) {
    std::vector<float> cap_buf(MLS_PERIOD);
    std::vector<float> corr(MLS_PERIOD);

    while (d->worker_running) {
        {
            std::unique_lock<std::mutex> lock(d->worker_mutex);
            d->worker_cv.wait(lock, [d] { return d->new_data_ready || !d->worker_running; });
            if (!d->worker_running) break;
            cap_buf = d->correlation_input;
            d->new_data_ready = false;
        }

        // Circular cross-correlation
        for (int lag = 0; lag < MLS_PERIOD; ++lag) {
            float s = 0.0f;
            for (int i = 0; i < MLS_PERIOD; ++i) {
                s += d->mls_ref[i] * cap_buf[(i + lag) % MLS_PERIOD];
            }
            corr[lag] = s;
        }

        // Calculate exact delay
        float rtt = calculate_subsample_peak(corr);
        if (rtt > 0.0f) {
            d->current_measured_rtt.store(rtt, std::memory_order_release);
            
            // target_delay = TARGET_RTT - rtt
            // Smooth adjustment to avoid phase clicks (simple lowpass/PI)
            float current_target = TARGET_RTT - rtt;
            current_target = std::max(0.0f, std::min(current_target, (float)(HISTORY_SIZE - 10)));
            
            // Lock-free write to the target_delay atomic
            float old_delay = d->target_delay.load(std::memory_order_acquire);
            float smoothed_delay = old_delay + 0.05f * (current_target - old_delay); // Low pass filter
            d->target_delay.store(smoothed_delay, std::memory_order_release);
        }
    }
}

// Lagrange 3rd-order fractional delay interpolator
float get_delayed_sample(const float *history, int write_idx, float delay) {
    int I = (int)std::floor(delay);
    float d = delay - I;

    // Taps indices
    int idx_m1 = (write_idx - I + 1 + HISTORY_SIZE) % HISTORY_SIZE;
    int idx_0  = (write_idx - I + HISTORY_SIZE) % HISTORY_SIZE;
    int idx_p1 = (write_idx - I - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    int idx_p2 = (write_idx - I - 2 + HISTORY_SIZE) % HISTORY_SIZE;

    float s_m1 = history[idx_m1];
    float s_0  = history[idx_0];
    float s_p1 = history[idx_p1];
    float s_p2 = history[idx_p2];

    // Coefficients
    float c_m1 = -d * (d - 1.0f) * (d - 2.0f) / 6.0f;
    float c_0  = (d + 1.0f) * (d - 1.0f) * (d - 2.0f) / 2.0f;
    float c_p1 = -(d + 1.0f) * d * (d - 2.0f) / 2.0f;
    float c_p2 = (d + 1.0f) * d * (d - 1.0f) / 6.0f;

    return c_m1 * s_m1 + c_0 * s_0 + c_p1 * s_p1 + c_p2 * s_p2;
}

// Real-Time Audio Process Callback
static void on_process(void *userdata, struct spa_io_position *position) {
    CLLSData *d = static_cast<CLLSData*>(userdata);
    uint32_t n_samples = position->clock.duration;

    // 1. Get port buffers
    float *in_loopback  = (float*)pw_filter_get_dsp_buffer(d->in_loopback_port, n_samples);
    float *out_loopback = (float*)pw_filter_get_dsp_buffer(d->out_loopback_port, n_samples);
    float *in_audio_L   = (float*)pw_filter_get_dsp_buffer(d->in_audio_L_port, n_samples);
    float *in_audio_R   = (float*)pw_filter_get_dsp_buffer(d->in_audio_R_port, n_samples);
    float *out_audio_L  = (float*)pw_filter_get_dsp_buffer(d->out_audio_L_port, n_samples);
    float *out_audio_R  = (float*)pw_filter_get_dsp_buffer(d->out_audio_R_port, n_samples);

    if (!out_loopback || !in_loopback || !in_audio_L || !in_audio_R || !out_audio_L || !out_audio_R) {
        return;
    }

    // 2. Play MLS pilot & Capture loopback data
    static int mls_read_idx = 0;
    for (uint32_t i = 0; i < n_samples; ++i) {
        // Output MLS signal
        out_loopback[i] = d->mls_ref[mls_read_idx];
        mls_read_idx = (mls_read_idx + 1) % MLS_PERIOD;

        // Capture loopback return
        d->loopback_ring[d->loopback_write_idx] = in_loopback[i];
        d->loopback_write_idx = (d->loopback_write_idx + 1) % MLS_PERIOD;

        // If one full MLS period is filled, wake up the background correlation thread
        if (d->loopback_write_idx == 0) {
            std::unique_lock<std::mutex> lock(d->worker_mutex);
            d->correlation_input = d->loopback_ring;
            d->new_data_ready = true;
            d->worker_cv.notify_one();
        }
    }

    // 3. Process main audio paths through dynamic delay lines
    float current_delay = d->target_delay.load(std::memory_order_acquire);

    for (uint32_t i = 0; i < n_samples; ++i) {
        // Write fresh input to history
        d->history_L[d->history_write_idx] = in_audio_L[i];
        d->history_R[d->history_write_idx] = in_audio_R[i];

        // Retrieve dynamically delayed samples
        out_audio_L[i] = get_delayed_sample(d->history_L, d->history_write_idx, current_delay);
        out_audio_R[i] = get_delayed_sample(d->history_R, d->history_write_idx, current_delay);

        d->history_write_idx = (d->history_write_idx + 1) % HISTORY_SIZE;
    }
}

int main(int argc, char *argv[]) {
    pw_init(&argc, &argv);

    CLLSData data;
    std::memset(&data, 0, sizeof(data));

    // Initialize state
    data.mls_ref = generate_mls_10();
    data.loopback_ring.resize(MLS_PERIOD, 0.0f);
    data.loopback_write_idx = 0;
    data.history_write_idx = 0;
    std::memset(data.history_L, 0, sizeof(data.history_L));
    std::memset(data.history_R, 0, sizeof(data.history_R));
    
    data.current_measured_rtt.store(0.0f);
    data.target_delay.store(0.0f);

    // Start background correlation worker thread
    data.worker_running = true;
    data.new_data_ready = false;
    data.worker_thread = std::thread(correlation_worker, &data);

    // Create PipeWire main loop, context, and connect to core
    data.loop = pw_main_loop_new(NULL);
    if (!data.loop) {
        std::cerr << "Failed to create PipeWire main loop!" << std::endl;
        return 1;
    }

    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    if (!context) {
        std::cerr << "Failed to create PipeWire context!" << std::endl;
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    struct pw_core *core = pw_context_connect(context, NULL, 0);
    if (!core) {
        std::cerr << "Failed to connect to PipeWire core!" << std::endl;
        pw_context_destroy(context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    const char *filter_name = (argc > 1) ? argv[1] : "CLLS-Aligner";
    data.filter = pw_filter_new(core, filter_name, NULL);
    if (!data.filter) {
        std::cerr << "Failed to create PipeWire filter client!" << std::endl;
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(data.loop);
        return 1;
    }

    // Set up filter events struct (C++ compliant)
    struct pw_filter_events filter_events;
    std::memset(&filter_events, 0, sizeof(filter_events));
    filter_events.version = PW_VERSION_FILTER_EVENTS;
    filter_events.process = on_process;

    pw_filter_add_listener(data.filter, &data.listener, &filter_events, &data);

    // Define format properties (32-bit Float Audio, DSP mode)
    struct spa_dict_item items[2];
    items[0] = SPA_DICT_ITEM_INIT(PW_KEY_FORMAT_DSP, "32 bit float audio");
    items[1] = SPA_DICT_ITEM_INIT(PW_KEY_PORT_NAME, "in_loopback");
    struct spa_dict dict = SPA_DICT_INIT(items, 2);

    // Create ports
    // Note: ports created via pw_filter_add_port automatically register to PipeWire
    data.in_loopback_port = pw_filter_add_port(data.filter, PW_DIRECTION_INPUT,
                                               PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                               0, NULL, NULL, 0);

    data.out_loopback_port = pw_filter_add_port(data.filter, PW_DIRECTION_OUTPUT,
                                                PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                0, NULL, NULL, 0);

    data.in_audio_L_port = pw_filter_add_port(data.filter, PW_DIRECTION_INPUT,
                                              PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                              0, NULL, NULL, 0);
    
    data.in_audio_R_port = pw_filter_add_port(data.filter, PW_DIRECTION_INPUT,
                                              PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                              0, NULL, NULL, 0);

    data.out_audio_L_port = pw_filter_add_port(data.filter, PW_DIRECTION_OUTPUT,
                                               PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                               0, NULL, NULL, 0);
    
    data.out_audio_R_port = pw_filter_add_port(data.filter, PW_DIRECTION_OUTPUT,
                                               PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                               0, NULL, NULL, 0);

    // Connect to PipeWire Graph
    std::cout << ">>> Connecting CLLS Aligner filter to PipeWire..." << std::endl;
    if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
        std::cerr << "Failed to connect filter to graph!" << std::endl;
        return 1;
    }

    std::cout << ">>> [OK] CLLS Aligner active. Running main event loop." << std::endl;
    
    // Periodically log status in main thread context
    std::thread status_logger([&data] {
        while (data.worker_running) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            float rtt = data.current_measured_rtt.load(std::memory_order_acquire);
            float delay = data.target_delay.load(std::memory_order_acquire);
            if (rtt > 0.0f) {
                std::cout << "[CLLS STATUS] Measured RTT: " << rtt << " samples | Applied Offset: +" << delay << " samples" << std::endl;
            }
        }
    });

    // Run PipeWire loop (blocks until terminated)
    pw_main_loop_run(data.loop);

    // Shutdown sequence
    std::cout << ">>> Shutting down CLLS Aligner..." << std::endl;
    data.worker_running = false;
    {
        std::lock_guard<std::mutex> lock(data.worker_mutex);
        data.worker_cv.notify_all();
    }
    if (data.worker_thread.joinable()) data.worker_thread.join();
    if (status_logger.joinable()) status_logger.join();

    pw_filter_destroy(data.filter);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(data.loop);
    pw_deinit();

    return 0;
}
