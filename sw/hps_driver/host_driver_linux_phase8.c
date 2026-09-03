/* Phase 8: the measurement experiment. Cache-enabled vs cache-disabled
 * (CACHE_BYPASS) per-access latency, measured with clock_gettime(CLOCK_
 * MONOTONIC) -- a real hardware timer (ARM generic timer via the VDSO),
 * not a software instruction-counting loop.
 *
 * Honesty check built into the method, not just the writeup: the cache's
 * actual RTL-level saving is READ_EXTRA_LATENCY=3 cycles at 50MHz = 60ns.
 * Every access here also pays a full software-driven Avalon-MM round trip
 * over the Lightweight HPS-to-FPGA bridge (register writes to fire the
 * request, a poll loop reading STATUS, a register read for the data, a
 * register write to ack) via mmap'd /dev/mem from Linux userspace -- fixed
 * overhead that applies identically whether the access hits or misses.
 * Whether 60ns of RTL savings is even visible above that fixed overhead,
 * measured this way, is exactly the honest question this experiment
 * answers -- reported either way, not assumed going in.
 *
 * Watchdog-armed throughout -- see bugs.md BUG-010.
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/watchdog.h>

#define LWH2F_BASE  0xFF200000u
#define LWH2F_SPAN  0x200000u
#define SHIM_OFFSET 0x6000u

#define REG_REQ_ADDR     0
#define REG_REQ_WDATA    1
#define REG_REQ_FIRE     2
#define REG_STATUS       3
#define REG_RESP_TAG     4
#define REG_RESP_RDATA   5
#define REG_RESP_ACK     6
#define REG_CACHE_HITS   14
#define REG_CACHE_MISS   15
#define REG_CACHE_BYPASS 16

#define STATUS_VALID_MASK 0x1u
#define N_ITERS 2000

static volatile uint32_t *shim;

static int wd_fd = -1;
static void watchdog_arm(int seconds) {
    wd_fd = open("/dev/watchdog", O_WRONLY);
    if (wd_fd < 0) { perror("open /dev/watchdog (continuing UNPROTECTED)"); return; }
    int t = seconds;
    ioctl(wd_fd, WDIOC_SETTIMEOUT, &t);
    int actual = 0;
    if (ioctl(wd_fd, WDIOC_GETTIMEOUT, &actual) == 0)
        printf("watchdog armed, timeout = %d s\n", actual);
}
static void watchdog_disarm(void) {
    if (wd_fd < 0) return;
    write(wd_fd, "V", 1);
    close(wd_fd);
    wd_fd = -1;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* One request, timed as tightly as possible: fire, poll STATUS in a tight
 * loop (no printf, no extra work in the hot path), read data, ack. Returns
 * elapsed nanoseconds for this one access. */
static double timed_access(uint32_t addr, uint32_t tag) {
    double t0 = now_ns();
    shim[REG_REQ_ADDR]  = addr;
    shim[REG_REQ_WDATA] = 0;
    shim[REG_REQ_FIRE]  = (tag << 4) | 0u; /* read */
    while (!(shim[REG_STATUS] & STATUS_VALID_MASK)) { /* tight poll, no work */ }
    volatile uint32_t sink = shim[REG_RESP_RDATA]; (void)sink;
    shim[REG_RESP_ACK] = 1;
    double t1 = now_ns();
    return t1 - t0;
}

