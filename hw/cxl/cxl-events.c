/*
 * CXL Event processing
 *
 * Copyright(C) 2023 Intel Corporation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_events.h"

/* Artificial limit on the number of events a log can hold */
#define CXL_TEST_EVENT_OVERFLOW 8

/*
 * TEST ONLY: misbehaving device emulation, selected from the environment.
 *
 *   CXL_TEST_STICKY_EVENT_STATUS=1  leave the Event Status bit set once the
 *                                   log has been drained
 *   CXL_TEST_EVENT_RETRY=1          answer Get/Clear Event Records with Retry
 *                                   Required for every log
 *   CXL_TEST_BAD_RECORD_COUNT=1     claim more records than the payload holds
 *   CXL_TEST_EVENT_IRQ_STORM=1      raise the event interrupt without ever
 *                                   putting a record in a log
 *   CXL_TEST_ENDLESS_RECORDS=1      acknowledge Clear Event Records without
 *                                   removing anything, so the log never drains
 */
static bool cxl_test_knob(const char *name)
{
    const char *val = getenv(name);

    return val && val[0] == '1';
}

/*
 * TEST ONLY: a device that screams on its event vector with every log
 * empty, so the host's handler has nothing to do and must disown the
 * interrupt.  Armed when the host sets the event interrupt policy and
 * started a little later, so the rest of probe is out of the way.
 *
 * It stops once the host masks the vector, which is what the kernel's
 * spurious detector does when it gives up on a screaming interrupt, and
 * gives up on its own after CXL_TEST_STORM_MAX ticks if the host never
 * does.  The 100000 interrupts note_interrupt() wants before it will
 * disable anything set the floor for that ceiling.
 */
#define CXL_TEST_STORM_START_NS  (2 * NANOSECONDS_PER_SECOND)
#define CXL_TEST_STORM_PERIOD_NS 50000
#define CXL_TEST_STORM_MAX       400000
#define CXL_TEST_STORM_QUIET     20000

static struct {
    QEMUTimer *timer;
    CXLType3Dev *ct3d;
    int vector;
    unsigned int ticks;
    unsigned int masked;
} cxl_test_storm;

static void cxl_test_event_irq_storm_tick(void *opaque)
{
    PCIDevice *pdev = &cxl_test_storm.ct3d->parent_obj;
    unsigned int vector = cxl_test_storm.vector;

    if (msix_enabled(pdev)) {
        if (msix_is_masked(pdev, vector)) {
            cxl_test_storm.masked++;
        } else {
            cxl_test_storm.masked = 0;
        }
        msix_notify(pdev, vector);
    } else if (msi_enabled(pdev)) {
        msi_notify(pdev, vector);
    }

    if (++cxl_test_storm.ticks >= CXL_TEST_STORM_MAX ||
        cxl_test_storm.masked >= CXL_TEST_STORM_QUIET) {
        return;
    }

    timer_mod_ns(cxl_test_storm.timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 CXL_TEST_STORM_PERIOD_NS);
}

void cxl_test_event_irq_storm_arm(CXLType3Dev *ct3d)
{
    CXLEventLog *log = &ct3d->cxl_dstate.event_logs[CXL_EVENT_TYPE_INFO];

    if (!cxl_test_knob("CXL_TEST_EVENT_IRQ_STORM") || cxl_test_storm.timer) {
        return;
    }

    cxl_test_storm.ct3d = ct3d;
    cxl_test_storm.vector = log->irq_vec;
    cxl_test_storm.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        cxl_test_event_irq_storm_tick, NULL);
    timer_mod_ns(cxl_test_storm.timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 CXL_TEST_STORM_START_NS);
}

static void reset_overflow(CXLEventLog *log)
{
    log->overflow_err_count = 0;
    log->first_overflow_timestamp = 0;
    log->last_overflow_timestamp = 0;
}

void cxl_event_init(CXLDeviceState *cxlds, int start_msg_num)
{
    CXLEventLog *log;
    int i;

    for (i = 0; i < CXL_EVENT_TYPE_MAX; i++) {
        log = &cxlds->event_logs[i];
        log->next_handle = 1;
        log->overflow_err_count = 0;
        log->first_overflow_timestamp = 0;
        log->last_overflow_timestamp = 0;
        log->irq_enabled = false;
        log->irq_vec = start_msg_num++;
        qemu_mutex_init(&log->lock);
        QSIMPLEQ_INIT(&log->events);
    }

    /* Override -- Dynamic Capacity uses the same vector as info */
    cxlds->event_logs[CXL_EVENT_TYPE_DYNAMIC_CAP].irq_vec =
                      cxlds->event_logs[CXL_EVENT_TYPE_INFO].irq_vec;

}

