/* Consolidated Phase 2H + 3H + 4H hardware bring-up test. Mirrors the
 * exact stimulus patterns already verified in sim/phase2, sim/phase3h,
 * and sim/phase4h -- the point of this test is confirming real hardware
 * agrees with those sim results, not exercising anything new.
 *
 * Register map: see rtl/cxl_avalon_shim.sv's header comment. Physical
 * base 0xFF206000 (Lightweight HPS-to-FPGA bridge + this component's
 * Qsys baseAddress), confirmed against soc_system.sopcinfo earlier in
 * this project, not just assumed.
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

/* BUG-010 safety net: a stalled Avalon transaction (e.g. a second REQ_FIRE
 * write blocking forever because m2s_ready never comes) hangs the ARM core
 * uninterruptibly -- no signal, no `timeout`, only a power cycle. Arming the
 * hardware watchdog first turns that into an automatic self-reset instead. */
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
    write(wd_fd, "V", 1);   /* magic close: disarm rather than leave it running */
    close(wd_fd);
    wd_fd = -1;
    printf("watchdog disarmed cleanly\n");
}

/* Loop guard: iteration-count guards elsewhere in this file can take a long
 * time to trip and give no wall-clock signal while spinning. This one prints
 * progress and aborts on real elapsed time, so a hang is visible and bounded
 * regardless of how fast/slow the polled condition would eventually resolve
 * (or never does). See BUG-009: a prior run of this program produced ZERO
 * output before the whole board went unreachable, and it turned out stdout
 * was fully buffered over the non-tty SSH pipe -- so a hang anywhere between
 * program start and exit was indistinguishable from "hasn't printed yet". */
#include <time.h>
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#define TIMEOUT_SEC 5.0
#define WAIT_UNTIL(cond, label) do { \
        double _t0 = now_sec(); \
        while (!(cond)) { \
            if (now_sec() - _t0 > TIMEOUT_SEC) { \
                fprintf(stderr, "TIMEOUT after %.1fs waiting: %s\n", TIMEOUT_SEC, label); \
                fflush(stdout); fflush(stderr); \
                _exit(1); \
            } \
        } \
    } while (0)

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

#define STATUS_VALID_MASK   0x1u
#define STATUS_IS_DATA_MASK 0x2u
#define BI_STATUS_PENDING_MASK 0x1u

static volatile uint32_t *shim;
static int errors = 0;

static void fire(uint32_t addr, uint32_t tag, int is_write, uint32_t wdata) {
    shim[REG_REQ_ADDR] = addr;
    shim[REG_REQ_WDATA] = wdata;
    shim[REG_REQ_FIRE] = (tag << 4) | (is_write ? 1u : 0u);
}

static uint32_t poll_status_valid(void) {
    WAIT_UNTIL(shim[REG_STATUS] & STATUS_VALID_MASK, "STATUS valid");
    return shim[REG_STATUS];
}

static void bias_write(uint32_t addr, int host_biased) {
    shim[REG_BIAS_ADDR] = addr;
    shim[REG_BIAS_SET] = host_biased ? 1u : 0u;
}

static uint32_t bias_read(uint32_t addr) {
    shim[REG_BIAS_ADDR] = addr;
    return shim[REG_BIAS_GET] & 1u;
}

/* --- Phase 2H: concurrent out-of-order completion, mirrors sim/phase2 --- */
static void test_phase2h(void) {
    printf("\n=== Phase 2H: multi-tag routing (see note on ordering) ===\n");
    fire(0x09, 10, 0, 0);          /* read, tag 10, never written -> expect 0 */

    /* Is request 1 already finished before we can even issue request 2? If so,
     * the two are never concurrently in flight, and relative completion order
     * says nothing about the engine's out-of-order capability -- it just
     * mirrors issue order. Measured rather than assumed, because sim/phase2
     * DOES show the write retiring first, and the difference between the two
     * environments is the point worth recording. */
    int already_done = (shim[REG_STATUS] & STATUS_VALID_MASK) ? 1 : 0;
    printf("request 1 complete before request 2 issued? %s\n",
           already_done ? "YES -- requests cannot overlap via this interface"
                        : "no -- requests genuinely overlap");

    fire(0x40, 12, 1, 0xCAFEBABEu); /* write, tag 12, issued 2nd, numerically higher tag */

    int seen10 = 0, seen12 = 0, pos10 = -1, pos12 = -1, pos = 0;
    double t0 = now_sec();
    while (!(seen10 && seen12)) {
        uint32_t s = shim[REG_STATUS];
        if (s & STATUS_VALID_MASK) {
            uint32_t tag = shim[REG_RESP_TAG];
            if (tag == 10 && !seen10) { seen10 = 1; pos10 = pos++; }
            if (tag == 12 && !seen12) { seen12 = 1; pos12 = pos++; }
            shim[REG_RESP_ACK] = 1;
        }
        if (now_sec() - t0 > TIMEOUT_SEC) {
            fprintf(stderr, "TIMEOUT after %.1fs waiting for tags 10 and 12 (seen10=%d seen12=%d)\n",
                    TIMEOUT_SEC, seen10, seen12);
            fflush(stdout); fflush(stderr);
            _exit(1);
        }
    }
    /* What IS verifiable here: both tags come back, correctly routed and
     * distinguished. What is NOT verifiable here: relative completion order.
     * Demonstrating out-of-order completion requires both requests to be in
     * flight at once, which needs cycle-level issue control -- sim/phase2 has
     * that and confirms the write (tag 12) retires before the read (tag 10)
     * thanks to READ_EXTRA_LATENCY. Through a software-paced register
     * interface each MMIO write costs far more cycles than the engine's whole
     * read latency, so request 1 always retires before request 2 is issued and
     * completion order can only ever mirror issue order. Asserting sim's
     * ordering here was a bad test, not a bug in the engine. */
    if (seen10 && seen12) {
        printf("PASS: both tags routed and completed correctly on real hardware "
               "(read tag 10 at pos %d, write tag 12 at pos %d)\n", pos10, pos12);
        printf("NOTE: completion order here mirrors issue order because the requests "
               "cannot overlap via a software-paced interface; out-of-order completion "
               "is verified cycle-accurately in sim/phase2 instead.\n");
    } else {
        printf("FAIL: missing response(s) -- seen tag10=%d tag12=%d\n", seen10, seen12);
        errors++;
    }
}

