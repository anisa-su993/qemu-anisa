/*
 * CXL Type 3 (memory expander) device
 *
 * Copyright(C) 2020 Intel Corporation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-v2-only
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/qapi-commands-arm-error-inject.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/pmem.h"
#include "qemu/range.h"
#include "qemu/rcu.h"
#include "qemu/guest-random.h"
#include "sysemu/hostmem.h"
#include "sysemu/numa.h"
#include "hw/boards.h"
#include "hw/acpi/ghes.h"

#define DWORD_BYTE 4

/* For ARM processor errors */
void qmp_arm_inject_error(ArmProcessorErrorTypeList *errortypes, Error **errp)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    uint8_t error_types = 0;

    while (errortypes) {
        error_types |= BIT(errortypes->value);
        errortypes = errortypes->next;
    }

    ghes_record_arm_errors(error_types, ACPI_GHES_NOTIFY_GPIO);
    if (mc->set_error) {
        mc->set_error();
    }

    return;
}
