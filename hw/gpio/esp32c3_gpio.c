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

static bool c3_read(Esp32GpioState *parent, hwaddr addr, uint64_t *value)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(parent);
    switch (addr) {
    case 0x04: *value = s->out; return true;
    case 0x20: *value = s->enable; return true;
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
    default: return false;
    }
    return true;
}

static void c3_reset(Object *obj, ResetType type)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    s->out = 0;
    s->enable = 0;
}

static void esp32c3_gpio_init(Object *obj)
{
    /* Set the default value for the property */
    object_property_set_int(obj, "strap_mode", ESP32C3_STRAP_MODE_FLASH_BOOT, &error_fatal);
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
