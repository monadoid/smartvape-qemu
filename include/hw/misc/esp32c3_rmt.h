/* ESP32-C3 RMT transmit engine. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "hw/sysbus.h"
#include "qemu/timer.h"
#define TYPE_ESP32C3_RMT "esp32c3.rmt"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32C3RmtState, ESP32C3_RMT)
typedef struct C3RmtChannel {
    ESP32C3RmtState *parent;
    QEMUTimer *timer;
    unsigned index, position, sent;
    uint32_t config, active, limit, active_limit, word;
    uint64_t elapsed_twons;
    int64_t started_ns;
    bool running;
} C3RmtChannel;
struct ESP32C3RmtState {
    SysBusDevice parent_obj;
    MemoryRegion regs, memory;
    qemu_irq irq, level[2], enable[2];
    C3RmtChannel tx[2];
    uint32_t ram[192], sys_conf, raw, ena, carrier[2];
};
