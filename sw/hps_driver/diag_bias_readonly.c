/* Read-only diagnostic for BUG-009: checks the bias table's reset value for
 * addresses that were never bias_write'd, WITHOUT ever firing an M2S request
 * (REQ_FIRE). Per cxl_avalon_shim.sv, avs_waitrequest is asserted ONLY for a
 * second write to REQ_FIRE while the one-deep pending register is still
 * occupied -- every read, and every write to any OTHER register, always
 * completes in a single bus cycle. This program only ever reads BIAS_GET, so
 * it cannot itself trigger the bus-level stall that wedged the board twice.
 */
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define LWH2F_BASE  0xFF200000u
#define LWH2F_SPAN  0x200000u
#define SHIM_OFFSET 0x6000u

#define REG_BIAS_ADDR   7
#define REG_BIAS_GET    9
#define REG_BI_STATUS   10
#define REG_STATUS      3

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    if (base == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    volatile uint32_t *shim = (volatile uint32_t *)((char *)base + SHIM_OFFSET);

    printf("Read-only BUG-009 diagnostic starting\n");
    fflush(stdout);

    uint32_t addrs[] = {0x00, 0x09, 0x40, 0x7F, 0xAA, 0xFF};
    int any_host_biased = 0;
    for (unsigned i = 0; i < sizeof(addrs)/sizeof(addrs[0]); i++) {
        shim[REG_BIAS_ADDR] = addrs[i];
        uint32_t b = shim[REG_BIAS_GET] & 1u;
        printf("addr 0x%02x: bias=%u (%s)\n", addrs[i], b, b ? "Host-Biased" : "Device-Biased");
        fflush(stdout);
        if (b) any_host_biased = 1;
    }

    uint32_t bi_status = shim[REG_BI_STATUS];
    uint32_t status = shim[REG_STATUS];
    printf("BI_STATUS=0x%08x  STATUS=0x%08x\n", bi_status, status);
    fflush(stdout);

    if (any_host_biased) {
        printf("RESULT: at least one untouched address reads Host-Biased -- reset fix did NOT take effect\n");
    } else {
        printf("RESULT: all untouched addresses read Device-Biased -- reset fix appears correct\n");
    }

    munmap(base, LWH2F_SPAN);
    close(fd);
    return 0;
}
