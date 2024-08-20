/*
 * ARM  Processor errors QMP stubs
 *
 * Copyright(C) 2024 Huawei LTD.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-v2-only
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-arm-error-inject.h"


typedef struct ArmProcessorErrorTypeList ArmProcessorErrorTypeList;

void qmp_arm_inject_error(ArmProcessorErrorTypeList *type, Error **errp)
{
    error_setg(errp, "ARM processor error support is not compiled in");
}
