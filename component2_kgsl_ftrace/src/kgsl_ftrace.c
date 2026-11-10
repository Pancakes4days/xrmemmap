/*
 * kgsl_ftrace.c — GPU virtual address capture via KGSL ftrace tracepoints
 */

#include "kgsl_ftrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define TRACE_DIR "/sys/kernel/debug/tracing"
#define TRACE_BUF (TRACE_DIR "/trace")
#define TRACE_ON  (TRACE_DIR "/tracing_on")

#define EVENT_MEM_ALLOC  TRACE_DIR "/events/kgsl/kgsl_mem_alloc/enable"
#define EVENT_MEM_MMAP   TRACE_DIR "/events/kgsl/kgsl_mem_mmap/enable"
#define EVENT_MEM_FREE   TRACE_DIR "/events/kgsl/kgsl_mem_free/enable"

static pid_t s_filter_tgid;

static int write_file(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return -1;
    ssize_t len = (ssize_t)strlen(val);
    ssize_t wr = write(fd, val, len);
    close(fd);
    return (wr == len) ? 0 : -1;
}

int kgsl_trace_start(pid_t filter_tgid)
{
    s_filter_tgid = filter_tgid;

    if (write_file(TRACE_ON, "0") < 0)
        return -1;
    if (write_file(TRACE_BUF, "0") < 0)
        return -1;

    if (write_file(EVENT_MEM_ALLOC, "1") < 0)
        return -1;
    if (write_file(EVENT_MEM_MMAP, "1") < 0)
        return -1;
    if (write_file(EVENT_MEM_FREE, "1") < 0)
        return -1;
    if (write_file(TRACE_ON, "1") < 0)
        return -1;

    return 0;
}

int kgsl_trace_stop(void)
{
    int err = 0;
    if (write_file(TRACE_ON, "0") < 0)
        err = -1;
    if (write_file(EVENT_MEM_ALLOC, "0") < 0)
        err = -1;
    if (write_file(EVENT_MEM_MMAP, "0") < 0)
        err = -1;
    if (write_file(EVENT_MEM_FREE, "0") < 0)
        err = -1;
    return err;
}

/* Parse a key=value pair from a trace line. Returns pointer past consumed
 * portion, or NULL if key not found at current position. */
static const char *parse_kv_u64(const char *p, const char *key, uint64_t *out)
{
    size_t klen = strlen(key);
    if (strncmp(p, key, klen) != 0 || p[klen] != '=')
        return NULL;
    p += klen + 1;
    *out = strtoull(p, (char **)&p, 0);
    while (*p == ' ')
        p++;
    return p;
}

static const char *parse_kv_uint(const char *p, const char *key, unsigned int *out)
{
    size_t klen = strlen(key);
    if (strncmp(p, key, klen) != 0 || p[klen] != '=')
        return NULL;
    p += klen + 1;
    *out = (unsigned int)strtoul(p, (char **)&p, 0);
    while (*p == ' ')
        p++;
    return p;
}

static const char *parse_kv_str(const char *p, const char *key, char *out, size_t outlen)
{
    size_t klen = strlen(key);
    if (strncmp(p, key, klen) != 0 || p[klen] != '=')
        return NULL;
    p += klen + 1;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
    while (*p == ' ')
        p++;
    return p;
}

/*
 * Find or create a mapping entry by kgsl_id in out[0..count).
 * If the id already exists, return its index. Otherwise, if count < max,
 * initialize a new entry at out[count] and increment *pcount.
 * Returns index, or -1 if full.
 */
static int find_or_create(kgsl_mapping_t *out, int *pcount, int max, unsigned int id)
{
    for (int i = 0; i < *pcount; i++) {
        if (out[i].kgsl_id == id)
            return i;
    }
    if (*pcount >= max)
        return -1;
    int idx = *pcount;
    memset(&out[idx], 0, sizeof(out[idx]));
    out[idx].kgsl_id = id;
    (*pcount)++;
    return idx;
}

/*
 * Parse one trace line. Lines look like:
 *   test_vulkan-7268  [005] ...1  1321.225462: kgsl_mem_alloc: gpuaddr=0x500028000 size=4198400 tgid=7268 usage=vk_queue id=11 flags=0xc2400
 *   test_vulkan-7268  [005] ...1  1321.225470: kgsl_mem_mmap: useraddr=0x7c88bf7000 gpuaddr=0x500028000 size=4198400 usage=vk_queue id=11 flags=0xc2400
 *   test_vulkan-7268  [002] ...1  1321.262806: kgsl_mem_free: gpuaddr=0x50042a000 size=262144 type=gpumem tgid=7268 usage=vk_devicememory id=13
 */
