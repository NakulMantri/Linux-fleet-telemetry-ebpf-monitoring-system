#include "agent.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#ifndef DISABLE_BPF
#include <bpf/libbpf.h>
#include "disk_io.skel.h"
#endif

// Pointer to active agent for static eBPF callbacks
static TelemetryAgent* g_agent = nullptr;

TelemetryAgent::TelemetryAgent(const std::string& socket_path, bool simulate_mode, bool verbose_mode)
    : socket_path_(socket_path),
      simulate_mode_(simulate_mode),
      verbose_mode_(verbose_mode),
      running_(false),
      server_fd_(-1) {
    g_agent = this;
}

TelemetryAgent::~TelemetryAgent() {
    stop();
    if (g_agent == this) {
        g_agent = nullptr;
    }
}

bool TelemetryAgent::start() {
    running_ = true;

    // Start Unix Domain Socket server to stream telemetry
    server_thread_ = std::thread(&TelemetryAgent::run_socket_server, this);

    if (simulate_mode_) {
        std::cout << "[INFO] Running in SIMULATION Mode. High-fidelity synthetic event generator started." << std::endl;
        simulator_thread_ = std::thread(&TelemetryAgent::run_simulator, this);
    } else {
#ifdef DISABLE_BPF
        std::cerr << "[ERROR] BPF compilation was disabled at build time. Forcing Simulation Mode." << std::endl;
        simulate_mode_ = true;
        simulator_thread_ = std::thread(&TelemetryAgent::run_simulator, this);
#else
        std::cout << "[INFO] Running in PRODUCTION Mode. Attaching eBPF kernel hooks..." << std::endl;
        ebpf_thread_ = std::thread(&TelemetryAgent::run_ebpf, this);
#endif
    }

    return true;
}

void TelemetryAgent::stop() {
    if (!running_) return;
    running_ = false;

    std::cout << "[INFO] Shutting down agent..." << std::endl;

    // Close socket server to wake up accept loop
    close_sockets();

    if (server_thread_.joinable()) server_thread_.join();
    if (simulator_thread_.joinable()) simulator_thread_.join();
    if (ebpf_thread_.joinable()) ebpf_thread_.join();

    std::cout << "[INFO] Agent shut down completed." << std::endl;
}

// Unix Domain Socket Server Implementation
void TelemetryAgent::run_socket_server() {
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[ERROR] Failed to create socket: " << strerror(errno) << std::endl;
        return;
    }

    // Unlink old socket file if it exists
    unlink(socket_path_.c_str());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to bind socket: " << strerror(errno) << std::endl;
        close(server_fd_);
        return;
    }

    // Grant read/write permission on socket file for companion pipeline
    chmod(socket_path_.c_str(), 0666);

    if (listen(server_fd_, 10) < 0) {
        std::cerr << "[ERROR] Failed to listen: " << strerror(errno) << std::endl;
        close(server_fd_);
        return;
    }

    std::cout << "[INFO] Socket server listening on UDS: " << socket_path_ << std::endl;

    while (running_) {
        // Use select to support graceful shutdown on accept
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd_, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int retval = select(server_fd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (retval == -1) {
            if (errno == EINTR) continue;
            break;
        } else if (retval == 0) {
            continue; // Timeout, check if still running
        }

        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd >= 0) {
            std::cout << "[INFO] New python pipeline client connected." << std::endl;
            
            // Set socket non-blocking to prevent slow pipeline from slowing C++ agent
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
        }
    }
}

void TelemetryAgent::broadcast_event(const disk_io_event& event) {
    // Format event as highly structured compact JSON line
    std::ostringstream ss;
    ss << "{\"pid\":" << event.pid 
       << ",\"comm\":\"" << event.comm << "\""
       << ",\"latency_ns\":" << event.latency_ns
       << ",\"sectors\":" << event.sectors
       << ",\"dev_major\":" << event.dev_major
       << ",\"dev_minor\":" << event.dev_minor
       << ",\"is_write\":" << (int)event.is_write << "}\n";
    
    std::string data = ss.str();

    if (verbose_mode_) {
        std::cout << "[EVENT] " << data << std::flush;
    }

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = client_fds_.begin(); it != client_fds_.end();) {
        int fd = *it;
        // Send JSON data. MSG_NOSIGNAL prevents crash if client disconnected
        ssize_t bytes_sent = send(fd, data.c_str(), data.length(), MSG_NOSIGNAL);
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Client buffer full, skip to maintain overhead under 1%
                // (In production, a ring buffer would drop events to protect the sender)
                ++it;
            } else {
                // Client disconnected
                std::cout << "[INFO] Pipeline client disconnected." << std::endl;
                close(fd);
                it = client_fds_.erase(it);
            }
        } else {
            ++it;
        }
    }
}

void TelemetryAgent::close_sockets() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (int fd : client_fds_) {
        close(fd);
    }
    client_fds_.clear();

    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    unlink(socket_path_.c_str());
}

