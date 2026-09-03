/* Phase 5H + 6H hardware test: direct-mapped write-back cache on real silicon.
 *
 * Mirrors the stimulus already verified in sim/phase5 and sim/phase6, but with
 * one advantage the sim testbenches don't have: the CACHE_HITS / CACHE_MISSES
 * counters (shim registers 14/15) let hit-vs-miss be asserted DIRECTLY rather
 * than inferred from data values. A cache that returned correct data while
 * silently missing every time would pass a data-only test and fail these.
 *
 * Cache geometry (must match cxl_mem_protocol_engine.sv): CACHE_IDX_W = 4, so
 * the line index is addr[3:0] and addresses sharing the low nibble collide.
 * 0x05/0x15 collide on index 5; 0x07/0x17/0x27/0x37 all collide on index 7.
 *
 * Watchdog-armed throughout -- see bugs.md BUG-010. A cache bug that stalls
 * m2s_ready forever would otherwise wedge the board and cost a power cycle.
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
#define REG_CACHE_HITS  14
#define REG_CACHE_MISS  15

#define STATUS_VALID_MASK   0x1u
#define STATUS_IS_DATA_MASK 0x2u

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
    printf("watchdog disarmed cleanly\n");
}

static uint32_t hits(void)   { return shim[REG_CACHE_HITS]; }
static uint32_t misses(void) { return shim[REG_CACHE_MISS]; }

/* Issue one request and wait for its response. Sequential by construction --
 * one outstanding request at a time -- so a single fixed tag is safe and
 * avoids any risk of exceeding TAG_W's 0..15 range. */
static uint32_t do_req(int is_write, uint32_t addr, uint32_t wdata, const char *what) {
    shim[REG_REQ_ADDR]  = addr;
    shim[REG_REQ_WDATA] = wdata;
    shim[REG_REQ_FIRE]  = (1u << 4) | (is_write ? 1u : 0u);   /* tag 1 */

    double t0 = now_sec();
    while (!(shim[REG_STATUS] & STATUS_VALID_MASK)) {
        if (now_sec() - t0 > TIMEOUT_SEC) {
            fprintf(stderr, "TIMEOUT after %.1fs waiting for response: %s\n", TIMEOUT_SEC, what);
            fflush(stdout); fflush(stderr);
            watchdog_disarm();
            _exit(1);
        }
    }
    uint32_t rdata = shim[REG_RESP_RDATA];
    shim[REG_RESP_ACK] = 1;
    return rdata;
}

static void expect_counters(const char *label, uint32_t h0, uint32_t m0,
                            int want_hit_delta, int want_miss_delta) {
    int dh = (int)(hits() - h0);
    int dm = (int)(misses() - m0);
    if (dh == want_hit_delta && dm == want_miss_delta) {
        printf("PASS: %s -- hits+%d misses+%d as expected\n", label, dh, dm);
    } else {
        printf("FAIL: %s -- expected hits+%d misses+%d, got hits+%d misses+%d\n",
               label, want_hit_delta, want_miss_delta, dh, dm);
        errors++;
    }
}

static void expect_data(const char *label, uint32_t got, uint32_t want) {
    if (got == want) {
        printf("PASS: %s -- read back 0x%08x\n", label, got);
    } else {
        printf("FAIL: %s -- expected 0x%08x, got 0x%08x\n", label, want, got);
        errors++;
    }
}

/* --- Phase 5H: hit / miss / allocate / evict-writeback, mirrors sim/phase5 --- */
static void test_phase5h(void) {
    printf("\n=== Phase 5H: cache hit/miss/allocate/evict ===\n");
    uint32_t h, m;

    h = hits(); m = misses();
    do_req(1, 0x05, 0xAAAA0001u, "write A (0x05)");
    expect_counters("write A (first touch of index 5)", h, m, 0, 1);

    h = hits(); m = misses();
    uint32_t a = do_req(0, 0x05, 0, "read A back");
    expect_counters("read A back (resident line)", h, m, 1, 0);
    expect_data("read A back", a, 0xAAAA0001u);

    /* 0x15 shares index 5 with 0x05 -> forces eviction of A, which is dirty,
     * so A must be written back to backing memory before B can allocate. */
    h = hits(); m = misses();
    do_req(1, 0x15, 0xBBBB0002u, "write B (0x15, evicts A)");
    expect_counters("write B (collides with A on index 5)", h, m, 0, 1);

    /* If the eviction writeback dropped A, this returns stale/zero data. */
    h = hits(); m = misses();
    uint32_t a2 = do_req(0, 0x05, 0, "read A after eviction");
    expect_counters("read A after eviction (B now resident)", h, m, 0, 1);
    expect_data("read A after eviction -- survived writeback", a2, 0xAAAA0001u);

    /* Reading A back in turn evicted B; B was dirty, so it must have survived too. */
    uint32_t b = do_req(0, 0x15, 0, "read B after its own eviction");
    expect_data("read B after its own eviction -- survived writeback", b, 0xBBBB0002u);
}

/* --- Phase 6H: multi-round eviction thrash + index isolation, mirrors sim/phase6 --- */
static void test_phase6h(void) {
    printf("\n=== Phase 6H: multi-round eviction thrash ===\n");

    /* All four collide on index 7 -- every write evicts the previous one. */
    struct { uint32_t addr, data; } t[] = {
        {0x07, 0x11111111u}, {0x17, 0x22222222u},
        {0x27, 0x33333333u}, {0x37, 0x44444444u},
    };
    for (unsigned i = 0; i < 4; i++) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, "thrash write 0x%02x", t[i].addr);
        do_req(1, t[i].addr, t[i].data, lbl);
    }
    printf("4 colliding writes issued (each evicting the previous)\n");

    /* Read back in reverse order; every one is a miss served from backing
     * memory, and all four must still hold their original data. */
    for (int i = 3; i >= 0; i--) {
        char lbl[64];
        snprintf(lbl, sizeof lbl, "thrash readback 0x%02x", t[i].addr);
        uint32_t v = do_req(0, t[i].addr, 0, lbl);
        expect_data(lbl, v, t[i].data);
    }

    /* Index isolation: touching a different index must not disturb index 7. */
    do_req(1, 0x08, 0xDEADBEEFu, "write to unrelated index 8");
    uint32_t still = do_req(0, t[3].addr, 0, "re-read 0x37 after unrelated write");
    expect_data("index isolation -- 0x37 undisturbed by index-8 write", still, t[3].data);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    printf("CXL-Lite Phase 5H/6H cache hardware test\n");
    watchdog_arm(20);
    printf("counters at entry: hits=%u misses=%u\n", hits(), misses());

    test_phase5h();
    test_phase6h();

    printf("\ncounters at exit: hits=%u misses=%u\n", hits(), misses());
    watchdog_disarm();
    munmap(base, LWH2F_SPAN);
    close(fd);

    if (errors == 0) {
        printf("\nPHASE56H PASS: cache hit/miss/allocate/evict and multi-round thrash "
               "all verified on real hardware\n");
        return 0;
    }
    printf("\nPHASE56H FAILURES: %d\n", errors);
    return 1;
}
