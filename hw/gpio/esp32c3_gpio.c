/* ESP32-C3 GPIO. GPL-2.0-or-later; based on Espressif's GPIO device.
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Sources: ESP32-C3 TRM v1.4 chapter 5 and esp32c3 PAC 0.32.2.
 * Awake digital GPIO only: no pad voltage, synchronizer, sleep or NMI model.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/irq.h"
#include "hw/gpio/esp32c3_gpio.h"
#include "sysemu/runstate.h"

/* Bits 22..25 exist in the register fields but are explicitly invalid in TRM
 * 5.5.3. Never expose them as bonded output pins. */
#define DATA_MASK 0x03ffffff
#define PAD_MASK 0x003fffff

static uint32_t cpu_status(ESP32C3GPIOState *s)
{
    uint32_t enabled = 0;
    for (int pin = 0; pin < 22; pin++) {
        if (s->pin[pin] & BIT(13)) {
            enabled |= BIT(pin);
        }
    }
    return enabled & s->status;
}

static void update(ESP32C3GPIOState *s)
{
    uint32_t old_input = s->input;
    uint32_t old_known = s->input_known;
    uint32_t old_drive = s->drive_level;
    uint32_t old_enable = s->drive_enable;
    uint32_t old_valid = s->drive_valid;
    s->input = s->input_known = 0;
    s->drive_enable = s->drive_level = s->drive_valid = 0;
    for (int pin = 0; pin < 22; pin++) {
        uint32_t bit = BIT(pin), mux = s->mux[pin], cfg = s->out_sel[pin];
        bool level = false, known = false;
        bool gpio = ((mux >> 12) & 7) == 1;
        bool awake = !(mux & BIT(1));
        bool simple = (cfg & 0xff) == 128;
        unsigned signal = cfg & 0xff;
        bool rmt = signal == 51 || signal == 52;
        if (gpio && awake && (simple || rmt)) {
            bool source_enable = simple || (cfg & BIT(9)) ?
                                 !!(s->enable & bit) :
                                 !!(s->rmt_enable & BIT(signal - 51));
            bool source_level = simple ? !!(s->out & bit) :
                                !!(s->rmt_level & BIT(signal - 51));
            bool enable = source_enable ^ !!(cfg & BIT(10));
            bool output = source_level ^ !!(cfg & BIT(8));
            /* Open drain high releases the driver. */
            enable &= !(output && (s->pin[pin] & BIT(2)));
            s->drive_valid |= bit;
            if (enable) {
                s->drive_enable |= bit;
                if (output) { s->drive_level |= bit; }
                known = true;
                level = output;
            }
        }
        if (s->pad_driven & bit) {
            bool external = !!(s->pad_level & bit);
            if (known && external != level) {
                known = false; /* contention is not a valid digital zero */
            } else {
                known = true;
                level = external;
            }
        } else if (!known && (s->drive_valid & bit) &&
                   !(s->drive_enable & bit)) {
            unsigned pull = (mux >> 7) & 3;
            known = pull == 1 || pull == 2;
            level = pull == 2;
        }
        /* Simple input does not require selecting the GPIO output function. */
        if (!(mux & BIT(9))) {
            level = false;
            known = true;
        } else if (!awake || (mux & BIT(15)) || (s->pin[pin] & 0x1b)) {
            known = false; /* sleep/filter/synchronizer not implemented */
        }
        if ((mux & BIT(9)) && (s->pad_unknown & bit)) { known = false; }
        if (known) {
            s->input_known |= bit;
            if (level) { s->input |= bit; }
            bool old = !!(old_input & bit), was_known = !!(old_known & bit);
            unsigned type = (s->pin[pin] >> 7) & 7;
            if ((type == 1 && was_known && !old && level) ||
                (type == 2 && was_known && old && !level) ||
                (type == 3 && was_known && old != level) ||
                (type == 4 && !level) || (type == 5 && level)) {
                s->status |= bit;
            }
        }
    }
    qemu_set_irq(s->parent.irq, cpu_status(s) != 0);
    /* Record the actual chip output at every control transition, including Z
     * and unsupported routing. This is NOT a MOSFET or power model. */
    uint32_t changed = (old_drive ^ s->drive_level) |
                       (old_enable ^ s->drive_enable) | (old_valid ^ s->drive_valid);
    if (changed & BIT(7)) {
        s->gpio7_changes++;
        if (s->pause_on_gpio7 && runstate_is_running()) { vm_stop(RUN_STATE_PAUSED); }
    }
    for (unsigned pin = 6; pin <= 7; pin++) {
        if (changed & BIT(pin)) {
            int drive = !(s->drive_valid & BIT(pin)) ? -2 :
                        !(s->drive_enable & BIT(pin)) ? -1 : !!(s->drive_level & BIT(pin));
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_GPIO time_ns=%" PRId64 " pin=%u drive=%d\n",
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), pin, drive);
        }
    }
}