/* --- Phase 3H: bias register round trip, mirrors sim/phase3h --- */
static void test_phase3h(void) {
    printf("\n=== Phase 3H: bias register interface ===\n");
    bias_write(0x03, 1);
    bias_write(0x50, 0);
    uint32_t b1 = bias_read(0x03);
    uint32_t b2 = bias_read(0x50);
    if (b1 == 1 && b2 == 0) {
        printf("PASS: bias set/get round trip correct on real hardware (0x03=Host-Biased, 0x50=Device-Biased)\n");
    } else {
        printf("FAIL: bias round trip wrong, got 0x03=%u 0x50=%u\n", b1, b2);
        errors++;
    }
}

/* --- Phase 4H: BI handshake, this program IS the host-side BI responder,
 * mirrors sim/phase4h --- */
static void test_phase4h(void) {
    printf("\n=== Phase 4H: BI handshake (this program is the host responder) ===\n");
    bias_write(0x25, 1); /* mark Host-Biased */

    fire(0x25, 9, 0, 0); /* read -> must trigger a BI request */
    fflush(stdout); /* flush BEFORE the risky wait -- if this hangs, we still know phases 2H/3H and
                      * the fire() above ran, instead of guessing blind like the prior run did */

    WAIT_UNTIL(shim[REG_BI_STATUS] & BI_STATUS_PENDING_MASK, "BI_STATUS pending");
    uint32_t bi_addr = shim[REG_BI_REQ_ADDR];
    if (bi_addr != 0x25) {
        printf("FAIL: BI_REQ_ADDR expected 0x25, got 0x%02x\n", bi_addr);
        errors++;
    } else {
        printf("BI request correctly surfaced on real hardware: addr=0x%02x\n", bi_addr);
    }

    shim[REG_BI_REQ_ACK] = 1;   /* acknowledge */
    shim[REG_BI_RSP_SEND] = 1;  /* "invalidation done" */

    if (bias_read(0x25) != 0) {
        printf("FAIL: 0x25 still Host-Biased after BI_RSP_SEND\n");
        errors++;
    } else {
        printf("PASS: bias correctly flipped to Device-Biased after BI_RSP_SEND on real hardware\n");
    }

    uint32_t status = poll_status_valid();
    uint32_t tag = shim[REG_RESP_TAG];
    if (tag != 9 || !(status & STATUS_IS_DATA_MASK)) {
        printf("FAIL: post-BI response wrong: tag=%u status=0x%x\n", tag, status);
        errors++;
    } else {
        printf("PASS: post-BI read completed correctly on real hardware (tag=%u)\n", tag);
    }
    shim[REG_RESP_ACK] = 1;
}

int main(int argc, char **argv) {
    /* Optional argv[1]: "2h3h" runs only Phase 2H+3H (skips the untested-on-hardware
     * BI handshake path); default (no arg) runs all three, same as before. Added after
     * a prior run wedged the whole board with zero output -- see BUG-009 -- so BI logic
     * can now be isolated instead of always bundled with the phases already known-safe. */
    int run_4h = 1;
    if (argc > 1 && strcmp(argv[1], "2h3h") == 0) run_4h = 0;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffer even over a non-tty SSH pipe, so progress is
                                        * visible incrementally instead of only on exit/buffer-full */

    printf("CXL-Lite consolidated Phase 2H/3H/4H hardware bring-up\n");
    watchdog_arm(20);
    fflush(stdout);

    test_phase2h();
    fflush(stdout);
    test_phase3h();
    fflush(stdout);
    if (run_4h) {
        test_phase4h();
        fflush(stdout);
    } else {
        printf("\n=== Phase 4H skipped (ran with \"2h3h\" arg) ===\n");
        fflush(stdout);
    }

    watchdog_disarm();
    munmap(base, LWH2F_SPAN);
    close(fd);

    if (errors == 0) {
        printf("\nALL_PHASES_PASS: Phase 2H, 3H%s verified on real hardware\n",
               run_4h ? ", 4H all" : " (4H skipped)");
        return 0;
    }
    printf("\nFAILURES: %d\n", errors);
    return 1;
}
