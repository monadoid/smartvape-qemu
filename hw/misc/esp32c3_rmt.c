/* ESP32-C3 TRM v1.4 chapter 33; PAC esp32c3 0.32.2.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Two non-carrier TX channels, integer APB/XTAL clocks, direct RAM and wrap.
 * Unsupported modes are diagnosed and never produce a successful completion.
 * No RX, fractional/RC_FAST clock, migration or system clock/reset coupling yet.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/misc/esp32c3_rmt.h"

static void irq_update(ESP32C3RmtState *s)
{
    qemu_set_irq(s->irq, !!(s->raw & s->ena));
}

static void output(C3RmtChannel *c, bool level, bool enable)
{
    qemu_set_irq(c->parent->level[c->index], level);
    qemu_set_irq(c->parent->enable[c->index], enable);
}

static void stop(C3RmtChannel *c, bool marker)
{
    c->running = false;
    timer_del(c->timer);
    output(c, (c->active & BIT(6)) ? !!(c->active & BIT(5)) : marker, true);
}

static void unsupported(C3RmtChannel *c, const char *reason)
{
    qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED RMT channel=%u %s\n", c->index, reason);
    c->running = false;
    timer_del(c->timer);
    output(c, false, false);
}

static void advance(void *opaque)
{
    C3RmtChannel *c = opaque;
    ESP32C3RmtState *s = c->parent;
    if (!c->running) {
        return;
    }
    unsigned blocks = (c->active >> 16) & 7;
    unsigned words = blocks * 48;
    if (c->position == words * 2) {
        if (c->active & BIT(4)) {
            c->position = 0;
        } else {
            stop(c, false);
            s->raw |= BIT(4 + c->index);
            irq_update(s);
            return;
        }
    }
    if (!(c->position & 1)) {
        c->word = s->ram[c->index * 48 + c->position / 2];
    }
    uint16_t pulse = c->word >> ((c->position & 1) * 16);
    unsigned ticks = pulse & 0x7fff;
    bool level = pulse >> 15;
    if (!ticks) {
        stop(c, level);
        s->raw |= BIT(c->index);
        irq_update(s);
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_RMT end time_ns=%" PRId64 " channel=%u\n",
                      c->started_ns + (c->elapsed_twons + 1) / 2, c->index);
        return;
    }
    unsigned source = (s->sys_conf >> 24) & 3;
    unsigned divider = (c->active >> 8) & 255;
    divider = divider ? divider : 256;
    /* Half-nanoseconds retain 80 MHz precision without accumulating rounding. */
    uint64_t duration = (uint64_t)ticks * divider *
                       (((s->sys_conf >> 4) & 255) + 1) * (source == 1 ? 25 : 50);
    if (duration <= 125 + (source == 1 ? 150 : 300) *
                             (((s->sys_conf >> 4) & 255) + 1)) {
        unsupported(c, "pulse violates TRM equation 33.1");
        return;
    }
    output(c, level, true);
    qemu_log_mask(LOG_UNIMP,
        "SMARTVAPE_RMT pulse time_ns=%" PRId64 " channel=%u level=%u duration_twons=%" PRIu64 "\n",
        c->started_ns + (c->elapsed_twons + 1) / 2, c->index, level, duration);
    c->elapsed_twons += duration;
    c->position++;
    if (!(c->position & 1)) {
        unsigned limit = c->active_limit & 0x1ff;
        if (limit && ++c->sent == limit) {
            c->sent = 0;
            /* Threshold is raised after this pulse, not when it starts. */
        }
    }
    timer_mod_ns(c->timer, c->started_ns + (c->elapsed_twons + 1) / 2);
}

static void tick(void *opaque)
{
    C3RmtChannel *c = opaque;
    if (c->running && c->position && !(c->position & 1) &&
        (c->active_limit & 0x1ff) && !c->sent) {
        c->parent->raw |= BIT(8 + c->index);
        irq_update(c->parent);
    }
    advance(c);
}

static void start(C3RmtChannel *c)
{
    ESP32C3RmtState *s = c->parent;
    unsigned blocks = (c->active >> 16) & 7;
    unsigned source = (s->sys_conf >> 24) & 3;
    if (!blocks || blocks > 4 - c->index ||
        (c->active & (BIT(3) | BIT(21))) || (c->active_limit & BIT(19)) ||
        !(s->sys_conf & BIT(26)) || !(s->sys_conf & BIT(0)) ||
        (s->sys_conf & (BIT(2) | 0xfff000)) || (source != 1 && source != 3)) {
        unsupported(c, "unsupported TX/clock/memory configuration");
        return;
    }
    C3RmtChannel *other = &s->tx[1 - c->index];
    if (other->running && ((c->index == 0 && blocks > 1) ||
        (c->index == 1 && ((other->active >> 16) & 7) > 1))) {
        unsupported(c, "overlapping active channel RAM");
        unsupported(other, "overlapping active channel RAM");
        return;
    }
    c->position = c->sent = 0;
    c->elapsed_twons = 0;
    c->started_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->running = true;
    advance(c);
}