static void rmt_level(void *opaque, int channel, int level)
{
    ESP32C3GPIOState *s = opaque;
    s->rmt_level = (s->rmt_level & ~BIT(channel)) | (level ? BIT(channel) : 0);
    update(s);
}

static void set_unknown(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    uint32_t mask;
    if (!visit_type_uint32(v, name, &mask, errp)) { return; }
    if (runstate_is_running() || (mask & ~PAD_MASK)) {
        error_setg(errp, "Pause before changing unknown electrical inputs");
        return;
    }
    s->pad_unknown = mask;
    update(s);
}

static void rmt_enable(void *opaque, int channel, int level)
{
    ESP32C3GPIOState *s = opaque;
    s->rmt_enable = (s->rmt_enable & ~BIT(channel)) | (level ? BIT(channel) : 0);
    update(s);
}

static void pad_input(void *opaque, int pin, int level)
{
    ESP32C3GPIOState *s = opaque;
    s->pad_driven |= BIT(pin);
    s->pad_level = (s->pad_level & ~BIT(pin)) | (level ? BIT(pin) : 0);
    update(s);
}

static void pad_release(void *opaque, int pin, int level)
{
    ESP32C3GPIOState *s = opaque;
    if (level) { s->pad_driven &= ~BIT(pin); }
    update(s);
}

static void get_pad(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t pin = GPOINTER_TO_UINT(opaque);
    bool level = !!(ESP32C3_GPIO(obj)->pad_level & BIT(pin));
    visit_type_bool(v, name, &level, errp);
}

static void set_pad(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    bool level;
    if (visit_type_bool(v, name, &level, errp)) {
        pad_input(ESP32C3_GPIO(obj), GPOINTER_TO_UINT(opaque), level);
    }
}

static uint64_t mux_read(void *opaque, hwaddr addr, unsigned size)
{
    return ESP32C3_GPIO(opaque)->mux[addr / 4];
}

static void mux_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32C3GPIOState *s = opaque;
    s->mux[addr / 4] = value & 0xffff;
    if (value & (BIT(15) | BIT(1))) {
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO%u filter/sleep configuration\n", (unsigned)addr / 4);
    }
    update(s);
}

static const MemoryRegionOps mux_ops = {
    .read = mux_read, .write = mux_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static bool c3_read(Esp32GpioState *parent, hwaddr addr, uint64_t *value)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(parent);
    if (addr >= 0x74 && addr < 0xcc) { *value = s->pin[(addr - 0x74) / 4]; return true; }
    if (addr >= 0x554 && addr < 0x5ac) { *value = s->out_sel[(addr - 0x554) / 4]; return true; }
    switch (addr) {
    case 0x04: *value = s->out; return true;
    case 0x20: *value = s->enable; return true;
    case 0x3c:
        if ((~s->input_known & PAD_MASK) & ~s->reported_unknown) {
            uint32_t unknown = ~s->input_known & PAD_MASK;
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO input unknown_mask=0x%x\n", unknown);
            s->reported_unknown |= unknown;
        }
        *value = s->input; return true;
    case 0x44: *value = s->status; return true;
    case 0x5c: *value = cpu_status(s); return true;
    default: return false;
    }
}

