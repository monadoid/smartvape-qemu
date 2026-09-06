/*
 * ESP32-C3 GPIO emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32c3_gpio.h"


/* GPIO output latches only. Pad routing and interrupts remain unsupported.
 * Source: ESP32-C3 TRM, GPIO_OUT/ENABLE and W1TS/W1TC registers;
 * cross-checked against esp32c3 PAC 0.32.2 gpio.rs and gpio/out.rs.
 * The register data field is 26 bits. This does not assert 26 bonded pads.
 */
#define GPIO_DATA_MASK 0x03ffffff

/* First input slice: GPIO5, normal awake operation with an externally driven
 * digital pad. TRM v1.4 chapter 5: IN, STATUS, PCPU_INT, PIN5 and IO_MUX_GPIO5.
 * No analog thresholds, synchronizer delays, filter, sleep or NMI model. */
#define PIN5_BIT (1U << 5)

static void input_update(ESP32C3GPIOState *s)
{
    bool old = s->input5;
    unsigned type = (s->pin5 >> 7) & 7;
    s->input5 = s->pad5 && (s->mux5 & (1U << 9));
    if ((type == 1 && !old && s->input5) ||
        (type == 2 && old && !s->input5) ||
        (type == 3 && old != s->input5) ||
        (type == 4 && !s->input5) || (type == 5 && s->input5)) {
        s->status |= PIN5_BIT;
    }
    qemu_set_irq(s->parent.irq,
                 (s->pin5 & (1U << 13)) && (s->status & PIN5_BIT));
}

static void pad_input(void *opaque, int pin, int level)
{
    ESP32C3GPIOState *s = opaque;
    if (pin != 5) {
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO pad %d input\n", pin);
        return;
    }
    s->pad5 = level != 0;
    input_update(s);
}

static bool get_pad5(Object *obj, Error **errp)
{
    return ESP32C3_GPIO(obj)->pad5;
}

static void set_pad5(Object *obj, bool level, Error **errp)
{
    pad_input(ESP32C3_GPIO(obj), 5, level);
}

static uint64_t mux_read(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3GPIOState *s = opaque;
    return s->mux5;
}

static void mux_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32C3GPIOState *s = opaque;
    s->mux5 = value & 0xffff;
    if (value & ((1U << 15) | (1U << 1))) {
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO5 filter/sleep configuration\n");
    }
    input_update(s);
}

static const MemoryRegionOps mux_ops = {
    .read = mux_read,
    .write = mux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static bool c3_read(Esp32GpioState *parent, hwaddr addr, uint64_t *value)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(parent);
    switch (addr) {
    case 0x04: *value = s->out; return true;
    case 0x20: *value = s->enable; return true;
    case 0x3c: *value = s->input5 ? PIN5_BIT : 0; return true;
    case 0x44: *value = s->status; return true;
    case 0x5c: *value = (s->pin5 & (1U << 13)) ? s->status : 0; return true;
    case 0x88: *value = s->pin5; return true;
    default: return false;
    }
}

static bool c3_write(Esp32GpioState *parent, hwaddr addr, uint64_t value)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(parent);
    uint32_t bits = value & GPIO_DATA_MASK;
    switch (addr) {
    case 0x04: s->out = bits; break;
    case 0x08: s->out |= bits; break;
    case 0x0c: s->out &= ~bits; break;
    case 0x20: s->enable = bits; break;
    case 0x24: s->enable |= bits; break;
    case 0x28: s->enable &= ~bits; break;
    case 0x44: s->status = bits; input_update(s); break;
    case 0x48: s->status |= bits; input_update(s); break;
    case 0x4c: s->status &= ~bits; input_update(s); break;
    case 0x88:
        s->pin5 = value & 0x3ff9f;
        if (value & 0x3dc1f) {
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED GPIO5 sync/output/wakeup/NMI configuration\n");
        }
        input_update(s);
        break;
    default: return false;
    }
    return true;
}

static void c3_reset(Object *obj, ResetType type)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    s->out = 0;
    s->enable = 0;
    s->pin5 = 0;
    s->mux5 = 0xb00;
    s->status = 0;
    s->input5 = false;
    input_update(s);
}

static void esp32c3_gpio_init(Object *obj)
{
    /* Set the default value for the property */
    object_property_set_int(obj, "strap_mode", ESP32C3_STRAP_MODE_FLASH_BOOT, &error_fatal);
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    qdev_init_gpio_in_named(DEVICE(obj), pad_input, "pad", 22);
    object_property_add_bool(obj, "pad5-level", get_pad5, set_pad5);
    /* Map just GPIO5's four bytes; all other mux registers retain upstream
     * fallback diagnostics instead of silently appearing implemented. */
    memory_region_init_io(&s->button_mux, obj, &mux_ops, s, "gpio5-iomux", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->button_mux);
}

/* If we need to override any function from the parent (reset, realize, ...), it shall be done
 * in this class_init function */
static void esp32c3_gpio_class_init(ObjectClass *klass, void *data)
{
    Esp32GpioClass *gpio = ESP32_GPIO_CLASS(klass);
    gpio->read_reg = c3_read;
    gpio->write_reg = c3_write;
    RESETTABLE_CLASS(klass)->phases.hold = c3_reset;
}

static const TypeInfo esp32c3_gpio_info = {
    .name = TYPE_ESP32C3_GPIO,
    .parent = TYPE_ESP32_GPIO,
    .instance_size = sizeof(ESP32C3GPIOState),
    .instance_init = esp32c3_gpio_init,
    .class_init = esp32c3_gpio_class_init,
    .class_size = sizeof(ESP32C3GPIOClass),
};

static void esp32c3_gpio_register_types(void)
{
    type_register_static(&esp32c3_gpio_info);
}

type_init(esp32c3_gpio_register_types)
