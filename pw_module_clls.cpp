#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include <iostream>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <algorithm>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include "AudioIPC.h"

static struct pw_main_loop *g_main_loop = nullptr;

static void handle_clls_signal(int sig) {
    if (g_main_loop) {
        pw_main_loop_quit(g_main_loop);
    }
}

using CLLSSyncLayout = arthur::CLLSSyncLayout;

static CLLSSyncLayout *g_sync_layout = nullptr;
static int g_shm_fd = -1;

static inline bool is_slot_active(int slot) {
    if (!g_sync_layout) return false;
    pid_t pid = g_sync_layout->owner_pid[slot].load(std::memory_order_acquire);
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0 || errno != ESRCH) {
        return true;
    }
    // Process is dead. Clean up slot.
    g_sync_layout->owner_pid[slot].store(0, std::memory_order_release);
    return false;
}

// ---------------------------------------------------------------------------
// CLLS: Closed-Loop Latency Sync — PipeWire Filter Node
//
// Outputs an MLS pilot on a loopback channel, cross-correlates the return
// to measure hardware RTT, and applies fractional delay lines to align
// the main audio channels. Designed for multi-device phase locking.
// ---------------------------------------------------------------------------

// Define MLS order and period (Order 10 = 1023 samples)
constexpr int MLS_ORDER = 10;
constexpr int MLS_PERIOD = 1023;
constexpr int HISTORY_SIZE = 8192;
constexpr float TARGET_RTT = 1024.0f; // Target hardware round-trip in samples

// Core data structure for the CLLS filter
// All members use in-class initializers — NO memset over this struct.
struct CLLSData {
    struct pw_main_loop *loop = nullptr;
    struct pw_filter *filter = nullptr;
    struct spa_hook listener = {};
    int slot_idx = 0;

    // Ports
    void *in_loopback_port = nullptr;
    void *out_loopback_port = nullptr;
    void *in_audio_L_port = nullptr;
    void *in_audio_R_port = nullptr;
    void *out_audio_L_port = nullptr;
    void *out_audio_R_port = nullptr;

    // MLS reference (pre-generated, read-only after init)
    float mls_ref[MLS_PERIOD] = {};

    // Lock-free double buffer for loopback capture
    // RT callback writes into loopback_buf[active_write_buf], correlation
    // worker reads from loopback_buf[1 - active_write_buf].
    float loopback_buf[2][MLS_PERIOD] = {};
    std::atomic<int> active_write_buf{0};
    int loopback_write_idx = 0;
    int mls_read_idx = 0;   // Per-instance MLS playback index

    // Worker busy flag to avoid double-buffer overrun races in RT thread
    std::atomic<bool> worker_busy{false};

    // Signal from RT to worker that a full buffer is ready
    sem_t buffer_ready_sem;

    // History Buffers for fractional delay lines
    float history_L[HISTORY_SIZE] = {};
    float history_R[HISTORY_SIZE] = {};
    int history_write_idx = 0;

    // Latency tracking states
    std::atomic<float> current_measured_rtt{0.0f};
    std::atomic<float> target_delay{0.0f};

    // Per-sample smooth delay (owned exclusively by RT thread — not atomic)
    float smooth_delay_L = 0.0f;
    float smooth_delay_R = 0.0f;

    // PI controller state (owned exclusively by worker thread)
    float pi_integral = 0.0f;
    static constexpr float Kp = 0.02f;    // Proportional gain
    static constexpr float Ki = 0.0005f;  // Integral gain (eliminates steady-state error)
    static constexpr float ANTI_WINDUP_LIMIT = 2000.0f;

    // Threading for background correlation
    std::thread worker_thread;
    std::atomic<bool> worker_running{false};
};

