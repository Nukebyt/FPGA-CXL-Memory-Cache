/* Linux userspace version of the Phase 1H bring-up driver, for running under
 * the board's already-booted Angstrom Linux over the HPS's own gcc, rather
 * than the bare-metal path in host_driver.c (which assumes exclusive control
 * of the CPU and would corrupt a running kernel -- not usable here).
 *
 * Same write/fire/poll/read/poll/ack sequence already verified in
 * sim/phase1h/tb_avalon_shim.cpp and cxl_regs.h's register map -- the point
 * of this test is to confirm real hardware agrees with that sim result.
 *
 * mmap(/dev/mem) at the Lightweight HPS-to-FPGA bridge base (0xFF200000,
 * span 0x200000 -- fixed by the Cyclone V HPS hard IP), then index our
 * peripheral at its Qsys-assigned offset (0x6000, confirmed against
 * soc_system.sopcinfo's <baseAddress> for cxl_avalon_shim_0.avs, not just
 * assumed from memory). Blocking MMIO stores/loads respect avs_waitrequest
 * in hardware -- the ARM store instruction doesn't retire until the
 * peripheral is ready, so no software backpressure handling is needed
 * beyond the response-side STATUS poll below.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define LWH2F_BASE   0xFF200000u
#define LWH2F_SPAN   0x200000u
#define SHIM_OFFSET  0x6000u

#define REG_REQ_ADDR   0
#define REG_REQ_WDATA  1
#define REG_REQ_FIRE   2
#define REG_STATUS     3
#define REG_RESP_TAG   4
#define REG_RESP_RDATA 5
#define REG_RESP_ACK   6

#define STATUS_VALID_MASK   0x1u
#define STATUS_IS_DATA_MASK 0x2u

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    volatile uint32_t *shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    printf("CXL-Lite Phase 1H hardware bring-up (Linux userspace)\n");
    printf("mmap ok: LwH2F base=0x%08x, shim offset=0x%04x -> phys 0x%08x\n",
           LWH2F_BASE, SHIM_OFFSET, LWH2F_BASE + SHIM_OFFSET);

    const uint32_t test_addr  = 0x05;
    const uint32_t test_wdata = 0xABCD1234u;
    int errors = 0;

    /* Write test_wdata to test_addr, tag 3. */
    shim[REG_REQ_ADDR]  = test_addr;
    shim[REG_REQ_WDATA] = test_wdata;
    shim[REG_REQ_FIRE]  = (3u << 4) | 0x1u; /* tag=3, is_write=1 */

    uint32_t status;
    int guard = 0;
    do {
        status = shim[REG_STATUS];
        if (++guard > 1000000) { fprintf(stderr, "TIMEOUT waiting for write completion\n"); return 1; }
    } while (!(status & STATUS_VALID_MASK));

    uint32_t tag = shim[REG_RESP_TAG];
    printf("write completion: status=0x%x, tag=%u%s\n", status, tag,
           (status & STATUS_IS_DATA_MASK) ? " (UNEXPECTED: is_data=1)" : " (NDR, as expected)");
    if (status & STATUS_IS_DATA_MASK) errors++;
    if (tag != 3) { printf("FAIL: write tag mismatch, expected 3 got %u\n", tag); errors++; }
    shim[REG_RESP_ACK] = 1;

    /* Read test_addr back, tag 7, expect test_wdata. */
    shim[REG_REQ_ADDR] = test_addr;
    shim[REG_REQ_FIRE] = (7u << 4) | 0x0u; /* tag=7, is_write=0 */

    guard = 0;
    do {
        status = shim[REG_STATUS];
        if (++guard > 1000000) { fprintf(stderr, "TIMEOUT waiting for read completion\n"); return 1; }
    } while (!(status & STATUS_VALID_MASK));

    tag = shim[REG_RESP_TAG];
    uint32_t rdata = shim[REG_RESP_RDATA];
    printf("read completion: status=0x%x, tag=%u, rdata=0x%08x%s\n", status, tag, rdata,
           (status & STATUS_IS_DATA_MASK) ? " (DRS, as expected)" : " (UNEXPECTED: is_data=0)");
    if (!(status & STATUS_IS_DATA_MASK)) errors++;
    if (tag != 7) { printf("FAIL: read tag mismatch, expected 7 got %u\n", tag); errors++; }
    if (rdata != test_wdata) {
        printf("FAIL: read data mismatch, expected 0x%08x got 0x%08x\n", test_wdata, rdata);
        errors++;
    }
    shim[REG_RESP_ACK] = 1;

    munmap(base, LWH2F_SPAN);
    close(fd);

    if (errors == 0) {
        printf("PHASE1H_HARDWARE PASS: write+read round trip verified on real silicon\n");
        return 0;
    }
    printf("PHASE1H_HARDWARE FAIL: %d errors\n", errors);
    return 1;
}
