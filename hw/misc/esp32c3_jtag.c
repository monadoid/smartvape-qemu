/* ESP32-C3 USB serial TX FIFO, TRM v1.4 30.6 registers 30.6/30.10.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Explicit console fixture: chardev is an immediately consuming host. Without
 * it the submitted/full FIFO remains blocked. No USB PHY, enumeration, JTAG,
 * RX or USB interrupts are claimed. USB VBUS is an independent board input.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/esp32c3_jtag.h"

static void submit(ESP32C3UsbJtagState *s)
{
    s->submitted = true;
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        int written = qemu_chr_fe_write_all(&s->chr, s->tx, s->count);
        if (written == s->count) {
            s->count = 0;
            s->submitted = false;
        } else {
            qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED USB console backend failed\n");
        }
    }
}
static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    ESP32C3UsbJtagState *s = opaque;
    if (addr == 4) {
        return (!s->submitted && s->count < sizeof(s->tx)) ? BIT(1) : 0;
    }
    qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED USB read offset=0x%" HWADDR_PRIx "\n", addr);
    return 0;
}
static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ESP32C3UsbJtagState *s = opaque;
    if (addr == 0) {
        if (!s->submitted && s->count < sizeof(s->tx)) {
            s->tx[s->count++] = value;
            if (s->count == sizeof(s->tx)) { submit(s); }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "USB serial write while FIFO unavailable\n");
        }
    } else if (addr == 4) {
        if (value & 1) { submit(s); }
    } else {
        qemu_log_mask(LOG_UNIMP, "SMARTVAPE_UNMODELED USB write offset=0x%" HWADDR_PRIx "\n", addr);
    }
}
static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static void reset(Object *obj, ResetType type)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(obj);
    s->count = 0;
    s->submitted = false;
}
static void init(Object *obj)
{
    ESP32C3UsbJtagState *s = ESP32C3_JTAG(obj);
    memory_region_init_io(&s->iomem, obj, &ops, s, TYPE_ESP32C3_JTAG, ESP32C3_JTAG_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}
static Property properties[] = {
    DEFINE_PROP_CHR("chardev", ESP32C3UsbJtagState, chr),
    DEFINE_PROP_END_OF_LIST(),
};
static void class_init(ObjectClass *klass, void *data)
{
    RESETTABLE_CLASS(klass)->phases.hold = reset;
    device_class_set_props(DEVICE_CLASS(klass), properties);
}
static const TypeInfo info = {
    .name = TYPE_ESP32C3_JTAG, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3UsbJtagState), .instance_init = init,
    .class_init = class_init,
};
static void register_type(void) { type_register_static(&info); }
type_init(register_type)
