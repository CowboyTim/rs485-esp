#!/bin/bash

set -ef -o pipefail

MODULE=${MODULE:-rs485-esp}
DEV_BOARD=${DEV_BOARD:-esp8266:esp8266:generic}
PLATFORM_BOARD=${PLATFORM_BOARD:-esp8266:esp8266}
DEV_PORT=${DEV_PORT:-/dev/ttyUSB0}

function do_update(){
    DEV_URLS=${DEV_URLS:-http://arduino.esp8266.com/stable/package_esp8266com_index.json}
    [ "${DEV_UPDATE:-0}" = 1 ] && {
        arduino-cli --additional-urls "$DEV_URLS" update
        arduino-cli --additional-urls "$DEV_URLS" core install "${PLATFORM_BOARD}"
        arduino-cli --additional-urls "$DEV_URLS" lib update-index
        arduino-cli --additional-urls "$DEV_URLS" lib install 'SerialCommands'
        arduino-cli --additional-urls "$DEV_URLS" lib install 'ModbusMaster'
        arduino-cli --additional-urls "$DEV_URLS" lib upgrade
        arduino-cli --additional-urls "$DEV_URLS" lib list
        arduino-cli --additional-urls "$DEV_URLS" board list
    }
}

function do_build(){
    DEV_EXTRA_FLAGS=""
    if [ ! -z "${DEBUG}" -a "${DEBUG}" = "1" ]; then
        DEV_EXTRA_FLAGS="$DEV_EXTRA_FLAGS -DDEBUG"
    fi
    if [ ! -z "${AT_DEBUG}" -a "${AT_DEBUG}" = "1" ]; then
        DEV_EXTRA_FLAGS="$DEV_EXTRA_FLAGS -DAT_DEBUG"
    fi
    if [ ! -z "${VERBOSE}" ]; then
        DEV_EXTRA_FLAGS="$DEV_EXTRA_FLAGS -DVERBOSE"
    fi
    if [ ! -z "${DEFAULT_NTP_SERVER}" ]; then
        DEV_EXTRA_FLAGS="$DEV_EXTRA_FLAGS -DDEFAULT_NTP_SERVER=\"${DEFAULT_NTP_SERVER}\""
    fi
    arduino-cli -b "${DEV_BOARD}" compile \
        --log \
        --log-level info \
        --output-dir dist \
        --build-property compiler.cpp.extra_flags="$DEV_EXTRA_FLAGS" \
        --build-property compiler.c.extra_flags="$DEV_EXTRA_FLAGS" \
        --build-property build.partitions=min_spiffs \
        $MODULE \
        || exit $?
}

function do_upload(){
    arduino-cli -b "${DEV_BOARD}" upload -p ${DEV_PORT} $MODULE
}

function do_monitor(){
    arduino-cli -b "${DEV_BOARD}" monitor -p ${DEV_PORT} -c baudrate=${DEV_BOARD_BAUDRATE:-74880}
}

case $1 in
    deploy)
        do_build
        do_upload
        ;;
    upload)
        do_upload
        ;;
    monitor)
        do_monitor
        ;;
    build|compile)
        do_build
        ;;
    update)
        DEV_UPDATE=1 do_update
        ;;
    *)
        do_update
        do_build
        do_upload
        do_monitor
        ;;
esac
