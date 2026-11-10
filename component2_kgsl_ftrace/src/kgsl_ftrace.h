/*
 * kgsl_ftrace.h — GPU virtual address capture via KGSL ftrace tracepoints
 *
 * Part of xrmemmap. Captures GPU ↔ CPU address mappings emitted by
 * Qualcomm's KGSL driver through the kernel's ftrace subsystem.
 *
 * Requires root (CAP_SYS_ADMIN) to read/write /sys/kernel/debug/tracing/.
 * CONFIG_KPROBES=n on this kernel, so only tracepoint events work.
 *
 * Key tracepoints used:
 *   kgsl/kgsl_mem_alloc — GPU memory allocation (gpuaddr, size, usage, flags)
 *   kgsl/kgsl_mem_mmap  — CPU↔GPU mapping     (useraddr, gpuaddr, size)
 *   kgsl/kgsl_mem_free  — GPU memory free      (gpuaddr, size)
 */

#ifndef XRMEMMAP_KGSL_FTRACE_H
#define XRMEMMAP_KGSL_FTRACE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KGSL_USAGE_MAX 16

/* A single KGSL memory mapping captured from ftrace. */
typedef struct {
    uint64_t     gpuaddr;             /* GPU virtual address */
    uint64_t     useraddr;            /* CPU virtual address (from mmap) */
    uint64_t     size;                /* allocation size in bytes */
    uint64_t     flags;               /* KGSL allocation flags */
    unsigned int kgsl_id;             /* KGSL internal allocation id */
    unsigned int tgid;                /* thread group (process) id */
    char         usage[KGSL_USAGE_MAX]; /* usage tag (e.g. "vk_devicememory") */
    int          freed;               /* 1 if a kgsl_mem_free was seen */
} kgsl_mapping_t;

/*
 * kgsl_trace_start — enable KGSL ftrace events and clear the trace buffer.
 *
 * filter_tgid: if > 0, only capture events from this thread group.
 *              if 0, capture from all processes.
 *
 * Returns 0 on success, -1 on error (errno set).
 */
int kgsl_trace_start(pid_t filter_tgid);

/*
 * kgsl_trace_stop — disable KGSL ftrace events.
 *
 * Returns 0 on success, -1 on error.
 */
int kgsl_trace_stop(void);

/*
 * kgsl_trace_read — read the trace buffer and parse KGSL events.
 *
 * Populates out[] with up to max_entries mappings found in the trace buffer.
 * Alloc and mmap events are correlated by kgsl_id: the returned entries
 * contain both the GPU address (from alloc) and CPU address (from mmap).
 *
 * Returns the number of mappings written, or -1 on error.
 */
int kgsl_trace_read(kgsl_mapping_t *out, int max_entries);

/*
 * kgsl_trace_find_by_useraddr — find the mapping whose CPU VA range
 * contains the given address.
 *
 * Searches mappings[0..count) for one where
 *   useraddr <= addr < useraddr + size
 *
 * Returns a pointer to the matching entry, or NULL if not found.
 */
const kgsl_mapping_t *kgsl_trace_find_by_useraddr(
    const kgsl_mapping_t *mappings, int count, uint64_t addr);

/*
 * kgsl_trace_find_by_gpuaddr — find the mapping whose GPU VA range
 * contains the given address.
 *
 * Returns a pointer to the matching entry, or NULL if not found.
 */
const kgsl_mapping_t *kgsl_trace_find_by_gpuaddr(
    const kgsl_mapping_t *mappings, int count, uint64_t addr);

#ifdef __cplusplus
}
#endif

#endif /* XRMEMMAP_KGSL_FTRACE_H */
