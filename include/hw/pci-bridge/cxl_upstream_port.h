
#ifndef CXL_USP_H
#define CXL_USP_H
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_port.h"
#include "hw/cxl/cxl.h"

typedef struct CXLUpstreamPort {
    /*< private >*/
    PCIEPort parent_obj;

    /*< public >*/
    CXLComponentState cxl_cstate;
    CXLCCI swcci;
    CXLCCI mctpcci;

    MemoryRegion bar;
    CPMUState cpmu;
    MemoryRegion cpmu_registers;

    PCIExpLinkSpeed speed;
    PCIExpLinkWidth width;

    DOECap doe_cdat;
    uint64_t sn;
} CXLUpstreamPort;

#endif /* CXL_SUP_H */
