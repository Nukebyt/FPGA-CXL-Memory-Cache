/* Register map for cxl_avalon_shim, mirrored from rtl/cxl_avalon_shim.sv's
 * header comment. Physical base = Lightweight HPS-to-FPGA bridge
 * (0xFF200000, fixed by the Cyclone V HPS hard IP -- not something
 * Platform Designer reassigns) + this component's Qsys connection
 * baseAddress (0x6000, set in quartus/add_cxl_shim.tcl). */
#ifndef CXL_REGS_H
#define CXL_REGS_H

#include <stdint.h>

#define CXL_SHIM_BASE ((volatile uint32_t *)0xFF206000u)

#define CXL_REG_REQ_ADDR   (CXL_SHIM_BASE[0])
#define CXL_REG_REQ_WDATA  (CXL_SHIM_BASE[1])
#define CXL_REG_REQ_FIRE   (CXL_SHIM_BASE[2])
#define CXL_REG_STATUS     (CXL_SHIM_BASE[3])
#define CXL_REG_RESP_TAG   (CXL_SHIM_BASE[4])
#define CXL_REG_RESP_RDATA (CXL_SHIM_BASE[5])
#define CXL_REG_RESP_ACK   (CXL_SHIM_BASE[6])

#define CXL_STATUS_VALID_MASK   0x1u
#define CXL_STATUS_IS_DATA_MASK 0x2u

#define CXL_FIRE_IS_WRITE(w)   ((uint32_t)((w) & 0x1u))
#define CXL_FIRE_TAG(tag)      ((uint32_t)(((tag) & 0xFu) << 4))

#endif /* CXL_REGS_H */
