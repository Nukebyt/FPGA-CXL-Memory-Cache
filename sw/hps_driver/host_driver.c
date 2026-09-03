/* Bare-metal HPS "host" driver for Phase 1H hardware bring-up. Issues one
 * hardcoded write followed by a read-back to the same address, over
 * cxl_avalon_shim's register interface, and reports the result over UART.
 * This is deliberately the same write/fire/poll/read/ack sequence already
 * verified in sim/phase1h/tb_avalon_shim.cpp -- the point of this first
 * bring-up test is to confirm real hardware agrees with that sim result,
 * not to exercise anything new.
 */
#include <stdint.h>
#include "cxl_regs.h"
#include "uart.h"

static uint32_t poll_status_valid(void) {
    uint32_t status;
    do {
        status = CXL_REG_STATUS;
    } while (!(status & CXL_STATUS_VALID_MASK));
    return status;
}

int main(void) {
    uart_puts("\nCXL-Lite Phase 1H hardware bring-up\n");

    const uint32_t test_addr  = 0x05;
    const uint32_t test_wdata = 0xABCD1234u;

    /* Write test_wdata to test_addr, tag 3. */
    CXL_REG_REQ_ADDR  = test_addr;
    CXL_REG_REQ_WDATA = test_wdata;
    CXL_REG_REQ_FIRE  = CXL_FIRE_TAG(3) | CXL_FIRE_IS_WRITE(1);

    uint32_t status = poll_status_valid();
    uart_puts("write completion: status=");
    uart_put_hex32(status);
    uart_puts(", tag=");
    uart_put_hex32(CXL_REG_RESP_TAG);
    uart_puts((status & CXL_STATUS_IS_DATA_MASK) ? " (UNEXPECTED: is_data=1)\n" : " (NDR, as expected)\n");
    CXL_REG_RESP_ACK = 1;

    /* Read test_addr back, tag 7, expect test_wdata. */
    CXL_REG_REQ_ADDR = test_addr;
    CXL_REG_REQ_FIRE = CXL_FIRE_TAG(7) | CXL_FIRE_IS_WRITE(0);

    status = poll_status_valid();
    uint32_t rdata = CXL_REG_RESP_RDATA;
    uart_puts("read completion: status=");
    uart_put_hex32(status);
    uart_puts(", tag=");
    uart_put_hex32(CXL_REG_RESP_TAG);
    uart_puts(", rdata=");
    uart_put_hex32(rdata);
    uart_puts((rdata == test_wdata) ? " -- PASS\n" : " -- FAIL (mismatch)\n");
    CXL_REG_RESP_ACK = 1;

    uart_puts("done.\n");

    for (;;) {
        /* bare-metal: park here, nothing to return to */
    }
}
