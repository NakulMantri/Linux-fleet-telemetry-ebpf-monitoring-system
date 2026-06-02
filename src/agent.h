#ifndef __AGENT_H
#define __AGENT_H

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include "disk_io.h"

class TelemetryAgent {
public:
    TelemetryAgent(const std::string& socket_path, bool simulate_mode, bool verbose_mode = false);
    ~TelemetryAgent();

    // Start the agent lifecycle
    bool start();
    
    // Stop the agent
    void stop();

    // Broadcast a captured event to all connected socket clients
    void broadcast_event(const disk_io_event& event);

private:
    // Unix Domain Socket management
    void run_socket_server();
    void handle_client(int client_fd);
    void close_sockets();

    // eBPF kernel runtime management (Linux only)
    void run_ebpf();
    static int handle_ebpf_event(void *ctx, void *data, size_t data_sz);

    // Simulation Engine for testing/macOS runtime
    void run_simulator();

    std::string socket_path_;
    bool simulate_mode_;
    bool verbose_mode_;
    std::atomic<bool> running_;
    
    // Threads
    std::thread server_thread_;
    std::thread simulator_thread_;
    std::thread ebpf_thread_;

    // Client connection tracking
    std::mutex clients_mutex_;
    std::vector<int> client_fds_;

    // Server file descriptor
    int server_fd_;
};

#endif // __AGENT_H
