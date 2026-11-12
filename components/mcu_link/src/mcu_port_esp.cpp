// Serial port opening for ESP-IDF.
//
//   "uart:<num>:<tx_gpio>:<rx_gpio>"   e.g. "uart:2:27:22" (CYD -> SKR Pico)
//   "usbjtag"                          ESP32-S3/C3 USB-Serial-JTAG (the
//                                      console USB port; simulated MCU
//                                      through tools/sim_bridge.py)
//
// Installs the UART driver and opens it through the VFS so Klipper's
// serialqueue can use read()/write()/poll() on it like on Linux.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/mcu.hpp"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "soc/soc_caps.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

namespace kl {

static int open_uart_num = -1;
static bool open_usbjtag = false;

#if SOC_USB_SERIAL_JTAG_SUPPORTED
static int
open_usb_serial_jtag(std::string *err)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {};
        cfg.tx_buffer_size = 1024;
        cfg.rx_buffer_size = 1024;
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            if (err)
                *err = "usb_serial_jtag_driver_install failed";
            return -1;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    int fd = open("/dev/usbserjtag", O_RDWR | O_NONBLOCK);
    if (fd < 0 && err)
        *err = "Unable to open /dev/usbserjtag";
    if (fd >= 0)
        open_usbjtag = true;
    return fd;
}
#endif

int
mcu_open_serial(const std::string &port, int baud, std::string *err)
{
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (port == "usbjtag")
        return open_usb_serial_jtag(err);
#endif
    int num, tx, rx;
    if (sscanf(port.c_str(), "uart:%d:%d:%d", &num, &tx, &rx) != 3) {
        if (err)
            *err = "Bad serial port '" + port + "' (want uart:<num>:<tx>:<rx>)";
        return -1;
    }
    uart_config_t cfg = {};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (!uart_is_driver_installed((uart_port_t)num)
        && uart_driver_install((uart_port_t)num, 1024, 1024, 0, nullptr, 0)
               != ESP_OK) {
        if (err)
            *err = "uart_driver_install failed";
        return -1;
    }
    uart_param_config((uart_port_t)num, &cfg);
    uart_set_pin((uart_port_t)num, tx, rx, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE);
    // Low latency: wake the reader as soon as a byte arrives
    uart_set_rx_full_threshold((uart_port_t)num, 1);
    uart_set_rx_timeout((uart_port_t)num, 1);
    uart_vfs_dev_use_driver(num);
    // The VFS converts line endings for console text by default (RX CR->LF,
    // TX LF->CRLF), which corrupts binary Klipper frames: pass bytes as-is
    uart_vfs_dev_port_set_rx_line_endings(num, ESP_LINE_ENDINGS_LF);
    uart_vfs_dev_port_set_tx_line_endings(num, ESP_LINE_ENDINGS_LF);
    char path[16];
    snprintf(path, sizeof(path), "/dev/uart/%d", num);
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (err)
            *err = std::string("Unable to open ") + path;
        return -1;
    }
    uart_flush_input((uart_port_t)num);
    open_uart_num = num;
    return fd;
}

void
mcu_close_serial(int fd)
{
    close(fd);
    if (open_uart_num >= 0) {
        // Back to the console defaults (CONFIG_NEWLIB_*_LINE_ENDING_*)
        uart_vfs_dev_port_set_rx_line_endings(open_uart_num, ESP_LINE_ENDINGS_CR);
        uart_vfs_dev_port_set_tx_line_endings(open_uart_num,
                                              ESP_LINE_ENDINGS_CRLF);
    }
    open_uart_num = -1;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    if (open_usbjtag) {
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
        open_usbjtag = false;
    }
#endif
}

} // namespace kl
