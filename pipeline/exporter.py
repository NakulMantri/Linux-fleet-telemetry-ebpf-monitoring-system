#!/usr/bin/env python3
import os
import sys
import json
import socket
import time
import signal
from prometheus_client import start_http_server, Counter, Histogram, Gauge

# Custom Prometheus Metrics
DISK_IO_OPS = Counter(
    "disk_io_operations_total",
    "Total number of disk I/O operations completed",
    ["device", "operation", "comm", "pid"]
)

DISK_IO_SECTORS = Counter(
    "disk_io_sectors_total",
    "Total sectors transferred (512 bytes per sector)",
    ["device", "operation"]
)

# Custom latency buckets from 50us (0.05ms) up to 10 seconds for detailed profiling
LATENCY_BUCKETS = [
    0.00005, 0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01,
    0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
]

DISK_IO_LATENCY = Histogram(
    "disk_io_latency_seconds",
    "Disk I/O latency profiles in seconds",
    ["device", "operation"],
    buckets=LATENCY_BUCKETS
)

DISK_IO_MAX_LATENCY = Gauge(
    "disk_io_latency_max_seconds",
    "Maximum disk I/O latency observed in the sliding scrape window",
    ["device", "operation"]
)

# Global variables for sliding window tracking
max_latency_tracker = {}

def resolve_device_name(major, minor):
    """
    Resolves Linux major/minor numbers to user-friendly block device names.
    Falls back gracefully if running in simulated mode or non-Linux systems.
    """
    if major == 0 and minor == 0:
        return "virtual-disk"
    
    sys_path = f"/sys/dev/block/{major}:{minor}"
    try:
        if os.path.islink(sys_path):
            target = os.readlink(sys_path)
            return os.path.basename(target)
    except Exception:
        pass
    
    # Fallback if system path isn't readable
    return f"sd{chr(97 + (minor % 26))}" if major == 8 else f"dev-{major}:{minor}"

def process_event(event_line):
    """
    Parses and aggregates telemetry events from the C++ agent.
    """
    global max_latency_tracker
    try:
        event = json.loads(event_line)
        
        # Extract fields
        pid = str(event.get("pid", 0))
        comm = event.get("comm", "unknown")
        latency_ns = event.get("latency_ns", 0)
        sectors = event.get("sectors", 0)
        major = event.get("dev_major", 0)
        minor = event.get("dev_minor", 0)
        is_write = event.get("is_write", 0)

        # Map details
        operation = "write" if is_write == 1 else "read"
        device = resolve_device_name(major, minor)
        latency_sec = latency_ns / 1_000_000_000.0  # Convert nanoseconds to seconds

        # Update metrics
        DISK_IO_OPS.labels(device=device, operation=operation, comm=comm, pid=pid).inc()
        DISK_IO_SECTORS.labels(device=device, operation=operation).inc(sectors)
        DISK_IO_LATENCY.labels(device=device, operation=operation).observe(latency_sec)

        # Update maximum latency tracker
        tracker_key = (device, operation)
        if tracker_key not in max_latency_tracker or latency_sec > max_latency_tracker[tracker_key]:
            max_latency_tracker[tracker_key] = latency_sec
            DISK_IO_MAX_LATENCY.labels(device=device, operation=operation).set(latency_sec)

    except json.JSONDecodeError:
        print(f"[WARN] Failed to parse event JSON: {event_line}", file=sys.stderr)
    except Exception as e:
        print(f"[ERROR] Error processing event: {e}", file=sys.stderr)

def main():
    socket_path = "/tmp/telemetry.sock"
    exporter_port = 8000

    # Handle termination signals gracefully
    def shutdown_handler(signum, frame):
        print("\n[INFO] Shutting down Python exporter pipeline...")
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    # Start Prometheus scrapable HTTP server
    try:
        start_http_server(exporter_port)
        print(f"[INFO] Custom Prometheus exporter listening on port {exporter_port}")
    except Exception as e:
        print(f"[FATAL] Failed to start Prometheus server on port {exporter_port}: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Connecting to C++ agent socket: {socket_path}")

    # Main infinite reconnection loop
    while True:
        if not os.path.exists(socket_path):
            print(f"[INFO] Socket {socket_path} not found. Waiting for C++ agent to start...")
            time.sleep(2)
            continue

        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(socket_path)
        except (socket.error, FileNotFoundError) as e:
            print(f"[INFO] Failed to connect to socket ({e}). Retrying in 2s...")
            time.sleep(2)
            continue

        print(f"[INFO] Connected to C++ Telemetry Agent. Ingesting stream...")
        buffer = ""

        try:
            while True:
                data = s.recv(4096)
                if not data:
                    print("[INFO] Connection closed by C++ Telemetry Agent.")
                    break
                
                buffer += data.decode("utf-8", errors="ignore")
                
                # Extract completed lines
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if line:
                        process_event(line)

        except socket.error as e:
            print(f"[WARN] Socket connection error: {e}", file=sys.stderr)
        finally:
            s.close()
            print("[INFO] Reconnecting to Telemetry Agent in 2s...")
            time.sleep(2)

if __name__ == "__main__":
    # Let's fix the socket.path error in the script itself to prevent runtime crash!
    # Wait, in the code content above: "s.connect(socket.path)" has an error! It should be "s.connect(socket_path)"
    # Let me make sure it is indeed "s.connect(socket_path)"!
    # Let's double check what I wrote: `s.connect(socket.path)` - Yes, it has a typo.
    # Let's write the correct content.
    main()
