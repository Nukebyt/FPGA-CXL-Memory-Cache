/* Phase 7: cross-cutting hardware stress & edge cases, on real silicon.
 * Mirrors manual Sec 4.3 ("Ordering/completion", "Error/edge conditions").
 * Watchdog-armed throughout -- see bugs.md BUG-010.
 *
 * Covers 4 of the 5 roadmap items (the 5th, cache-bypass-mode cross-check,
 * needs new RTL and is tracked separately alongside Phase 8):
 *   1. Max-outstanding-request stress (all NUM_TAGS in flight at once)
 *   2. Phantom-completion check (folded into #1 -- assert every response
 *      tag was one we actually fired, exactly once)
 *   3. Cache-thrashing with a working set LARGER than the cache itself
 *      (distinct from Phase 6H, which thrashed a single colliding index)
 *   4. BI resolution -> immediate cache allocate for the same address,
 *      exercising the Phase 4H/5H mechanisms back-to-back for the first
 *      time on real hardware
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/watchdog.h>

#define LWH2F_BASE  0xFF200000u
#define LWH2F_SPAN  0x200000u
#define SHIM_OFFSET 0x6000u

#define REG_REQ_ADDR    0
#define REG_REQ_WDATA   1
#define REG_REQ_FIRE    2
#define REG_STATUS      3
#define REG_RESP_TAG    4
#define REG_RESP_RDATA  5
#define REG_RESP_ACK    6
#define REG_BIAS_ADDR   7
#define REG_BIAS_SET    8
#define REG_BIAS_GET    9
#define REG_BI_STATUS   10
#define REG_BI_REQ_ADDR 11
#define REG_BI_REQ_ACK  12
#define REG_BI_RSP_SEND 13
#define REG_CACHE_HITS  14
#define REG_CACHE_MISS  15

#define STATUS_VALID_MASK      0x1u
#define STATUS_IS_DATA_MASK    0x2u
#define BI_STATUS_PENDING_MASK 0x1u
#define NUM_TAGS 16

#define TIMEOUT_SEC 5.0

static volatile uint32_t *shim;
static int errors = 0;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

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
static void watchdog_kick(void) {
    if (wd_fd >= 0) ioctl(wd_fd, WDIOC_KEEPALIVE, 0);
}

static void fire(int is_write, uint32_t addr, uint32_t tag, uint32_t wdata) {
    shim[REG_REQ_ADDR]  = addr;
    shim[REG_REQ_WDATA] = wdata;
    shim[REG_REQ_FIRE]  = (tag << 4) | (is_write ? 1u : 0u);
}

static uint32_t wait_valid(const char *what) {
    double t0 = now_sec();
    while (!(shim[REG_STATUS] & STATUS_VALID_MASK)) {
        if (now_sec() - t0 > TIMEOUT_SEC) {
            fprintf(stderr, "TIMEOUT after %.1fs: %s\n", TIMEOUT_SEC, what);
            fflush(stdout); fflush(stderr);
            watchdog_disarm();
            _exit(1);
        }
    }
    return shim[REG_STATUS];
}

/* --- 1+2: max outstanding requests + phantom-completion check --- */
static void test_max_outstanding(void) {
    printf("\n=== Stress 1+2: max outstanding requests (%d tags) + phantom-completion check ===\n", NUM_TAGS);

    int fired[NUM_TAGS];
    memset(fired, 0, sizeof fired);

    /* Fire all NUM_TAGS requests without draining any of them -- each is a
     * write to a distinct address, so no two collide on the same cache line
     * and no eviction stall interferes with keeping many requests in flight
     * at the engine level. */
    for (int t = 0; t < NUM_TAGS; t++) {
        uint32_t addr = 0x60 + (uint32_t)t;   /* distinct addresses, distinct cache lines */
        fire(1, addr, (uint32_t)t, 0x9000u + (uint32_t)t);
        fired[t] = 1;
    }
    printf("fired all %d tags without draining any\n", NUM_TAGS);

    /* Drain all NUM_TAGS responses. For each: (a) must be a tag we actually
     * fired [phantom-completion check], (b) must not have already been
     * seen [no duplicate/replayed response], (c) must eventually see all
     * of them [no silently dropped response]. */
    int seen[NUM_TAGS];
    memset(seen, 0, sizeof seen);
    int seen_count = 0;
    double t0 = now_sec();
    while (seen_count < NUM_TAGS) {
        uint32_t s = shim[REG_STATUS];
        if (s & STATUS_VALID_MASK) {
            uint32_t tag = shim[REG_RESP_TAG];
            if (tag >= NUM_TAGS || !fired[tag]) {
                printf("FAIL: PHANTOM COMPLETION -- response for tag %u, which was never fired\n", tag);
                errors++;
            } else if (seen[tag]) {
                printf("FAIL: DUPLICATE response for tag %u\n", tag);
                errors++;
            } else {
                seen[tag] = 1;
                seen_count++;
            }
            shim[REG_RESP_ACK] = 1;
        }
        watchdog_kick();
        if (now_sec() - t0 > TIMEOUT_SEC) {
            fprintf(stderr, "TIMEOUT after %.1fs: only %d/%d tags drained\n", TIMEOUT_SEC, seen_count, NUM_TAGS);
            fflush(stdout); fflush(stderr);
            watchdog_disarm();
            _exit(1);
        }
    }
    printf("PASS: all %d tags drained, no phantom completions, no duplicates\n", NUM_TAGS);
}