static void parse_line(const char *line, kgsl_mapping_t *out, int *count, int max,
                       pid_t filter_tgid)
{
    const char *event;

    event = strstr(line, "kgsl_mem_alloc: ");
    if (event) {
        event += 16; /* skip "kgsl_mem_alloc: " */
        uint64_t gpuaddr = 0, size = 0, flags = 0;
        unsigned int tgid = 0, id = 0;
        char usage[KGSL_USAGE_MAX] = {0};
        const char *p = event;
        const char *next;

        while (*p && *p != '\n') {
            if ((next = parse_kv_u64(p, "gpuaddr", &gpuaddr)))  { p = next; continue; }
            if ((next = parse_kv_u64(p, "size", &size)))         { p = next; continue; }
            if ((next = parse_kv_u64(p, "flags", &flags)))       { p = next; continue; }
            if ((next = parse_kv_uint(p, "tgid", &tgid)))        { p = next; continue; }
            if ((next = parse_kv_uint(p, "id", &id)))            { p = next; continue; }
            if ((next = parse_kv_str(p, "usage", usage, sizeof(usage)))) { p = next; continue; }
            p++;
        }

        if (filter_tgid > 0 && (pid_t)tgid != filter_tgid)
            return;

        int idx = find_or_create(out, count, max, id);
        if (idx < 0) return;
        out[idx].gpuaddr = gpuaddr;
        out[idx].size    = size;
        out[idx].flags   = flags;
        out[idx].tgid    = tgid;
        memcpy(out[idx].usage, usage, KGSL_USAGE_MAX);
        return;
    }

    event = strstr(line, "kgsl_mem_mmap: ");
    if (event) {
        event += 15;
        uint64_t useraddr = 0, gpuaddr = 0, size = 0, flags = 0;
        unsigned int id = 0;
        char usage[KGSL_USAGE_MAX] = {0};
        const char *p = event;
        const char *next;

        while (*p && *p != '\n') {
            if ((next = parse_kv_u64(p, "useraddr", &useraddr))) { p = next; continue; }
            if ((next = parse_kv_u64(p, "gpuaddr", &gpuaddr)))   { p = next; continue; }
            if ((next = parse_kv_u64(p, "size", &size)))          { p = next; continue; }
            if ((next = parse_kv_u64(p, "flags", &flags)))        { p = next; continue; }
            if ((next = parse_kv_uint(p, "id", &id)))             { p = next; continue; }
            if ((next = parse_kv_str(p, "usage", usage, sizeof(usage)))) { p = next; continue; }
            p++;
        }

        /* mmap events don't carry tgid; correlate by existing alloc entry.
         * When filtering by tgid, only populate entries that an alloc already
         * created — don't create new entries from unfiltered mmap events. */
        int idx;
        if (filter_tgid > 0) {
            idx = -1;
            for (int i = 0; i < *count; i++) {
                if (out[i].kgsl_id == id) { idx = i; break; }
            }
            if (idx < 0) return;
        } else {
            idx = find_or_create(out, count, max, id);
            if (idx < 0) return;
        }
        out[idx].useraddr = useraddr;
        if (!out[idx].gpuaddr) out[idx].gpuaddr = gpuaddr;
        if (!out[idx].size)    out[idx].size = size;
        if (!out[idx].flags)   out[idx].flags = flags;
        if (!out[idx].usage[0]) memcpy(out[idx].usage, usage, KGSL_USAGE_MAX);
        return;
    }

    event = strstr(line, "kgsl_mem_free: ");
    if (event) {
        event += 15;
        unsigned int id = 0;
        const char *p = event;
        const char *next;

        while (*p && *p != '\n') {
            if ((next = parse_kv_uint(p, "id", &id))) { p = next; continue; }
            p++;
        }

        for (int i = 0; i < *count; i++) {
            if (out[i].kgsl_id == id) {
                out[i].freed = 1;
                break;
            }
        }
    }
}

int kgsl_trace_read(kgsl_mapping_t *out, int max_entries)
{
    FILE *fp = fopen(TRACE_BUF, "r");
    if (!fp)
        return -1;

    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#')
            continue;
        if (!strstr(line, "kgsl_mem"))
            continue;
        parse_line(line, out, &count, max_entries, s_filter_tgid);
    }

    fclose(fp);
    return count;
}

const kgsl_mapping_t *kgsl_trace_find_by_useraddr(
    const kgsl_mapping_t *mappings, int count, uint64_t addr)
{
    for (int i = 0; i < count; i++) {
        if (mappings[i].useraddr &&
            addr >= mappings[i].useraddr &&
            addr <  mappings[i].useraddr + mappings[i].size)
            return &mappings[i];
    }
    return NULL;
}

const kgsl_mapping_t *kgsl_trace_find_by_gpuaddr(
    const kgsl_mapping_t *mappings, int count, uint64_t addr)
{
    for (int i = 0; i < count; i++) {
        if (mappings[i].gpuaddr &&
            addr >= mappings[i].gpuaddr &&
            addr <  mappings[i].gpuaddr + mappings[i].size)
            return &mappings[i];
    }
    return NULL;
}