// eBPF Production Mode - Linux Only
void TelemetryAgent::run_ebpf() {
#ifndef DISABLE_BPF
    struct disk_io_bpf *skel = nullptr;
    struct ring_buffer *rb = nullptr;
    int err;

    // Load and verify eBPF program
    skel = disk_io_bpf__open_and_load();
    if (!skel) {
        std::cerr << "[ERROR] Failed to open and load eBPF skeleton. Make sure you are root." << std::endl;
        std::cerr << "[INFO] Falling back to Simulation Mode..." << std::endl;
        simulate_mode_ = true;
        simulator_thread_ = std::thread(&TelemetryAgent::run_simulator, this);
        return;
    }

    // Attach tracepoints/kprobes
    err = disk_io_bpf__attach(skel);
    if (err) {
        std::cerr << "[ERROR] Failed to attach eBPF probes: " << strerror(-err) << std::endl;
        disk_io_bpf__destroy(skel);
        return;
    }

    // Setup Ring Buffer poll
    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_ebpf_event, nullptr, nullptr);
    if (!rb) {
        std::cerr << "[ERROR] Failed to create ring buffer observer" << std::endl;
        disk_io_bpf__destroy(skel);
        return;
    }

    std::cout << "[INFO] eBPF hooks attached successfully. Recording active." << std::endl;

    while (running_) {
        // Poll ring buffer with a 100ms timeout
        err = ring_buffer__poll(rb, 100);
        if (err < 0 && err != -EINTR) {
            std::cerr << "[ERROR] Error polling BPF ring buffer: " << err << std::endl;
            break;
        }
    }

    ring_buffer__free(rb);
    disk_io_bpf__destroy(skel);
#endif
}

#ifndef DISABLE_BPF
int TelemetryAgent::handle_ebpf_event(void *ctx, void *data, size_t data_sz) {
    if (data_sz < sizeof(disk_io_event)) return -1;
    
    const disk_io_event* event = (const disk_io_event*)data;
    if (g_agent) {
        g_agent->broadcast_event(*event);
    }
    return 0;
}
#endif

// High-Fidelity Simulator for macOS / Test Runs
void TelemetryAgent::run_simulator() {
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Simulate process distributions
    std::vector<std::string> processes = {
        "postgres", "nginx", "systemd-journal", "dockerd", "prometheus", "rsyslogd", "dd", "tar", "kworker/u2:1"
    };
    std::discrete_distribution<> proc_dist({30, 20, 15, 10, 10, 5, 4, 3, 3});

    // Simulate sector size (read/write transfer block sizes)
    std::vector<int> sector_sizes = {8, 16, 32, 64, 128, 256, 512, 1024};
    std::discrete_distribution<> sector_dist({40, 20, 15, 10, 5, 5, 3, 2});

    // Generate log-normal latency distributions for realistic random spikes
    // Mean = 1.2ms, Dev = 0.8ms for normal reads
    std::lognormal_distribution<double> read_lat_dist(7.0, 0.6);   // ~1ms normal operations
    std::lognormal_distribution<double> write_lat_dist(7.5, 0.8);  // ~2ms normal operations

    std::uniform_real_distribution<double> event_rate_dist(0.01, 0.15); // Wait time between operations (seconds)
    std::uniform_int_distribution<> dev_minor_dist(0, 4);

    unsigned long long event_count = 0;
    while (running_) {
        // Sleep for a short randomized period to generate continuous disk traffic
        double wait_sec = event_rate_dist(gen);
        std::this_thread::sleep_for(std::chrono::duration<double>(wait_sec));

        if (!running_) break;

        disk_io_event event;
        event.pid = 1000 + (gen() % 30000);
        
        std::string proc = processes[proc_dist(gen)];
        strncpy(event.comm, proc.c_str(), sizeof(event.comm) - 1);
        event.comm[sizeof(event.comm) - 1] = '\0';

        event.sectors = sector_sizes[sector_dist(gen)];
        event.dev_major = 8; // Block device standard (e.g., sda)
        event.dev_minor = dev_minor_dist(gen);
        
        // 40% writes, 60% reads
        event.is_write = (gen() % 10 < 4) ? 1 : 0;

        // Apply realistic latency matching the type of I/O (writes are generally slower)
        double latency_ns = 0;
        if (event.is_write) {
            latency_ns = write_lat_dist(gen);
        } else {
            latency_ns = read_lat_dist(gen);
        }
        
        // Add random high-latency spike every ~25 events (e.g. simulating disk cache flush or queue congestion)
        if (gen() % 25 == 0) {
            latency_ns *= 8.0; // Spike up to 50ms - 200ms
        }

        event.latency_ns = (unsigned long long)latency_ns;

        // Broadcast to Python pipeline
        broadcast_event(event);

        event_count++;
        if (!verbose_mode_ && event_count % 50 == 0) {
            std::cout << "[INFO] Generated " << event_count << " simulated disk I/O events. Telemetry active." << std::endl;
        }
    }
}

// Graceful signal handler
static std::atomic<bool> g_quit(false);
void signal_handler(int sig) {
    g_quit = true;
}

int main(int argc, char* argv[]) {
    // Register signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    bool simulate = false;
    bool verbose = false;
    std::string socket_path = "/tmp/telemetry.sock";

    // Argument parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--simulate" || arg == "-s") {
            simulate = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -s, --simulate      Force high-fidelity disk event simulation mode (required on macOS)" << std::endl;
            std::cout << "  -v, --verbose       Print every disk I/O event directly to standard output" << std::endl;
            std::cout << "  --socket <path>     Path to output Unix Domain Socket (default: /tmp/telemetry.sock)" << std::endl;
            std::cout << "  -h, --help          Show this help description" << std::endl;
            return 0;
        }
    }

#ifdef __APPLE__
    // Always force simulation on macOS since eBPF is Linux-specific
    simulate = true;
#endif

    std::cout << "==================================================" << std::endl;
    std::cout << "   Lniux Fleet Telemtery & eBPF monitoring system  " << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "[NOTICE] Telemetry is streamed silently to UDS (/tmp/telemetry.sock) to keep CPU <1%." << std::endl;
    std::cout << "[NOTICE] To view the scrolling live events in this console, run with: -v or --verbose" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    TelemetryAgent agent(socket_path, simulate, verbose);
    if (!agent.start()) {
        std::cerr << "[FATAL] Failed to start telemetry agent." << std::endl;
        return 1;
    }

    // Keep running until signal received
    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    agent.stop();
    return 0;
}
