#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define LWH2F_BASE 0xFF200000u
#define LWH2F_SPAN 0x200000u
#define SHIM_OFFSET 0x6000u
#define REG_REQ_ADDR 0
#define REG_REQ_WDATA 1
#define REG_REQ_FIRE 2
#define REG_STATUS 3
#define REG_RESP_TAG 4
#define REG_RESP_RDATA 5
#define REG_RESP_ACK 6
#define STATUS_VALID_MASK 0x1u
#define STATUS_IS_DATA_MASK 0x2u

static uint32_t wait_status(volatile uint32_t *shim) {
    uint32_t s; int g=0;
    do { s = shim[REG_STATUS]; if(++g>1000000){fprintf(stderr,"TIMEOUT\n");_exit(1);} } while(!(s&STATUS_VALID_MASK));
    return s;
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR|O_SYNC);
    void *base = mmap(NULL, LWH2F_SPAN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, LWH2F_BASE);
    volatile uint32_t *shim = (volatile uint32_t*)((char*)base + SHIM_OFFSET);
    int errors = 0;

    struct { uint32_t addr, tag, data; } writes[] = {
        {0x11, 1, 0xDEADBEEFu}, {0x22, 2, 0x00000000u}, {0x7F, 9, 0x11223344u}
    };
    for (int i = 0; i < 3; i++) {
        shim[REG_REQ_ADDR] = writes[i].addr;
        shim[REG_REQ_WDATA] = writes[i].data;
        shim[REG_REQ_FIRE] = (writes[i].tag << 4) | 0x1u;
        uint32_t s = wait_status(shim);
        uint32_t tag = shim[REG_RESP_TAG];
        printf("write addr=0x%02x data=0x%08x tag=%u -> status=0x%x resp_tag=%u %s\n",
               writes[i].addr, writes[i].data, writes[i].tag, s, tag, (tag==writes[i].tag)?"OK":"TAG_MISMATCH");
        if (tag != writes[i].tag) errors++;
        shim[REG_RESP_ACK] = 1;
    }

    struct { uint32_t addr, tag, exp; } reads[] = {
        {0x11, 11, 0xDEADBEEFu}, {0x22, 12, 0x00000000u}, {0x7F, 13, 0x11223344u}, {0x33, 14, 0x00000000u}
    };
    for (int i = 0; i < 4; i++) {
        shim[REG_REQ_ADDR] = reads[i].addr;
        shim[REG_REQ_FIRE] = (reads[i].tag << 4) | 0x0u;
        uint32_t s = wait_status(shim);
        uint32_t tag = shim[REG_RESP_TAG];
        uint32_t rdata = shim[REG_RESP_RDATA];
        int ok = (tag==reads[i].tag) && (rdata==reads[i].exp) && (s & STATUS_IS_DATA_MASK);
        printf("read  addr=0x%02x        tag=%u -> status=0x%x resp_tag=%u rdata=0x%08x exp=0x%08x %s\n",
               reads[i].addr, reads[i].tag, s, tag, rdata, reads[i].exp, ok?"OK":"MISMATCH");
        if (!ok) errors++;
        shim[REG_RESP_ACK] = 1;
    }

    munmap(base, LWH2F_SPAN); close(fd);
    printf(errors==0 ? "ALL_OK (%d checks)\n" : "FAILURES: %d\n", errors==0?7:errors);
    return errors;
}
