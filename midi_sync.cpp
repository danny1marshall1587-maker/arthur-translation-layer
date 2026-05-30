#include <alsa/asoundlib.h>
#include <iostream>
#include <cstring>
#include <atomic>
#include <thread>
#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include "AudioIPC.h"

// Global shutdown flag
std::atomic<bool> g_shutdown(false);

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_shutdown.store(true);
    }
}

using CLLSSyncLayout = arthur::CLLSSyncLayout;

static CLLSSyncLayout *g_sync_layout = nullptr;
static int g_shm_fd = -1;
static bool g_target_rtt_initialized = false;
static float g_static_target_rtt = 0.0f;

void attach_clls_sync() {
    const char *shm_name = "/arthur_clls_sync";
    g_shm_fd = shm_open(shm_name, O_RDONLY, 0600);
    if (g_shm_fd >= 0) {
        struct stat st;
        if (fstat(g_shm_fd, &st) == 0 && st.st_size >= (off_t)sizeof(CLLSSyncLayout)) {
            void *ptr = mmap(NULL, sizeof(CLLSSyncLayout), PROT_READ, MAP_SHARED, g_shm_fd, 0);
            if (ptr != MAP_FAILED) {
                g_sync_layout = static_cast<CLLSSyncLayout*>(ptr);
                std::cout << "[OK] MIDI Sync attached to CLLS Sync shared memory." << std::endl;
            }
        }
    } else {
        std::cout << "[WARNING] CLLS Sync shared memory not found. Running without latency pre-compensation." << std::endl;
    }
}

