#ifndef __DISK_IO_H
#define __DISK_IO_H

#define TASK_COMM_LEN 16

// Event sent from eBPF kernel ring buffer to user space
struct disk_io_event {
    unsigned int pid;
    char comm[TASK_COMM_LEN];
    unsigned long long latency_ns;
    unsigned long long sectors;
    unsigned int dev_major;
    unsigned int dev_minor;
    unsigned char is_write;
};

#endif // __DISK_IO_H