static bool c3_write(Esp32GpioState *parent, hwaddr addr, uint64_t value)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(parent);
    uint32_t bits = value & DATA_MASK;
    if (addr >= 0x74 && addr < 0xcc) {
        s->pin[(addr - 0x74) / 4] = value & 0x3ff9f;
        if (value & 0x3dc1b) {
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO%u sync/wakeup/NMI configuration\n", (unsigned)(addr - 0x74) / 4);
        }
    } else if (addr >= 0x554 && addr < 0x5ac) {
        s->out_sel[(addr - 0x554) / 4] = value & 0x7ff;
        if ((value & 0xff) != 128 && (value & 0xff) != 51 && (value & 0xff) != 52) {
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO%u peripheral output signal=%u\n",
                          (unsigned)(addr - 0x554) / 4, (unsigned)value & 0xff);
        }
    } else {
        switch (addr) {
        case 0x04: s->out = bits; break;
        case 0x08: s->out |= bits; break;
        case 0x0c: s->out &= ~bits; break;
        case 0x20: s->enable = bits; break;
        case 0x24: s->enable |= bits; break;
        case 0x28: s->enable &= ~bits; break;
        case 0x44: s->status = bits; break;
        case 0x48: s->status |= bits; break;
        case 0x4c: s->status &= ~bits; break;
        default: return false;
        }
    }
    update(s);
    return true;
}

static void c3_reset(Object *obj, ResetType type)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    s->out = s->enable = s->status = s->reported_unknown = 0;
    s->input = s->input_known = 0;
    for (int pin = 0; pin < 22; pin++) {
        s->pin[pin] = 0;
        s->mux[pin] = 0xb00; /* TRM register 5.21 */
        s->out_sel[pin] = 0x80;
    }
    update(s); /* external fixture levels survive a chip reset */
    qemu_log_mask(LOG_UNIMP, "SMARTVAPE_GPIO time_ns=%" PRId64 " pin=7 drive=-2 reset=1\n",
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void esp32c3_gpio_init(Object *obj)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    object_property_set_int(obj, "strap_mode", ESP32C3_STRAP_MODE_FLASH_BOOT, &error_fatal);
    qdev_init_gpio_in_named(DEVICE(obj), pad_input, "pad", 22);
    qdev_init_gpio_in_named(DEVICE(obj), pad_release, "release-pad", 22);
    qdev_init_gpio_in_named(DEVICE(obj), rmt_level, "rmt-level", 2);
    qdev_init_gpio_in_named(DEVICE(obj), rmt_enable, "rmt-enable", 2);
    for (int pin = 0; pin < 22; pin++) {
        g_autofree char *name = g_strdup_printf("pad%d-level", pin);
        object_property_add(obj, name, "bool", get_pad, set_pad, NULL, GUINT_TO_POINTER(pin));
    }
    object_property_add_uint32_ptr(obj, "drive-level", &s->drive_level, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "drive-enable", &s->drive_enable, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "drive-valid", &s->drive_valid, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "input-known", &s->input_known, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "pause-on-gpio7", &s->pause_on_gpio7, OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "unknown-pad-mask", "uint32", NULL, set_unknown, NULL, NULL);
    object_property_add_uint64_ptr(obj, "gpio7-changes", &s->gpio7_changes, OBJ_PROP_FLAG_READ);
    memory_region_init_io(&s->mux_regs, obj, &mux_ops, s, "gpio-iomux", 22 * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mux_regs);
}

static void esp32c3_gpio_class_init(ObjectClass *klass, void *data)
{
    Esp32GpioClass *gpio = ESP32_GPIO_CLASS(klass);
    gpio->read_reg = c3_read;
    gpio->write_reg = c3_write;
    RESETTABLE_CLASS(klass)->phases.hold = c3_reset;
}

static const TypeInfo esp32c3_gpio_info = {
    .name = TYPE_ESP32C3_GPIO, .parent = TYPE_ESP32_GPIO,
    .instance_size = sizeof(ESP32C3GPIOState), .instance_init = esp32c3_gpio_init,
    .class_init = esp32c3_gpio_class_init, .class_size = sizeof(ESP32C3GPIOClass),
};

static void esp32c3_gpio_register_types(void) { type_register_static(&esp32c3_gpio_info); }
type_init(esp32c3_gpio_register_types)