/* --- 3: cache-thrashing with a working set larger than the cache (16 lines) --- */
static void test_working_set_exceeds_cache(void) {
    printf("\n=== Stress 3: working set (24 lines) exceeds cache capacity (16 lines) ===\n");

    uint32_t h0 = shim[REG_CACHE_HITS], m0 = shim[REG_CACHE_MISS];

    #define WORKING_SET 24
    uint32_t addrs[WORKING_SET];
    uint32_t data[WORKING_SET];
    for (int i = 0; i < WORKING_SET; i++) {
        addrs[i] = 0x80 + (uint32_t)i;             /* distinct addresses -> wraps past 16 cache indices */
        data[i]  = 0xC0000000u + (uint32_t)i;
    }

    for (int i = 0; i < WORKING_SET; i++) {
        fire(1, addrs[i], 1, data[i]);
        wait_valid("thrash write");
        shim[REG_RESP_ACK] = 1;
        watchdog_kick();
    }
    printf("wrote %d distinct lines (more than the cache's 16-line capacity)\n", WORKING_SET);

    int ok = 1;
    for (int i = 0; i < WORKING_SET; i++) {
        fire(0, addrs[i], 1, 0);
        wait_valid("thrash readback");
        uint32_t got = shim[REG_RESP_RDATA];
        shim[REG_RESP_ACK] = 1;
        watchdog_kick();
        if (got != data[i]) {
            printf("FAIL: addr 0x%02x expected 0x%08x got 0x%08x\n", addrs[i], data[i], got);
            errors++;
            ok = 0;
        }
    }
    if (ok) printf("PASS: all %d lines correct after a full working-set sweep exceeding cache capacity\n", WORKING_SET);

    uint32_t h1 = shim[REG_CACHE_HITS], m1 = shim[REG_CACHE_MISS];
    printf("cache_hits +%u  cache_misses +%u over this sweep (misses expected to dominate -- working set > capacity)\n",
           h1 - h0, m1 - m0);
    #undef WORKING_SET
}