static CXLEvent *cxl_event_get_head(CXLEventLog *log)
{
    return QSIMPLEQ_FIRST(&log->events);
}

static CXLEvent *cxl_event_get_next(CXLEvent *entry)
{
    return QSIMPLEQ_NEXT(entry, node);
}

static int cxl_event_count(CXLEventLog *log)
{
    CXLEvent *event;
    int rc = 0;

    QSIMPLEQ_FOREACH(event, &log->events, node) {
        rc++;
    }

    return rc;
}

static bool cxl_event_empty(CXLEventLog *log)
{
    return QSIMPLEQ_EMPTY(&log->events);
}

static void cxl_event_delete_head(CXLDeviceState *cxlds,
                                  CXLEventLogType log_type,
                                  CXLEventLog *log)
{
    CXLEvent *entry = cxl_event_get_head(log);

    reset_overflow(log);
    QSIMPLEQ_REMOVE_HEAD(&log->events, node);
    if (cxl_event_empty(log) && !cxl_test_knob("CXL_TEST_STICKY_EVENT_STATUS")) {
        cxl_event_set_status(cxlds, log_type, false);
    }
    g_free(entry);
}

/*
 * return true if an interrupt should be generated as a result
 * of inserting this event.
 */
bool cxl_event_insert(CXLDeviceState *cxlds, CXLEventLogType log_type,
                      CXLEventRecordRaw *event)
{
    uint64_t time;
    CXLEventLog *log;
    CXLEvent *entry;

    if (log_type >= CXL_EVENT_TYPE_MAX) {
        return false;
    }

    time = cxl_device_get_timestamp(cxlds);

    log = &cxlds->event_logs[log_type];

    QEMU_LOCK_GUARD(&log->lock);

    if (cxl_event_count(log) >= CXL_TEST_EVENT_OVERFLOW) {
        if (log->overflow_err_count == 0) {
            log->first_overflow_timestamp = time;
        }
        log->overflow_err_count++;
        log->last_overflow_timestamp = time;
        return false;
    }

    entry = g_new0(CXLEvent, 1);

    memcpy(&entry->data, event, sizeof(*event));

    entry->data.hdr.handle = cpu_to_le16(log->next_handle);
    log->next_handle++;
    /* 0 handle is never valid */
    if (log->next_handle == 0) {
        log->next_handle++;
    }
    entry->data.hdr.timestamp = cpu_to_le64(time);

    QSIMPLEQ_INSERT_TAIL(&log->events, entry, node);
    cxl_event_set_status(cxlds, log_type, true);

    /* Count went from 0 to 1 */
    return cxl_event_count(log) == 1;
}

void cxl_discard_all_event_records(CXLDeviceState *cxlds)
{
    CXLEventLogType log_type;
    CXLEventLog *log;

    for (log_type = 0; log_type < CXL_EVENT_TYPE_MAX; log_type++) {
        log = &cxlds->event_logs[log_type];
        while (!cxl_event_empty(log)) {
            cxl_event_delete_head(cxlds, log_type, log);
        }
    }
}

CXLRetCode cxl_event_get_records(CXLDeviceState *cxlds, CXLGetEventPayload *pl,
                                 uint8_t log_type, int max_recs,
                                 size_t *len)
{
    CXLEventLog *log;
    CXLEvent *entry;
    uint16_t nr;

    if (log_type >= CXL_EVENT_TYPE_MAX) {
        return CXL_MBOX_INVALID_INPUT;
    }

    if (cxl_test_knob("CXL_TEST_EVENT_RETRY")) {
        return CXL_MBOX_RETRY_REQUIRED;
    }

    log = &cxlds->event_logs[log_type];

    QEMU_LOCK_GUARD(&log->lock);

    entry = cxl_event_get_head(log);
    for (nr = 0; entry && nr < max_recs; nr++) {
        memcpy(&pl->records[nr], &entry->data, CXL_EVENT_RECORD_SIZE);
        entry = cxl_event_get_next(entry);
    }

    if (!cxl_event_empty(log)) {
        pl->flags |= CXL_GET_EVENT_FLAG_MORE_RECORDS;
    }

    if (log->overflow_err_count) {
        pl->flags |= CXL_GET_EVENT_FLAG_OVERFLOW;
        pl->overflow_err_count = cpu_to_le16(log->overflow_err_count);
        pl->first_overflow_timestamp =
            cpu_to_le64(log->first_overflow_timestamp);
        pl->last_overflow_timestamp =
            cpu_to_le64(log->last_overflow_timestamp);
    }

    pl->record_count = cpu_to_le16(nr);
    *len = CXL_EVENT_PAYLOAD_HDR_SIZE + (CXL_EVENT_RECORD_SIZE * nr);

    /*
     * TEST ONLY: claim far more records than the payload just returned,
     * so the host is asked to walk off the end of its event buffer.
     */
    if (nr && cxl_test_knob("CXL_TEST_BAD_RECORD_COUNT")) {
        pl->record_count = cpu_to_le16(nr + 1);
    }

    return CXL_MBOX_SUCCESS;
}

