/* Watchdog-protected single-register probe.
 *
 * Context (BUG-010): accessing the Lightweight HPS-to-FPGA bridge while the
 * FPGA fabric is unconfigured stalls the ARM core in an uninterruptible bus
 * transaction -- SIGKILL and `timeout` cannot recover it, and the whole SoC
 * wedges (networking included), requiring a physical power cycle. That cost
 * several power cycles during bring-up before the cause was understood.
 *
 * So: arm the hardware watchdog BEFORE touching MMIO. If the read stalls the
 * core, the watchdog fires and the board resets itself instead of needing
 * hands-on recovery. If the read succeeds, the watchdog is disarmed cleanly
 * via the magic-close ('V') protocol and nothing is disturbed.
 *
 * Prints (line-buffered, flushed) before the risky access, so the last line
 * that reaches the terminal identifies exactly how far execution got.
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

#define LWH2F_BASE  0xFF200000u
#define LWH2F_SPAN  0x200000u
#define SHIM_OFFSET 0x6000u

#define REG_STATUS  3

#define WD_TIMEOUT_SEC 20

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    int wd = open("/dev/watchdog", O_WRONLY);
    if (wd < 0) {
        perror("open /dev/watchdog");
        printf("REFUSING to probe MMIO without a watchdog safety net\n");
        return 1;
    }
    int timeout = WD_TIMEOUT_SEC;
    if (ioctl(wd, WDIOC_SETTIMEOUT, &timeout) != 0) {
        perror("WDIOC_SETTIMEOUT (continuing with driver default)");
    }
    int actual = 0;
    if (ioctl(wd, WDIOC_GETTIMEOUT, &actual) == 0) {
        printf("watchdog armed, timeout = %d s\n", actual);
    } else {
        printf("watchdog armed (timeout unknown)\n");
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    volatile uint32_t *shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    printf("mmap OK; about to read STATUS at 0x%08X ...\n",
           LWH2F_BASE + SHIM_OFFSET + REG_STATUS * 4);

    uint32_t v = shim[REG_STATUS];   /* <-- the access that wedges an unconfigured fabric */

    printf("READ OK: STATUS = 0x%08x\n", v);
    printf("RESULT: fabric is responding on the lightweight bridge\n");

    /* Magic close: disarm rather than leave the watchdog running. */
    write(wd, "V", 1);
    close(wd);
    munmap(base, LWH2F_SPAN);
    close(fd);
    printf("watchdog disarmed cleanly\n");
    return 0;
}
