/* SPDX-License-Identifier: GPL-2.0-or-later
 * ESP32-C3 APB SARADC one-shot digital interface, TRM v1.4 34.2.3.3 / 34.6.
 * Analog transfer, internal calibration routing and timing are deliberately NOT
 * invented. A board backend must supply each conversion and its completion time.
 * With no backend the request stays pending and emits a fidelity gap.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "sysemu/runstate.h"
#define TYPE_C3_ADC "esp32c3.adc"
OBJECT_DECLARE_SIMPLE_TYPE(C3Adc, C3_ADC)
struct C3Adc {
    SysBusDevice parent;
    MemoryRegion regs, analog_regs;
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t ctrl, ctrl2, onetime, clkm, arb, raw, ena, data;
    uint32_t channel, attenuation, pending;
    uint64_t requests, sample_code;
    uint32_t pause_on_request;
    uint32_t analog_ctrl[2], analog_conf[3], init_code, dref, calibration_route;
    uint8_t sar[8];
};
static void update(C3Adc *s) { qemu_set_irq(s->irq, !!(s->raw & s->ena)); }
static void complete(void *opaque)
{
    C3Adc *s = opaque;
    if (!s->pending) { return; }
    s->data = s->sample_code;
    if (s->ctrl2 & BIT(9)) { s->data ^= 0xfff; }
    s->pending = 0;
    s->sample_code = UINT64_MAX;
    s->raw |= BIT(31);
    update(s);
}
static void set_code(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    C3Adc *s = C3_ADC(obj);
    uint64_t value;
    if (!visit_type_uint64(v, name, &value, errp)) { return; }
    if (runstate_is_running() || !s->pending || timer_pending(s->timer) || value > 4095) {
        error_setg(errp, "Pause at an unscheduled ADC request and supply a 12-bit analog-model result");
        return;
    }
    s->sample_code = value;
}
static void set_delay(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    C3Adc *s = C3_ADC(obj);
    uint64_t delay;
    if (!visit_type_uint64(v, name, &delay, errp)) { return; }
    if (runstate_is_running() || !s->pending || timer_pending(s->timer) ||
        s->sample_code > 4095 || !delay || delay > 1000000000) {
        error_setg(errp, "Paused pending ADC needs sample-code and a positive modeled delay <=1s");
        return;
    }
    timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}
static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    C3Adc *s = opaque;
    switch (addr) {
    case 0: return s->ctrl;
    case 4: return s->ctrl2;
    case 0x20: return s->onetime;
    case 0x24: return s->arb;
    case 0x2c: return s->data;
    case 0x40: return s->ena;
    case 0x44: return s->raw;
    case 0x48: return s->raw & s->ena;
    case 0x4c: return 0;
    case 0x54: return s->clkm;
    default:
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED read 0x%08" HWADDR_PRIx " (APB_SARADC)\n", 0x60040000 + addr);
        return 0;
    }
}
static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    C3Adc *s = opaque;
    switch (addr) {
    case 0: s->ctrl = value & 0xd883ffc3; break;
    case 4:
        s->ctrl2 = value & 0x1fff7ff;
        if (value & BIT(24)) {
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED ADC timer scan\n");
        }
        break;
    case 0x24: s->arb = value; break;
    case 0x54: s->clkm = value; break;
    case 0x20: {
        bool rising = (value & BIT(29)) && !(s->onetime & BIT(29));
        s->onetime = value & 0xff800000;
        if (!(value & BIT(29))) {
            timer_del(s->timer);
            s->pending = 0;
            s->sample_code = UINT64_MAX;
        }
        if (rising) {
            if ((value & (BIT(31) | BIT(30))) != BIT(31) ||
                ((value >> 25) & 15) > 4 || (s->ctrl2 & BIT(24))) {
                qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED ADC unsupported channel/controller/mode\n");
                break;
            }
            s->channel = (value >> 25) & 15;
            s->attenuation = (value >> 23) & 3;
            s->requests++;
            s->pending = 1;
            s->sample_code = UINT64_MAX;
            if (s->pause_on_request) {
                vm_stop(RUN_STATE_PAUSED);
            } else {
                qemu_log_mask(LOG_UNIMP,
                "SMARTVAPE_UNMODELED ADC analog request=%" PRIu64 " channel=%u attenuation=%u needs transfer/calibration/timing backend\n",
                s->requests, s->channel, s->attenuation);
            }
        }
        break;
    }
    case 0x40: s->ena = value & 0xfc000000; update(s); break;
    case 0x4c: s->raw &= ~value; update(s); break;
    default:
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED write 0x%08" HWADDR_PRIx " (APB_SARADC)\n", 0x60040000 + addr);
    }
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
/* Internal analog bus command layout verified against the pinned C3 ROM:
 * read routine 0x40038e58, write routine 0x40039170; data[23:16], register[15:8],
 * block[7:0], write bit24, busy bit25, execute bit26. esp-hal C3 regi2c.rs
 * identifies SAR block 0x69 registers. This is a zero-latency register transport,
 * not PLL/radio calibration or analog settling. Unsupported blocks remain gaps.
 * Configuration addresses: ESP-IDF v5.4.2 soc/esp32c3/include/soc/regi2c_defs.h.
 */
