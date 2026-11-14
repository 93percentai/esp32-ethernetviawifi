#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc=${CC:-cc}
flags='-std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
$cc $flags -Ifirmware-korvo/main tests/test_ecm.c firmware-korvo/main/ecm_protocol.c -o "$build_dir/ecm"
"$build_dir/ecm"
$cc $flags -Ifirmware/main tests/test_form.c firmware/main/form_codec.c -o "$build_dir/form"
"$build_dir/form"
$cc $flags -Itests/support -Ifirmware-korvo/main -Ifirmware/main tests/test_usb_driver.c firmware-korvo/main/ecm_protocol.c firmware-korvo/main/usb_net.c firmware-korvo/main/usb_net_class.c firmware-korvo/main/usb_net_native.c firmware-korvo/main/vendor/rtl8152_port.c -o "$build_dir/usb"
"$build_dir/usb"
$cc $flags -Itests/support -Ifirmware-korvo/main -Ifirmware/main tests/test_bridge.c firmware-korvo/main/ecm_protocol.c -o "$build_dir/bridge"
"$build_dir/bridge"
$cc $flags -Ifirmware-korvo/main tests/test_log_buffer.c firmware-korvo/main/log_buffer.c -o "$build_dir/log"
"$build_dir/log"
$cc $flags -Ifirmware-korvo/main tests/test_usb_net.c firmware-korvo/main/usb_net.c firmware-korvo/main/ecm_protocol.c -o "$build_dir/protocols"
"$build_dir/protocols"
$cc $flags -Ifirmware-korvo/main tests/test_usb_setup.c firmware-korvo/main/usb_net.c firmware-korvo/main/ecm_protocol.c firmware-korvo/main/usb_net_class.c firmware-korvo/main/usb_net_native.c firmware-korvo/main/vendor/rtl8152_port.c -o "$build_dir/setup"
"$build_dir/setup"
$cc $flags -DCONFIG_KORVO_USB_AUTO_CONFIGURATION=0 -Itests/support -Ifirmware-korvo/main -Ifirmware/main tests/test_usb_driver.c firmware-korvo/main/ecm_protocol.c firmware-korvo/main/usb_net.c firmware-korvo/main/usb_net_class.c firmware-korvo/main/usb_net_native.c firmware-korvo/main/vendor/rtl8152_port.c -o "$build_dir/usb_manual"
"$build_dir/usb_manual"
