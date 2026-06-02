#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include "disk_io.h"

char LICENSE[] SEC("license") = "GPL";

// BPF Map to store start timestamps keyed by request pointer
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u64);       // request pointer address as u64
    __type(value, u64);     // start time in ns
} io_start_times SEC(".maps");

// BPF Ring Buffer to stream completed events to user space
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024); // 256KB ring buffer
} rb SEC(".maps");

// Helper to determine if an operation is a write
static __always_inline bool is_write_op(u32 cmd_flags) {
    // REQ_OP_WRITE is usually 1, cmd_flags & 1 is a robust check across kernel versions
    return (cmd_flags & 1) != 0;
}

// Trace request start
SEC("kprobe/blk_account_io_start")
int BPF_KPROBE(blk_account_io_start, struct request *rq) {
    u64 rq_addr = (u64)rq;
    u64 ts = bpf_ktime_get_ns();

    bpf_map_update_elem(&io_start_times, &rq_addr, &ts, BPF_ANY);
    return 0;
}

// Trace request completion
SEC("kprobe/blk_account_io_done")
int BPF_KPROBE(blk_account_io_done, struct request *rq) {
    u64 rq_addr = (u64)rq;
    u64 *start_ts;

    start_ts = bpf_map_lookup_elem(&io_start_times, &rq_addr);
    if (!start_ts) {
        return 0; // Missed start event
    }

    u64 end_ts = bpf_ktime_get_ns();
    u64 latency_ns = end_ts - *start_ts;

    // Delete tracked request to avoid memory leaks
    bpf_map_delete_elem(&io_start_times, &rq_addr);

    // Reserve space in Ring Buffer for user event
    struct disk_io_event *event;
    event = bpf_ringbuf_reserve(&rb, sizeof(*event), 0);
    if (!event) {
        return 0; // Ring buffer full
    }

    // Capture calling context
    event->pid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&event->comm, sizeof(event->comm));
    event->latency_ns = latency_ns;

    // Read sectors (data len / 512)
    u32 data_len = BPF_CORE_READ(rq, __data_len);
    event->sectors = data_len >> 9;

    // Read device major/minor from gendisk
    struct gendisk *disk = BPF_CORE_READ(rq, rq_disk);
    if (disk) {
        event->dev_major = BPF_CORE_READ(disk, major);
        event->dev_minor = BPF_CORE_READ(disk, first_minor);
    } else {
        event->dev_major = 0;
        event->dev_minor = 0;
    }

    // Read command flags to verify operation type
    u32 cmd_flags = BPF_CORE_READ(rq, cmd_flags);
    event->is_write = is_write_op(cmd_flags) ? 1 : 0;

    // Submit event to ring buffer
    bpf_ringbuf_submit(event, 0);

    return 0;
}