CXLRetCode cxl_event_clear_records(CXLDeviceState *cxlds,
                                   CXLClearEventPayload *pl)
{
    CXLEventLog *log;
    uint8_t log_type;
    CXLEvent *entry;
    int nr;

    log_type = pl->event_log;

    if (log_type >= CXL_EVENT_TYPE_MAX) {
        return CXL_MBOX_INVALID_INPUT;
    }

    if (cxl_test_knob("CXL_TEST_EVENT_RETRY")) {
        return CXL_MBOX_RETRY_REQUIRED;
    }

    /*
     * TEST ONLY: acknowledge the clear but remove nothing, so every Get
     * Event Records returns the same records and the log never drains.
     */
    if (cxl_test_knob("CXL_TEST_ENDLESS_RECORDS")) {
        return CXL_MBOX_SUCCESS;
    }

    log = &cxlds->event_logs[log_type];

    QEMU_LOCK_GUARD(&log->lock);
    /*
     * Must iterate the queue twice.
     * "The device shall verify the event record handles specified in the input
     * payload are in temporal order. If the device detects an older event
     * record that will not be cleared when Clear Event Records is executed,
     * the device shall return the Invalid Handle return code and shall not
     * clear any of the specified event records."
     *   -- CXL r3.1 Section 8.2.9.2.3: Clear Event Records (0101h)
     */
    entry = cxl_event_get_head(log);
    for (nr = 0; entry && nr < pl->nr_recs; nr++) {
        uint16_t handle = pl->handle[nr];

        /* NOTE: Both handles are little endian. */
        if (handle == 0 || entry->data.hdr.handle != handle) {
            return CXL_MBOX_INVALID_INPUT;
        }
        entry = cxl_event_get_next(entry);
    }

    entry = cxl_event_get_head(log);
    for (nr = 0; entry && nr < pl->nr_recs; nr++) {
        cxl_event_delete_head(cxlds, log_type, log);
        entry = cxl_event_get_head(log);
    }

    return CXL_MBOX_SUCCESS;
}

void cxl_event_irq_assert(CXLType3Dev *ct3d)
{
    CXLDeviceState *cxlds = &ct3d->cxl_dstate;
    PCIDevice *pdev = &ct3d->parent_obj;
    int i;

    for (i = 0; i < CXL_EVENT_TYPE_MAX; i++) {
        CXLEventLog *log = &cxlds->event_logs[i];

        if (!log->irq_enabled || cxl_event_empty(log)) {
            continue;
        }

        /*  Notifies interrupt, legacy IRQ is not supported */
        if (msix_enabled(pdev)) {
            msix_notify(pdev, log->irq_vec);
        } else if (msi_enabled(pdev)) {
            msi_notify(pdev, log->irq_vec);
        }
    }
}

void cxl_create_dc_event_records_for_extents(CXLType3Dev *ct3d,
                                             CXLDCEventType type,
                                             CXLDCExtentRaw extents[],
                                             uint32_t ext_count)
{
    CXLEventDynamicCapacity event_rec = {};
    int i;

    cxl_assign_event_header(&event_rec.hdr,
                            &dynamic_capacity_uuid,
                            (1 << CXL_EVENT_TYPE_INFO),
                            sizeof(event_rec),
                            cxl_device_get_timestamp(&ct3d->cxl_dstate),
                            0, 0, 0, 0, 0, 0, 0, 0);
    event_rec.type = type;
    event_rec.validity_flags = 1;
    event_rec.host_id = 0;
    event_rec.updated_region_id = 0;
    event_rec.extents_avail = CXL_NUM_EXTENTS_SUPPORTED -
                              ct3d->dc.total_extent_count;

    for (i = 0; i < ext_count; i++) {
        memcpy(&event_rec.dynamic_capacity_extent,
               &extents[i],
               sizeof(CXLDCExtentRaw));
        event_rec.flags = 0;
        if (i < ext_count - 1) {
            /* Set "More" flag */
            event_rec.flags |= BIT(0);
        }

        if (cxl_event_insert(&ct3d->cxl_dstate,
                             CXL_EVENT_TYPE_DYNAMIC_CAP,
                             (CXLEventRecordRaw *)&event_rec)) {
            cxl_event_irq_assert(ct3d);
        }
    }
}
