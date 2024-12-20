/*
 * Support for generating APEI tables and recording CPER for Guests
 *
 * Copyright (c) 2020 HUAWEI TECHNOLOGIES CO., LTD.
 *
 * Author: Dongjiu Geng <gengdongjiu@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACPI_GHES_H
#define ACPI_GHES_H

#include "hw/acpi/bios-linker-loader.h"

/*
 * Values for Hardware Error Notification Type field
 */
enum AcpiGhesNotifyType {
    /* Polled */
    ACPI_GHES_NOTIFY_POLLED = 0,
    /* External Interrupt */
    ACPI_GHES_NOTIFY_EXTERNAL = 1,
    /* Local Interrupt */
    ACPI_GHES_NOTIFY_LOCAL = 2,
    /* SCI */
    ACPI_GHES_NOTIFY_SCI = 3,
    /* NMI */
    ACPI_GHES_NOTIFY_NMI = 4,
    /* CMCI, ACPI 5.0: 18.3.2.7, Table 18-290 */
    ACPI_GHES_NOTIFY_CMCI = 5,
    /* MCE, ACPI 5.0: 18.3.2.7, Table 18-290 */
    ACPI_GHES_NOTIFY_MCE = 6,
    /* GPIO-Signal, ACPI 6.0: 18.3.2.7, Table 18-332 */
    ACPI_GHES_NOTIFY_GPIO = 7,
    /* ARMv8 SEA, ACPI 6.1: 18.3.2.9, Table 18-345 */
    ACPI_GHES_NOTIFY_SEA = 8,
    /* ARMv8 SEI, ACPI 6.1: 18.3.2.9, Table 18-345 */
    ACPI_GHES_NOTIFY_SEI = 9,
    /* External Interrupt - GSIV, ACPI 6.1: 18.3.2.9, Table 18-345 */
    ACPI_GHES_NOTIFY_GSIV = 10,
    /* Software Delegated Exception, ACPI 6.2: 18.3.2.9, Table 18-383 */
    ACPI_GHES_NOTIFY_SDEI = 11,
    /* 12 and greater are reserved */
    ACPI_GHES_NOTIFY_RESERVED = 12
};

enum {
    ACPI_HEST_SRC_ID_SEA = 0,
    ACPI_HEST_SRC_ID_GPIO = 1,
    /* future ids go here */
    ACPI_HEST_SRC_ID_RESERVED,
};

typedef struct AcpiGhesState {
    uint64_t ghes_addr_le;
    bool present; /* True if GHES is present at all on this board */
    uint64_t pci_osc_addr_le;
} AcpiGhesState;

void build_ghes_error_table(GArray *hardware_errors, BIOSLinker *linker);
void acpi_build_hest(GArray *table_data, BIOSLinker *linker,
                     const char *oem_id, const char *oem_table_id);
void acpi_ghes_add_fw_cfg(AcpiGhesState *vms, FWCfgState *s,
                          GArray *hardware_errors);
int acpi_ghes_record_errors(uint8_t notify, uint64_t error_physical_addr);

typedef struct PCIDevice PCIDevice;
bool ghes_record_aer_errors(PCIDevice *dev, uint32_t notify);
bool ghes_record_arm_errors(uint8_t type, uint32_t notify);

typedef struct CXLError CXLError;
typedef struct PCIEAERErr PCIEAERErr;
bool ghes_record_cxl_errors(PCIDevice *dev, PCIEAERErr *err,
                            CXLError *cxl_err, uint32_t notify);

typedef struct CXLEventGenMedia CXLEventGenMedia;
bool ghes_record_cxl_event_gm(PCIDevice *dev,
                           CXLEventGenMedia *gem, uint32_t notify);
/**
 * acpi_ghes_present: Report whether ACPI GHES table is present
 *
 * Returns: true if the system has an ACPI GHES table and it is
 * safe to call acpi_ghes_record_errors() to record a memory error.
 */
bool acpi_ghes_present(void);
bool acpi_fw_first_pci(void);
bool acpi_fw_first_cxl_mem(void);
#endif