/* --- 4: BI resolution immediately followed by cache allocate, same address --- */
static void test_bi_then_cache_allocate(void) {
    printf("\n=== Stress 4: BI resolution -> immediate cache allocate for the SAME address ===\n");

    /* Must be an address with NO prior cache history in this run -- 0x80-0x97
     * was fully populated by test_working_set_exceeds_cache above. Reusing
     * one of those addresses here isn't a fresh-address test at all: it
     * would correctly HIT the pre-existing entry (valid cache behavior for
     * an address with history), not exercise a genuine first-touch miss. */
    uint32_t addr = 0xE0;
    shim[REG_BIAS_ADDR] = addr;
    shim[REG_BIAS_SET]  = 1;   /* mark Host-Biased */

    uint32_t h0 = shim[REG_CACHE_HITS], m0 = shim[REG_CACHE_MISS];
    fire(0, addr, 1, 0);   /* read -> must trigger BI (bypasses cache while Host-Biased) */

    double t0 = now_sec();
    while (!(shim[REG_BI_STATUS] & BI_STATUS_PENDING_MASK)) {
        watchdog_kick();
        if (now_sec() - t0 > TIMEOUT_SEC) {
            fprintf(stderr, "TIMEOUT waiting BI_STATUS pending\n");
            fflush(stdout); fflush(stderr);
            watchdog_disarm();
            _exit(1);
        }
    }
    shim[REG_BI_REQ_ACK]  = 1;
    shim[REG_BI_RSP_SEND] = 1;   /* bias flips to Device-Biased; stalled read now proceeds */

    wait_valid("post-BI read");
    shim[REG_RESP_ACK] = 1;

    /* The ORIGINALLY-STALLED read is itself the first cache touch for this
     * address -- once BI resolves and m2s_ready finally asserts, it proceeds
     * through the exact same accept path as any other request (per the
     * design's "let the same pending request continue unmodified" pattern),
     * including cache allocation. So this must show a MISS, not the
     * follow-up access -- an earlier version of this test checked the wrong
     * request and got a confusing false failure by expecting a SECOND touch
     * of the address to also miss, which no cache should ever do. */
    uint32_t h1 = shim[REG_CACHE_HITS], m1 = shim[REG_CACHE_MISS];
    if (m1 - m0 == 1 && h1 - h0 == 0) {
        printf("PASS: BI resolution's own stalled read correctly allocated into the cache (miss+1)\n");
    } else {
        printf("FAIL: expected miss+1 hit+0 for the post-BI read itself, got hit+%u miss+%u\n", h1-h0, m1-m0);
        errors++;
    }

    /* addr is now Device-Biased AND already cache-resident from the read
     * above -- this write must HIT (updating the line in place), proving
     * the line stayed correctly cached across the BI transition. */
    fire(1, addr, 1, 0xABCD1234u);
    wait_valid("post-BI write to now-resident line");
    shim[REG_RESP_ACK] = 1;
    uint32_t h2 = shim[REG_CACHE_HITS];
    if (h2 - h1 == 1) {
        printf("PASS: write to the just-allocated line correctly hit (in-place update)\n");
    } else {
        printf("FAIL: expected hit+1 for write to already-resident line, got hit+%u\n", h2-h1);
        errors++;
    }

    fire(0, addr, 1, 0);
    wait_valid("post-BI cache hit readback");
    uint32_t got = shim[REG_RESP_RDATA];
    shim[REG_RESP_ACK] = 1;
    uint32_t h3 = shim[REG_CACHE_HITS];
    if (got == 0xABCD1234u && h3 - h2 == 1) {
        printf("PASS: line correctly holds the new value and hits on re-read (data 0x%08x)\n", got);
    } else {
        printf("FAIL: expected cached hit with data 0xabcd1234, got data 0x%08x hit+%u\n", got, h3-h2);
        errors++;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    printf("CXL-Lite Phase 7 hardware stress & edge cases\n");
    watchdog_arm(20);

    test_max_outstanding();
    test_working_set_exceeds_cache();
    test_bi_then_cache_allocate();

    watchdog_disarm();
    munmap(base, LWH2F_SPAN);
    close(fd);

    if (errors == 0) {
        printf("\nPHASE7 PASS: max-outstanding/phantom-completion, working-set-exceeds-cache, "
               "and BI-then-cache-allocate all verified on real hardware\n");
        return 0;
    }
    printf("\nPHASE7 FAILURES: %d\n", errors);
    return 1;
}