static uint64_t analog_read(void *opaque, hwaddr addr, unsigned size)
{
    C3Adc *s = opaque;
    if (addr < 8) { return s->analog_ctrl[addr / 4]; }
    if (addr >= 0x40 && addr <= 0x48) { return s->analog_conf[(addr - 0x40) / 4]; }
    qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED analog bus read offset=%" HWADDR_PRIx "\n", addr);
    return 0;
}
static void analog_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    C3Adc *s = opaque;
    if (addr >= 0x40 && addr <= 0x48) {
        s->analog_conf[(addr - 0x40) / 4] = value;
        return;
    }
    if (addr < 8) {
        unsigned block = value & 0xff, reg = (value >> 8) & 0xff;
        s->analog_ctrl[addr / 4] = value & ~(BIT(25) | BIT(26));
        if (!(value & BIT(26))) { return; }
        if (block != 0x69 || reg >= 8) {
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED analog bus block=%x register=%u\n", block, reg);
            return;
        }
        if (value & BIT(24)) { s->sar[reg] = value >> 16; }
        s->analog_ctrl[addr / 4] = (s->analog_ctrl[addr / 4] & ~0xff0000) | (s->sar[reg] << 16);
        s->init_code = s->sar[0] | ((s->sar[1] & 15) << 8);
        s->dref = (s->sar[2] >> 4) & 7;
        s->calibration_route = (s->sar[7] >> 4) & 3;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED analog bus write offset=%" HWADDR_PRIx "\n", addr);
}
static const MemoryRegionOps analog_ops = {
    .read = analog_read, .write = analog_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static void reset(Object *obj, ResetType type)
{
    C3Adc *s = C3_ADC(obj);
    s->ctrl = 0x40038240;
    s->ctrl2 = 0xa1fe;
    s->onetime = 0x1a000000;
    s->arb = 0x900;
    s->clkm = 0x04; /* esp32c3 0.32.2 APB_SARADC.CLKM_CONF */
    s->raw = s->ena = s->data = s->pending = 0;
    s->sample_code = UINT64_MAX;
    memset(s->sar, 0, sizeof(s->sar));
    memset(s->analog_ctrl, 0, sizeof(s->analog_ctrl));
    memset(s->analog_conf, 0, sizeof(s->analog_conf));
    s->init_code = s->dref = s->calibration_route = 0;
    timer_del(s->timer);
    update(s);
}
static void init(Object *obj)
{
    C3Adc *s = C3_ADC(obj);
    memory_region_init_io(&s->regs, obj, &ops, s, "c3-adc", 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);
    memory_region_init_io(&s->analog_regs, obj, &analog_ops, s, "c3-analog-bus", 0x4c);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->analog_regs);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, complete, s);
    object_property_add_uint32_ptr(obj, "pending", &s->pending, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "channel", &s->channel, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "attenuation", &s->attenuation, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "requests", &s->requests, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "pause-on-request", &s->pause_on_request, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "init-code", &s->init_code, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "dref", &s->dref, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "calibration-route", &s->calibration_route, OBJ_PROP_FLAG_READ);
    object_property_add(obj, "sample-code", "uint64", NULL, set_code, NULL, NULL);
    object_property_add(obj, "complete-after-ns", "uint64", NULL, set_delay, NULL, NULL);
}
static void finalize(Object *obj) { timer_free(C3_ADC(obj)->timer); }
static void class_init(ObjectClass *klass, void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = reset;
}
static const TypeInfo info = {
    .name = TYPE_C3_ADC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(C3Adc), .instance_init = init,
    .instance_finalize = finalize, .class_init = class_init,
};
static void register_type(void) { type_register_static(&info); }
type_init(register_type)