static uint64_t reg_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3RmtState *s = opaque;
    switch (addr) {
    case 0x10: case 0x14: return s->tx[(addr - 0x10) / 4].config;
    case 0x28: case 0x2c: {
        C3RmtChannel *c = &s->tx[(addr - 0x28) / 4];
        return c->index * 48 + c->position / 2;
    }
    case 0x38: return s->raw;
    case 0x3c: return s->raw & s->ena;
    case 0x40: return s->ena;
    case 0x44: return 0;
    case 0x48: case 0x4c: return s->carrier[(addr - 0x48) / 4];
    case 0x58: case 0x5c: return s->tx[(addr - 0x58) / 4].limit;
    case 0x64: case 0x70: return 0;
    case 0x68: return s->sys_conf;
    default:
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED RMT read offset=0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void reg_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32C3RmtState *s = opaque;
    switch (addr) {
    case 0x10: case 0x14: {
        C3RmtChannel *c = &s->tx[(addr - 0x10) / 4];
        c->config = value & 0x7fff78;
        if (value & BIT(24)) {
            if (c->running && c->active != c->config && !(value & BIT(7))) {
                unsupported(c, "configuration change during TX");
            }
            c->active = c->config;
            c->active_limit = c->limit;
            if (value & BIT(7)) {
                stop(c, false);
                c->config &= ~BIT(7);
            }
            if (!c->running) {
                output(c, !!(c->active & BIT(5)), !!(c->active & BIT(6)));
            }
        }
        if ((value & BIT(1)) && !c->running) {
            c->position = 0;
        }
        if (value & BIT(0)) {
            if (c->running) {
                unsupported(c, "TX_START while already transmitting");
            } else {
                start(c);
            }
        }
        break;
    }
    case 0x40: s->ena = value & 0x3fff; irq_update(s); break;
    case 0x44: s->raw &= ~value; irq_update(s); break;
    case 0x48: case 0x4c: s->carrier[(addr - 0x48) / 4] = value; break;
    case 0x58: case 0x5c: s->tx[(addr - 0x58) / 4].limit = value & 0xfffff; break;
    case 0x68:
        for (unsigned i = 0; i < 2; i++) {
            if (s->tx[i].running && s->sys_conf != value) {
                unsupported(&s->tx[i], "clock change during TX");
            }
        }
        s->sys_conf = value & 0x87ffffff;
        break;
    case 0x64: case 0x70:
        if (!value) { break; }
        /* Fall through: simultaneous TX/divider restart need separate models. */
    default:
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED RMT write offset=0x%" HWADDR_PRIx " value=0x%" PRIx64 "\n", addr, value);
    }
}

static uint64_t ram_read(void *opaque, hwaddr addr, unsigned size)
{
    return ESP32C3_RMT(opaque)->ram[addr / 4];
}
static void ram_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32C3_RMT(opaque)->ram[addr / 4] = value;
}
static const MemoryRegionOps reg_ops = {
    .read = reg_read, .write = reg_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps ram_ops = {
    .read = ram_read, .write = ram_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static void reset(Object *obj, ResetType type)
{
    ESP32C3RmtState *s = ESP32C3_RMT(obj);
    s->sys_conf = 0x05000010;
    s->raw = s->ena = 0;
    for (unsigned i = 0; i < 2; i++) {
        C3RmtChannel *c = &s->tx[i];
        c->config = c->active = 0x710200;
        c->limit = c->active_limit = 128;
        c->position = c->sent = 0;
        c->running = false;
        timer_del(c->timer);
        output(c, false, false);
    }
    irq_update(s);
}
static void init(Object *obj)
{
    ESP32C3RmtState *s = ESP32C3_RMT(obj);
    SysBusDevice *bus = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->regs, obj, &reg_ops, s, "c3-rmt-regs", 0x100);
    memory_region_init_io(&s->memory, obj, &ram_ops, s, "c3-rmt-ram", sizeof(s->ram));
    sysbus_init_mmio(bus, &s->regs);
    sysbus_init_mmio(bus, &s->memory);
    sysbus_init_irq(bus, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->level, "tx-level", 2);
    qdev_init_gpio_out_named(DEVICE(obj), s->enable, "tx-enable", 2);
    for (unsigned i = 0; i < 2; i++) {
        s->tx[i].parent = s;
        s->tx[i].index = i;
        s->tx[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tick, &s->tx[i]);
    }
}
static void finalize(Object *obj)
{
    ESP32C3RmtState *s = ESP32C3_RMT(obj);
    for (unsigned i = 0; i < 2; i++) { timer_free(s->tx[i].timer); }
}
static void class_init(ObjectClass *klass, void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = reset;
}
static const TypeInfo info = {
    .name = TYPE_ESP32C3_RMT, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3RmtState), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_type(void) { type_register_static(&info); }
type_init(register_type)