void detach_clls_sync() {
    if (g_sync_layout) {
        munmap(g_sync_layout, sizeof(CLLSSyncLayout));
        g_sync_layout = nullptr;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
}

int main(int argc, char *argv[]) {
    int card = 0;
    int device = 0;
    int subdevice = 0;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            card = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            subdevice = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: midi_sync [options]\n"
                      << "Options:\n"
                      << "  -c <card>       ALSA PCM Card number (default: 0)\n"
                      << "  -d <device>     ALSA PCM Device number (default: 0)\n"
                      << "  -s <subdevice>  ALSA PCM Subdevice number (default: 0)\n"
                      << "  -h              Show help\n";
            return 0;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=== Arthur: ALSA PCM-Slaved MIDI Sync ===" << std::endl;
    std::cout << "Target PCM Hardware Clock: hw:" << card << "," << device << "," << subdevice << std::endl;

    snd_seq_t *seq = nullptr;
    int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        std::cerr << "[ERROR] Failed to open ALSA Sequencer: " << snd_strerror(err) << std::endl;
        return 1;
    }

    snd_seq_set_client_name(seq, "Arthur-MIDI-Sync");

    // Create ports: Sync-In for receiving from DAW, Sync-Out for forwarding to hardware synth
    int in_port = snd_seq_create_simple_port(seq, "Sync-In",
                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    
    if (in_port < 0) {
        std::cerr << "[ERROR] Failed to create input port: " << snd_strerror(in_port) << std::endl;
        snd_seq_close(seq);
        return 1;
    }

    int out_port = snd_seq_create_simple_port(seq, "Sync-Out",
                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    
    if (out_port < 0) {
        std::cerr << "[ERROR] Failed to create output port: " << snd_strerror(out_port) << std::endl;
        snd_seq_close(seq);
        return 1;
    }

    std::cout << "[OK] MIDI Ports registered: Sync-In (ID: " << in_port << ") | Sync-Out (ID: " << out_port << ")" << std::endl;

    // Allocate a timing queue
    int queue = snd_seq_alloc_queue(seq);
    if (queue < 0) {
        std::cerr << "[ERROR] Failed to allocate sequencer queue: " << snd_strerror(queue) << std::endl;
        snd_seq_close(seq);
        return 1;
    }
    std::cout << "[OK] Sequencer Queue allocated (ID: " << queue << ")" << std::endl;

    // Set the queue timer to slave directly to the PCM device clock
    snd_seq_queue_timer_t *timer = nullptr;
    snd_seq_queue_timer_alloca(&timer);
    
    snd_seq_queue_timer_set_type(timer, SND_SEQ_TIMER_ALSA);

    snd_timer_id_t *tid = nullptr;
    snd_timer_id_alloca(&tid);
    snd_timer_id_set_class(tid, SND_TIMER_CLASS_PCM);
    snd_timer_id_set_card(tid, card);
    snd_timer_id_set_device(tid, device);
    snd_timer_id_set_subdevice(tid, subdevice);

    snd_seq_queue_timer_set_id(timer, tid);

    err = snd_seq_set_queue_timer(seq, queue, timer);
    if (err < 0) {
        std::cerr << "[WARNING] Failed to slave queue to PCM hw:" << card << "," << device 
                  << ". Error: " << snd_strerror(err) << std::endl;
        std::cerr << "Slaving to standard system timer instead..." << std::endl;
    } else {
        std::cout << "[OK] Queue timer slaved to physical PCM sample clock." << std::endl;
    }

    // Start the queue
    err = snd_seq_start_queue(seq, queue, NULL);
    if (err < 0) {
        std::cerr << "[ERROR] Failed to start sequencer queue: " << snd_strerror(err) << std::endl;
        snd_seq_close(seq);
        return 1;
    }
    snd_seq_drain_output(seq);
    std::cout << "[OK] Timing queue started." << std::endl;

    // Put sequencer in nonblocking mode
    snd_seq_nonblock(seq, 1);

    // Attach to CLLS shared memory for RTT tracking
    attach_clls_sync();

    // Prepare poll descriptors for efficient CPU usage
    int npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfds = (struct pollfd*)alloca(sizeof(struct pollfd) * npfds);
    snd_seq_poll_descriptors(seq, pfds, npfds, POLLIN);

    std::cout << ">>> Running event loop with dynamic pre-compensation. Listening..." << std::endl;

    snd_seq_event_t *ev = nullptr;
    while (!g_shutdown.load()) {
        if (poll(pfds, npfds, 100) > 0) { // 100ms timeout
            while (snd_seq_event_input(seq, &ev) >= 0 && ev != nullptr) {
                // Compute current latency offset across all active CLLS aligners
                float max_rtt = 0.0f;
                if (g_sync_layout) {
                    for (int i = 0; i < 8; ++i) {
                        int32_t pid = g_sync_layout->owner_pid[i].load(std::memory_order_acquire);
                        if (pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH)) {
                            float other_rtt = g_sync_layout->measured_rtt[i].load(std::memory_order_acquire);
                            if (other_rtt > max_rtt) {
                                max_rtt = other_rtt;
                            }
                        }
                    }

                    float current_global = g_sync_layout->global_target_rtt.load(std::memory_order_acquire);
                    if (current_global == 0.0f && max_rtt > 0.0f) {
                        float new_target = max_rtt + 128.0f;
                        if (new_target > 7936.0f) new_target = 7936.0f;
                        g_sync_layout->global_target_rtt.store(new_target, std::memory_order_release);
                        current_global = new_target;
                    }

                    if (current_global > 0.0f && (!g_target_rtt_initialized || g_static_target_rtt != current_global)) {
                        g_static_target_rtt = current_global;
                        g_target_rtt_initialized = true;
                        std::cout << "[MIDI SYNC] Frozen target latency updated to: " << g_static_target_rtt << " samples." << std::endl;
                    }
                }
                
                float target_rtt = g_target_rtt_initialized ? g_static_target_rtt : 0.0f;
                long tick = ev->time.tick;
                long adjusted_tick = tick - (long)target_rtt;
                if (adjusted_tick < 0) adjusted_tick = 0;

                // Prepare pre-compensated event for output
                snd_seq_event_t out_ev;
                std::memset(&out_ev, 0, sizeof(snd_seq_event_t));
                out_ev = *ev;
                
                out_ev.source.port = out_port;
                out_ev.dest.client = SND_SEQ_ADDRESS_SUBSCRIBERS;
                out_ev.dest.port = 0;
                out_ev.time.tick = adjusted_tick;
                out_ev.queue = queue;
                
                snd_seq_event_output(seq, &out_ev);
                snd_seq_drain_output(seq);

                std::cout << "[MIDI SYNC] Event type " << (int)ev->type 
                          << " | Original sample offset: " << tick 
                          << " | Adjusted: " << adjusted_tick 
                          << " | Pre-compensation: -" << max_rtt << " samples" << std::endl;

                snd_seq_free_event(ev);
            }
        }
    }

    // Clean up
    std::cout << ">>> Shutting down MIDI Sync..." << std::endl;
    detach_clls_sync();
    snd_seq_stop_queue(seq, queue, NULL);
    snd_seq_free_queue(seq, queue);
    snd_seq_close(seq);
    std::cout << "Done." << std::endl;

    return 0;
}
