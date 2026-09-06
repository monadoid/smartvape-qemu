#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"
#include "esp32_gpio.h"

#define TYPE_ESP32C3_GPIO "esp32c3.gpio"
#define ESP32C3_GPIO(obj)           OBJECT_CHECK(ESP32C3GPIOState, (obj), TYPE_ESP32C3_GPIO)
#define ESP32C3_GPIO_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32C3GPIOClass, obj, TYPE_ESP32C3_GPIO)
#define ESP32C3_GPIO_CLASS(klass)   OBJECT_CLASS_CHECK(ESP32C3GPIOClass, klass, TYPE_ESP32C3_GPIO)

/* Bootstrap options for ESP32-C3 (4-bit) */
#define ESP32C3_STRAP_MODE_FLASH_BOOT 0x8   /* SPI Boot */
#define ESP32C3_STRAP_MODE_UART_BOOT  0x2   /* Diagnostic Mode0+UART0 download Mode */
#define ESP32C3_STRAP_MODE_USB_BOOT   0x0   /* Diagnostic Mode1+USB download Mode */

typedef struct ESP32C3State {
    Esp32GpioState parent;
    uint32_t out;
    uint32_t enable;
    MemoryRegion mux_regs;
    uint32_t pin[22];
    uint32_t mux[22];
    uint32_t out_sel[22];
    uint32_t status;
    uint32_t pad_level, pad_driven;
    uint32_t input, input_known, reported_unknown;
    uint32_t drive_level, drive_enable, drive_valid;
    uint32_t rmt_level, rmt_enable;
} ESP32C3GPIOState;

typedef struct ESP32C3GPIOClass {
    Esp32GpioClass parent;
} ESP32C3GPIOClass;
