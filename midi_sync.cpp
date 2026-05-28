#include <alsa/asoundlib.h>
#include <iostream>
#include <cstring>
#include <atomic>
#include <thread>
#include <csignal>

// Global shutdown flag
std::atomic<bool> g_shutdown(false);

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_shutdown.store(true);
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

    // Create simple input port to receive MIDI events
    int port = snd_seq_create_simple_port(seq, "MIDI-Input",
                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    
    if (port < 0) {
        std::cerr << "[ERROR] Failed to create sequencer port: " << snd_strerror(port) << std::endl;
        snd_seq_close(seq);
        return 1;
    }
    std::cout << "[OK] MIDI Input Port registered (ID: " << port << ")" << std::endl;

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
    
    // Set timer type to hardware PCM slaving
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

    // Put sequencer in nonblocking mode for event polling
    snd_seq_nonblock(seq, 1);

    std::cout << ">>> Running event loop. Incoming events will print with PCM sample offsets." << std::endl;

    snd_seq_event_t *ev = nullptr;
    while (!g_shutdown.load()) {
        err = snd_seq_event_input(seq, &ev);
        if (err >= 0 && ev != nullptr) {
            // Check if the event is timestamped
            long tick = ev->time.tick;
            std::cout << "[MIDI EVENT] Type: " << (int)ev->type 
                      << " | Source: " << (int)ev->source.client << ":" << (int)ev->source.port
                      << " | Tick (PCM Sample Offset): " << tick << std::endl;

            // Handle custom MIDI forwarding if needed...
            snd_seq_free_event(ev);
        } else {
            // Yield CPU slice
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Stop and free queue
    std::cout << ">>> Shutting down MIDI Sync..." << std::endl;
    snd_seq_stop_queue(seq, queue, NULL);
    snd_seq_free_queue(seq, queue);
    snd_seq_close(seq);
    std::cout << "Done." << std::endl;

    return 0;
}
