/* Dongle: reenvia a las mitades el nivel de bateria de cada una. */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#include <string.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_peripheral_battery_state_changed, pb, );