// Generate Maximum Length Sequence (Order 10)
static void generate_mls_10(float *out) {
    uint32_t state = 0x3FF; // 10-bit linear feedback shift register
    for (int i = 0; i < MLS_PERIOD; ++i) {
        uint32_t b9 = (state >> 9) & 1;
        uint32_t b2 = (state >> 2) & 1;
        uint32_t feedback = b9 ^ b2;
        state = ((state << 1) | feedback) & 0x3FF;
        out[i] = b9 ? 1.0f : -1.0f;
    }
}

// Parabolic interpolation for sub-sample accuracy
static float calculate_subsample_peak(const float *corr, int len) {
    float peak_val = -1e9f;
    int peak_idx = -1;
    for (int i = 0; i < len; ++i) {
        if (corr[i] > peak_val) {
            peak_val = corr[i];
            peak_idx = i;
        }
    }
    if (peak_idx == -1) return 0.0f;

    float y_m1 = corr[(peak_idx - 1 + len) % len];
    float y_0  = corr[peak_idx];
    float y_p1 = corr[(peak_idx + 1) % len];

    float denom = 2.0f * (y_m1 - 2.0f * y_0 + y_p1);
    float delta = 0.0f;
    if (std::abs(denom) > 1e-9f) {
        delta = (y_m1 - y_p1) / denom;
    }

    float measured_rtt = peak_idx + delta;
    // Wrap into valid range [0, MLS_PERIOD)
    if (measured_rtt < 0.0f) measured_rtt += MLS_PERIOD;
    if (measured_rtt >= (float)MLS_PERIOD) measured_rtt -= MLS_PERIOD;
    return measured_rtt;
}

