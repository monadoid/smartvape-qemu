/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host test control, NOT an ESP32 peripheral. No guest registers or interrupts.
 * Stops execution at a QEMU virtual-clock deadline so input scenarios need not
 * use host sleep durations as simulated time. icount is still not silicon time.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "sysemu/runstate.h"

#define TYPE_SMARTVAPE_TEST_CLOCK "smartvape-test-clock"
OBJECT_DECLARE_SIMPLE_TYPE(SmartVapeTestClock, SMARTVAPE_TEST_CLOCK)

struct SmartVapeTestClock {
    Object parent;
    QEMUTimer *timer;
    int64_t deadline;
    uint64_t stops;
};

static void stop_at_deadline(void *opaque)
{
    SmartVapeTestClock *s = opaque;
    s->stops++;
    vm_stop(RUN_STATE_PAUSED);
}

static void get_now(Object *obj, Visitor *v, const char *name,
                    void *opaque, Error **errp)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    visit_type_int64(v, name, &now, errp);
}

static void get_deadline(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    SmartVapeTestClock *s = SMARTVAPE_TEST_CLOCK(obj);
    visit_type_int64(v, name, &s->deadline, errp);
}

static void set_deadline(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    SmartVapeTestClock *s = SMARTVAPE_TEST_CLOCK(obj);
    int64_t deadline;
    if (!visit_type_int64(v, name, &deadline, errp)) {
        return;
    }
    if (runstate_is_running()) {
        error_setg(errp, "Stop the VM before scheduling an input boundary");
        return;
    }
    if (deadline <= qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        error_setg(errp, "Deadline must be in the virtual future");
        return;
    }
    s->deadline = deadline;
    timer_mod(s->timer, deadline);
}

static void clock_init(Object *obj)
{
    SmartVapeTestClock *s = SMARTVAPE_TEST_CLOCK(obj);
    s->deadline = -1;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, stop_at_deadline, s);
    object_property_add(obj, "now-ns", "int64", get_now, NULL, NULL, NULL);
    object_property_add(obj, "stop-at-ns", "int64", get_deadline,
                        set_deadline, NULL, NULL);
    object_property_add_uint64_ptr(obj, "stops", &s->stops, OBJ_PROP_FLAG_READ);
}

static void clock_finalize(Object *obj)
{
    timer_free(SMARTVAPE_TEST_CLOCK(obj)->timer);
}

static const TypeInfo clock_info = {
    .name = TYPE_SMARTVAPE_TEST_CLOCK,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(SmartVapeTestClock),
    .instance_init = clock_init,
    .instance_finalize = clock_finalize,
    .interfaces = (InterfaceInfo[]) { { TYPE_USER_CREATABLE }, { } },
};

static void register_clock(void) { type_register_static(&clock_info); }
type_init(register_clock)