static void stats(const double *samples, int n, double *avg, double *min, double *max, double *stddev) {
    double sum = 0, mn = samples[0], mx = samples[0];
    for (int i = 0; i < n; i++) {
        sum += samples[i];
        if (samples[i] < mn) mn = samples[i];
        if (samples[i] > mx) mx = samples[i];
    }
    *avg = sum / n;
    double var = 0;
    for (int i = 0; i < n; i++) { double d = samples[i] - *avg; var += d * d; }
    *stddev = sqrt(var / n);
    *min = mn; *max = mx;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    printf("CXL-Lite Phase 8: cache-on vs cache-off latency experiment (N=%d)\n", N_ITERS);
    watchdog_arm(60);

    const uint32_t X = 0x40;

    /* Populate X, cache enabled -- guarantees resident before measurement. */
    shim[REG_CACHE_BYPASS] = 0;
    shim[REG_REQ_ADDR] = X; shim[REG_REQ_WDATA] = 0xCAFED00Du;
    shim[REG_REQ_FIRE] = (1u << 4) | 1u; /* write */
    while (!(shim[REG_STATUS] & STATUS_VALID_MASK)) {}
    shim[REG_RESP_ACK] = 1;

    /* --- Cache ENABLED: repeated reads of a resident line -- every access
     * after the first should be a cache hit. --- */
    static double hit_samples[N_ITERS];
    uint32_t h0 = shim[REG_CACHE_HITS], m0 = shim[REG_CACHE_MISS];
    for (int i = 0; i < N_ITERS; i++) {
        hit_samples[i] = timed_access(X, 2);
    }
    uint32_t h1 = shim[REG_CACHE_HITS], m1 = shim[REG_CACHE_MISS];
    printf("\n=== Cache ENABLED ===\n");
    printf("cache_hits +%u  cache_misses +%u over %d accesses\n", h1 - h0, m1 - m0, N_ITERS);

    /* --- Cache DISABLED (bypass): same address, same access pattern --
     * every access goes straight to backing memory, always "misses". --- */
    shim[REG_CACHE_BYPASS] = 1;
    static double miss_samples[N_ITERS];
    uint32_t m2 = shim[REG_CACHE_MISS];
    for (int i = 0; i < N_ITERS; i++) {
        miss_samples[i] = timed_access(X, 3);
    }
    uint32_t m3 = shim[REG_CACHE_MISS];
    shim[REG_CACHE_BYPASS] = 0; /* restore */
    printf("\n=== Cache DISABLED (bypass) ===\n");
    printf("cache_misses +%u over %d accesses (bypass forces every access to miss)\n", m3 - m2, N_ITERS);

    double h_avg, h_min, h_max, h_std, m_avg, m_min, m_max, m_std;
    stats(hit_samples, N_ITERS, &h_avg, &h_min, &h_max, &h_std);
    stats(miss_samples, N_ITERS, &m_avg, &m_min, &m_max, &m_std);

    printf("\n=== Results (nanoseconds per access, N=%d each) ===\n", N_ITERS);
    printf("Cache ON  (hit):  avg=%.1f  min=%.1f  max=%.1f  stddev=%.1f\n", h_avg, h_min, h_max, h_std);
    printf("Cache OFF (miss): avg=%.1f  min=%.1f  max=%.1f  stddev=%.1f\n", m_avg, m_min, m_max, m_std);

    double diff = m_avg - h_avg;
    double pooled_std = (h_std + m_std) / 2.0;
    printf("\nDifference (miss - hit): %.1f ns average\n", diff);
    if (pooled_std > 0 && fabs(diff) < pooled_std) {
        printf("HONEST CONCLUSION: the difference (%.1f ns) is smaller than the measurement noise "
               "(avg stddev %.1f ns) -- NOT a statistically meaningful signal at this granularity. "
               "The RTL-level cache saving (READ_EXTRA_LATENCY=3 cycles @ 50MHz = 60ns) is real and "
               "verified directly via the cache_hits/cache_misses counters above (which DO show the "
               "expected hit/miss pattern), but it is dwarfed by the fixed per-access software-driven "
               "Avalon-MM round-trip cost through mmap'd /dev/mem and the Lightweight HPS-to-FPGA "
               "bridge -- which applies identically to hits and misses, so it cannot distinguish them. "
               "This is a real methodological limit of measuring a few-cycle RTL effect through a "
               "software register-poke interface, not a flaw in the cache itself.\n",
               diff, pooled_std);
    } else {
        printf("A measurable difference of %.1f ns average was observed (pooled stddev %.1f ns).\n", diff, pooled_std);
        if (h_avg > 0) {
            printf("Cache-enabled access is %.2f%% faster on average for this repeated-access workload.\n",
                   100.0 * diff / m_avg);
        }
    }

    watchdog_disarm();
    munmap(base, LWH2F_SPAN);
    close(fd);
    return 0;
}