// Background thread: reads from inactive double-buffer, computes cross-correlation
static void correlation_worker(CLLSData *d) {
    float cap_buf[MLS_PERIOD];
    float corr[MLS_PERIOD];

    while (d->worker_running.load(std::memory_order_acquire)) {
        // Wait for RT thread to signal a full buffer
        sem_wait(&d->buffer_ready_sem);
        if (!d->worker_running.load(std::memory_order_acquire)) break;

        d->worker_busy.store(true, std::memory_order_release);

        // Read from the INACTIVE buffer (the one the RT thread is NOT writing into)
        int read_buf = 1 - d->active_write_buf.load(std::memory_order_acquire);
        std::memcpy(cap_buf, d->loopback_buf[read_buf], sizeof(float) * MLS_PERIOD);

        // Circular cross-correlation (O(N²))
        for (int lag = 0; lag < MLS_PERIOD; ++lag) {
            float s = 0.0f;
            for (int i = 0; i < MLS_PERIOD; ++i) {
                s += d->mls_ref[i] * cap_buf[(i + lag) % MLS_PERIOD];
            }
            corr[lag] = s;
        }

        // Calculate average absolute correlation to estimate noise floor
        float sum_abs = 0.0f;
        for (int i = 0; i < MLS_PERIOD; ++i) {
            sum_abs += std::abs(corr[i]);
        }
        float avg_abs = sum_abs / MLS_PERIOD;

        // Find the peak value in corr
        float peak_val = -1e9f;
        for (int i = 0; i < MLS_PERIOD; ++i) {
            if (corr[i] > peak_val) {
                peak_val = corr[i];
            }
        }

        // Validate that peak is significant (noise-gate: 5x noise floor)
        if (peak_val > 5.0f * avg_abs) {
            float rtt = calculate_subsample_peak(corr, MLS_PERIOD);
            d->current_measured_rtt.store(rtt, std::memory_order_release);

            if (g_sync_layout && d->slot_idx >= 0 && d->slot_idx < 8) {
                g_sync_layout->measured_rtt[d->slot_idx].store(rtt, std::memory_order_release);
            }

            float max_rtt = rtt;
            if (g_sync_layout) {
                for (int i = 0; i < 8; ++i) {
                    if (is_slot_active(i)) {
                        float other_rtt = g_sync_layout->measured_rtt[i].load(std::memory_order_acquire);
                        if (other_rtt > max_rtt) {
                            max_rtt = other_rtt;
                        }
                    }
                }
            }
            float target_rtt = max_rtt + 128.0f; // Target T = max(L) + margin
            if (target_rtt > HISTORY_SIZE - 256.0f) {
                target_rtt = HISTORY_SIZE - 256.0f;
            }

            // PI controller: target_delay = target_rtt - rtt
            float desired_target = target_rtt - rtt;
            desired_target = std::clamp(desired_target, 0.0f, (float)HISTORY_SIZE - 256.0f);

            float current_delay = d->target_delay.load(std::memory_order_acquire);
            float error = desired_target - current_delay;

            // Integral term with anti-windup
            d->pi_integral += error;
            d->pi_integral = std::clamp(d->pi_integral, -CLLSData::ANTI_WINDUP_LIMIT, CLLSData::ANTI_WINDUP_LIMIT);

            float new_delay = current_delay + CLLSData::Kp * error + CLLSData::Ki * d->pi_integral;
            new_delay = std::clamp(new_delay, 0.0f, (float)HISTORY_SIZE - 256.0f);

            d->target_delay.store(new_delay, std::memory_order_release);
        }

        d->worker_busy.store(false, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Lagrange 3rd-order fractional delay interpolator
//
// For integer delay I and fractional part d ∈ [0,1):
//   idx_m1 = write_idx - I + 1   (x = -1, newer sample, delay I-1)
//   idx_0  = write_idx - I       (x =  0, center 1,     delay I)
//   idx_p1 = write_idx - I - 1   (x =  1, center 2,     delay I+1)
//   idx_p2 = write_idx - I - 2   (x =  2, older sample, delay I+2)
//
// Coefficients are standard 4-point Lagrange basis polynomials.
// ---------------------------------------------------------------------------
static inline float get_delayed_sample(const float *history, int write_idx, float delay) {
    // Ensure minimum delay of 1 sample so idx_m1 doesn't alias the write head
    if (delay < 1.0f) delay = 1.0f;

    int I = (int)std::floor(delay);
    float d = delay - (float)I;

    // Corrected Tap indices: newer to older (so increasing delay increases phase delay)
    int idx_m1 = (write_idx - I + 1 + HISTORY_SIZE) % HISTORY_SIZE;
    int idx_0  = (write_idx - I     + HISTORY_SIZE) % HISTORY_SIZE;
    int idx_p1 = (write_idx - I - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    int idx_p2 = (write_idx - I - 2 + HISTORY_SIZE) % HISTORY_SIZE;

    float s_m1 = history[idx_m1];
    float s_0  = history[idx_0];
    float s_p1 = history[idx_p1];
    float s_p2 = history[idx_p2];

    // Standard 4-point Lagrange basis coefficients for fractional delay d ∈ [0,1)
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

    float *in_loopback  = (float*)pw_filter_get_dsp_buffer(d->in_loopback_port, n_samples);
    float *out_loopback = (float*)pw_filter_get_dsp_buffer(d->out_loopback_port, n_samples);
    float *in_audio_L   = (float*)pw_filter_get_dsp_buffer(d->in_audio_L_port, n_samples);
    float *in_audio_R   = (float*)pw_filter_get_dsp_buffer(d->in_audio_R_port, n_samples);
    float *out_audio_L  = (float*)pw_filter_get_dsp_buffer(d->out_audio_L_port, n_samples);
    float *out_audio_R  = (float*)pw_filter_get_dsp_buffer(d->out_audio_R_port, n_samples);

    if (!out_loopback || !in_loopback || !in_audio_L || !in_audio_R || !out_audio_L || !out_audio_R) {
        return;
    }

    // 2. Play MLS pilot & Capture loopback data — lock-free double-buffer
    int write_buf = d->active_write_buf.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n_samples; ++i) {
        out_loopback[i] = d->mls_ref[d->mls_read_idx];
        d->mls_read_idx = (d->mls_read_idx + 1) % MLS_PERIOD;

        d->loopback_buf[write_buf][d->loopback_write_idx] = in_loopback[i];
        d->loopback_write_idx = (d->loopback_write_idx + 1) % MLS_PERIOD;

        if (d->loopback_write_idx == 0) {
            // Guard double-buffer overrun races: only swap if worker finished
            if (d->worker_busy.load(std::memory_order_acquire)) {
                d->loopback_write_idx = 0; // reset and drop this correlation cycle
            } else {
                d->active_write_buf.store(1 - write_buf, std::memory_order_release);
                write_buf = 1 - write_buf;
                sem_post(&d->buffer_ready_sem);
            }
        }
    }

    // 3. Process main audio through dynamic fractional delay lines
    float target = d->target_delay.load(std::memory_order_acquire);
    static constexpr float SMOOTH_COEFF = 0.001f;

    for (uint32_t i = 0; i < n_samples; ++i) {
        d->smooth_delay_L += SMOOTH_COEFF * (target - d->smooth_delay_L);
        d->smooth_delay_R += SMOOTH_COEFF * (target - d->smooth_delay_R);

        d->history_L[d->history_write_idx] = in_audio_L[i];
        d->history_R[d->history_write_idx] = in_audio_R[i];

        out_audio_L[i] = get_delayed_sample(d->history_L, d->history_write_idx, d->smooth_delay_L);
        out_audio_R[i] = get_delayed_sample(d->history_R, d->history_write_idx, d->smooth_delay_R);

        d->history_write_idx = (d->history_write_idx + 1) % HISTORY_SIZE;
    }
}

// Helper: build a float32 audio format SPA pod for port creation
static uint8_t port_buffer[1024];

static void attach_clls_sync() {
    const char *shm_name = "/arthur_clls_sync";
    g_shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0600);
    if (g_shm_fd < 0) {
        std::cerr << "[WARNING] Failed to open CLLS sync SHM: " << strerror(errno) << std::endl;
        return;
    }

    struct stat st;
    if (fstat(g_shm_fd, &st) == 0 && st.st_size < (off_t)sizeof(CLLSSyncLayout)) {
        if (ftruncate(g_shm_fd, sizeof(CLLSSyncLayout)) < 0) {
            std::cerr << "[WARNING] Failed to truncate CLLS sync SHM: " << strerror(errno) << std::endl;
            close(g_shm_fd);
            g_shm_fd = -1;
            return;
        }
    }

    void *ptr = mmap(NULL, sizeof(CLLSSyncLayout), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "[WARNING] Failed to mmap CLLS sync SHM: " << strerror(errno) << std::endl;
        close(g_shm_fd);
        g_shm_fd = -1;
        return;
    }

    g_sync_layout = static_cast<CLLSSyncLayout*>(ptr);
    
    // Initialize CLLS aggregates if not set
    if (g_sync_layout->version != CLLSSyncLayout::SYNC_VERSION) {
        g_sync_layout->version = CLLSSyncLayout::SYNC_VERSION;
        for (int i = 0; i < 8; ++i) {
            g_sync_layout->owner_pid[i].store(0, std::memory_order_relaxed);
            g_sync_layout->measured_rtt[i].store(0.0f, std::memory_order_relaxed);
        }
        g_sync_layout->global_target_rtt.store(0.0f, std::memory_order_relaxed);
    }
}

static void detach_clls_sync(int slot) {
    if (g_sync_layout) {
        if (slot >= 0 && slot < 8) {
            g_sync_layout->owner_pid[slot].store(0, std::memory_order_release);
        }
        
        // Unlink if no other slots are active
        bool any_active = false;
        for (int i = 0; i < 8; ++i) {
            if (is_slot_active(i)) {
                any_active = true;
            }
        }
        
        munmap(g_sync_layout, sizeof(CLLSSyncLayout));
        g_sync_layout = nullptr;
        
        if (!any_active) {
            shm_unlink("/arthur_clls_sync");
        }
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
}

int main(int argc, char *argv[]) {
    pw_init(&argc, &argv);

    CLLSData data;  // In-class initializers handle everything — no memset!

    const char *filter_name = (argc > 1) ? argv[1] : "CLLS-Aligner-1";
    
    // Parse slot index from filter_name (e.g. CLLS-Aligner-1 -> slot 0)
    int slot_idx = 0;
    std::string name_str(filter_name);
    size_t dash_idx = name_str.find_last_of('-');
    if (dash_idx != std::string::npos) {
        try {
            int parsed = std::stoi(name_str.substr(dash_idx + 1));
            if (parsed >= 1 && parsed <= 8) {
                slot_idx = parsed - 1;
            }
        } catch (...) {}
    }
    data.slot_idx = slot_idx;

    // Attach to CLLS sync shared memory
    attach_clls_sync();
    if (g_sync_layout && slot_idx >= 0 && slot_idx < 8) {
        g_sync_layout->owner_pid[slot_idx].store(getpid(), std::memory_order_release);
        g_sync_layout->measured_rtt[slot_idx].store(0.0f, std::memory_order_release);
    }

    std::cout << ">>> Started CLLS Aligner for Slot " << (slot_idx + 1) << std::endl;

    // Initialize MLS reference
    generate_mls_10(data.mls_ref);

    // Initialize semaphore for RT → worker signaling
    sem_init(&data.buffer_ready_sem, 0, 0);

    // Start background correlation worker thread
    data.worker_running.store(true, std::memory_order_release);
    data.worker_thread = std::thread(correlation_worker, &data);

    // Create PipeWire main loop, context, and connect to core
    data.loop = pw_main_loop_new(NULL);
    if (!data.loop) {
        std::cerr << "Failed to create PipeWire main loop!" << std::endl;
        detach_clls_sync(slot_idx);
        return 1;
    }
    g_main_loop = data.loop;

    struct sigaction sa_term;
    std::memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_clls_signal;
    sigaction(SIGINT, &sa_term, nullptr);
    sigaction(SIGTERM, &sa_term, nullptr);

    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(data.loop), NULL, 0);
    if (!context) {
        std::cerr << "Failed to create PipeWire context!" << std::endl;
        pw_main_loop_destroy(data.loop);
        detach_clls_sync(slot_idx);
        return 1;
    }

    struct pw_core *core = pw_context_connect(context, NULL, 0);
    if (!core) {
        std::cerr << "Failed to connect to PipeWire core!" << std::endl;
        pw_context_destroy(context);
        pw_main_loop_destroy(data.loop);
        detach_clls_sync(slot_idx);
        return 1;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_NODE_NAME, filter_name,
        PW_KEY_NODE_DESCRIPTION, "Closed Loop Latency Sync Aligner",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        NULL
    );
    data.filter = pw_filter_new(core, filter_name, props);
    if (!data.filter) {
        std::cerr << "Failed to create PipeWire filter client!" << std::endl;
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(data.loop);
        detach_clls_sync(slot_idx);
        return 1;
    }

    // Set up filter events struct (C++ compliant)
    struct pw_filter_events filter_events;
    std::memset(&filter_events, 0, sizeof(filter_events));
    filter_events.version = PW_VERSION_FILTER_EVENTS;
    filter_events.process = on_process;

    pw_filter_add_listener(data.filter, &data.listener, &filter_events, &data);

    // Helper lambda for creating named DSP audio ports
    auto make_port = [&](enum pw_direction dir, const char* name) -> void* {
        uint8_t buffer[1024];
        struct spa_pod_builder b;
        spa_pod_builder_init(&b, buffer, sizeof(buffer));

        struct spa_audio_info_dsp info = SPA_AUDIO_INFO_DSP_INIT(.format = SPA_AUDIO_FORMAT_DSP_F32);
        const struct spa_pod *params[1];
        params[0] = spa_format_audio_dsp_build(&b, SPA_PARAM_EnumFormat, &info);

        struct pw_properties *port_props = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, name,
            PW_KEY_AUDIO_CHANNEL, "MONO",
            NULL
        );

        void* port = pw_filter_add_port(data.filter, dir,
            PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(float),
            port_props,
            params, 1);

        return port;
    };

    data.in_loopback_port  = make_port(PW_DIRECTION_INPUT,  "in_loopback");
    data.out_loopback_port = make_port(PW_DIRECTION_OUTPUT, "out_loopback");
    data.in_audio_L_port   = make_port(PW_DIRECTION_INPUT,  "in_audio_L");
    data.in_audio_R_port   = make_port(PW_DIRECTION_INPUT,  "in_audio_R");
    data.out_audio_L_port  = make_port(PW_DIRECTION_OUTPUT, "out_audio_L");
    data.out_audio_R_port  = make_port(PW_DIRECTION_OUTPUT, "out_audio_R");

    // Connect to PipeWire Graph
    std::cout << ">>> Connecting CLLS Aligner filter to PipeWire..." << std::endl;
    if (pw_filter_connect(data.filter, PW_FILTER_FLAG_RT_PROCESS, NULL, 0) < 0) {
        std::cerr << "Failed to connect filter to graph!" << std::endl;
        pw_filter_destroy(data.filter);
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(data.loop);
        detach_clls_sync(slot_idx);
        return 1;
    }

    std::cout << ">>> [OK] CLLS Aligner active. Running main event loop." << std::endl;
    
    // Periodically log status in main thread context
    std::thread status_logger([&data, slot_idx] {
        while (data.worker_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            float rtt = data.current_measured_rtt.load(std::memory_order_acquire);
            float delay = data.target_delay.load(std::memory_order_acquire);
            if (rtt > 0.0f) {
                float max_rtt = rtt;
                if (g_sync_layout) {
                    for (int i = 0; i < 8; ++i) {
                        if (is_slot_active(i)) {
                            float other_rtt = g_sync_layout->measured_rtt[i].load(std::memory_order_acquire);
                            if (other_rtt > max_rtt) {
                                max_rtt = other_rtt;
                            }
                        }
                    }
                }
                float target_rtt = max_rtt + 128.0f;
                if (target_rtt > HISTORY_SIZE - 256.0f) target_rtt = HISTORY_SIZE - 256.0f;

                float phase_jitter = std::abs(target_rtt - rtt - delay);
                const char* status = (phase_jitter < 1.0f) ? "LOCKED" : "CONVERGING";
                std::cout << "=== CLLS Status (Slot " << (slot_idx + 1) << ") ===" << std::endl
                          << "  Target Latency:  " << target_rtt << " samples" << std::endl
                          << "  Measured RTT:    " << rtt << " samples" << std::endl
                          << "  Compensation:    +" << delay << " samples" << std::endl
                          << "  Phase Error:     " << phase_jitter << " samples" << std::endl
                          << "  Status:          " << status << std::endl;
                std::cout << "[CLLS STATUS] Measured RTT: " << rtt << " samples | Applied Offset: " << (delay >= 0 ? "+" : "") << delay << " samples" << std::endl;
            }
        }
    });

    // Run PipeWire loop (blocks until terminated)
    pw_main_loop_run(data.loop);

    // Shutdown sequence
    std::cout << ">>> Shutting down CLLS Aligner..." << std::endl;
    data.worker_running.store(false, std::memory_order_release);
    sem_post(&data.buffer_ready_sem);  // Wake worker so it can exit
    if (data.worker_thread.joinable()) data.worker_thread.join();
    if (status_logger.joinable()) status_logger.join();

    sem_destroy(&data.buffer_ready_sem);
    pw_filter_destroy(data.filter);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(data.loop);
    
    // Detach from CLLS sync shared memory
    detach_clls_sync(slot_idx);
    
    pw_deinit();

    return 0;
}
