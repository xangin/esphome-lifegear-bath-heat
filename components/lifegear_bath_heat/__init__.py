import json
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, number, select, switch
from esphome.core import CORE
from esphome.const import CONF_ID, __version__ as ESPHOME_VERSION

CODEOWNERS = ["@xangin"]
AUTO_LOAD = [
    "binary_sensor",
    "button",
    "number",
    "select",
    "sensor",
    "switch",
    "text_sensor",
]

DOMAIN = "lifegear_bath_heat"

lifegear_bath_heat_ns = cg.esphome_ns.namespace(DOMAIN)
LifegearBathHeat = lifegear_bath_heat_ns.class_("LifegearBathHeat", cg.Component)

# Entity classes live in the shared C++ header; their YAML schemas are split
# into the normal ESPHome platform modules below this package.
LifegearModeSelect = lifegear_bath_heat_ns.class_("LifegearModeSelect", select.Select)
LifegearFilterThresholdSelect = lifegear_bath_heat_ns.class_(
    "LifegearFilterThresholdSelect", select.Select, cg.Component
)
LifegearHeatTemperatureSelect = lifegear_bath_heat_ns.class_(
    "LifegearHeatTemperatureSelect", select.Select, cg.Component
)
LifegearModeTimerNumber = lifegear_bath_heat_ns.class_(
    "LifegearModeTimerNumber", number.Number, cg.Component
)
LifegearSwitch = lifegear_bath_heat_ns.class_("LifegearSwitch", switch.Switch)
LifegearButton = lifegear_bath_heat_ns.class_("LifegearButton", button.Button)

LifegearSwitchKind = lifegear_bath_heat_ns.enum("LifegearSwitchKind")
LifegearButtonKind = lifegear_bath_heat_ns.enum("LifegearButtonKind")

CONF_LIFEGEAR_BATH_HEAT_ID = "lifegear_bath_heat_id"
CONF_PANEL_RX_PIN = "panel_rx_pin"
CONF_PANEL_TX_PIN = "panel_tx_pin"
CONF_MACHINE_RX_PIN = "machine_rx_pin"
CONF_MACHINE_TX_PIN = "machine_tx_pin"
CONF_RMT_RX_PIN = "rmt_rx_pin"
CONF_DISPLAY_LANGUAGE = "display_language"
CONF_LEGACY_LANGUAGE = "language"
CONF_TIMER_DEFAULTS = "timer_defaults"

TIMER_KEYS = [
    "heat_bath",
    "heat_temp",
    "cool_high",
    "cool_low",
    "vent_high",
    "vent_low",
    "dry_eco",
    "dry_fast",
]
TIMER_CONF_KEYS = [f"{key}_timer" for key in TIMER_KEYS]

DEFAULT_TIMER_MINUTES = {
    "heat_bath": 60,
    "heat_temp": 60,
    "cool_high": 60,
    "cool_low": 60,
    "vent_high": 60,
    "vent_low": 180,
    "dry_eco": 180,
    "dry_fast": 60,
}

# Preserve the API keys used by the old template YAML and by the first native
# component release. Entity display names remain fully configurable in YAML.
OBJECT_IDS = {
    "mode": "operating_mode",
    "filter_threshold": "filter-threshold_______",
    "heat_temperature": "temp_______",
    "off": "off___",
    "filter_reset": "filter-reset_______",
    "return_to_panel": "panel_______",
    "left_light": "lightl___",
    "right_light": "lightr___",
    "buzzer": "buzzer_____",
    "vent24": "vent24_24h____",
    "filter_hours": "filter-hours_______",
    "timer_left": "timer-left_____",
    "host_status": "mmode_______",
    "current_mode": "mode_____",
    "raw_state": "raw____",
    "filter_due": "filter-due_______",
    "override": "override_ha___",
    "left_light_state": "lightl-state_____",
    "right_light_state": "lightr-state_____",
    "heat_bath_timer": "timer-heatbath_______",
    "heat_temp_timer": "timer-heattemp_______",
    "cool_high_timer": "timer-coolhi______",
    "cool_low_timer": "timer-coollo______",
    "vent_high_timer": "timer-venthi______",
    "vent_low_timer": "timer-ventlo______",
    "dry_eco_timer": "timer-dryeco_______",
    "dry_fast_timer": "timer-dryfast_______",
}

_TRANSLATION_DIR = Path(__file__).parent / "translations"


def load_translation(language):
    path = _TRANSLATION_DIR / f"{language}.json"
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def get_hub_config():
    return CORE.config[DOMAIN]


def get_display_translation():
    return load_translation(get_hub_config()[CONF_DISPLAY_LANGUAGE])


def get_timer_default(timer_key):
    return get_hub_config()[CONF_TIMER_DEFAULTS][timer_key]


def preserve_object_id(var, key):
    # ESPHome 2026.8 configures entity names and object-id hashes atomically in
    # App.register_*(). EntityBase::set_object_id() is no longer part of the
    # generated C++ API, so a post-registration override cannot be emitted.
    # Older ESPHome releases still support it, so retain their legacy HA object
    # IDs while letting 2026.8+ derive IDs from the YAML entity names.
    major, minor = (int(part) for part in ESPHOME_VERSION.split(".")[:2])
    if (major, minor) < (2026, 8):
        cg.add(var.set_object_id(OBJECT_IDS[key]))


def _normalize_legacy_language(config):
    config = dict(config)
    if CONF_LEGACY_LANGUAGE in config:
        if CONF_DISPLAY_LANGUAGE in config:
            raise cv.Invalid(
                "Use display_language only; language is the legacy option name"
            )
        config[CONF_DISPLAY_LANGUAGE] = config.pop(CONF_LEGACY_LANGUAGE)
    return config


def _validate_mode(config):
    panel_keys = [CONF_PANEL_RX_PIN, CONF_PANEL_TX_PIN, CONF_MACHINE_RX_PIN]
    present = [key for key in panel_keys if key in config]
    if present and len(present) != len(panel_keys):
        raise cv.Invalid(
            "panel_rx_pin/panel_tx_pin/machine_rx_pin must either all be set "
            "(relay mode) or all be omitted (standalone mode)"
        )
    return config


_PIN = cv.int_range(min=0, max=31)

CONFIG_SCHEMA = cv.All(
    _normalize_legacy_language,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LifegearBathHeat),
            cv.Optional(CONF_PANEL_RX_PIN): _PIN,
            cv.Optional(CONF_PANEL_TX_PIN): _PIN,
            cv.Optional(CONF_MACHINE_RX_PIN): _PIN,
            cv.Required(CONF_MACHINE_TX_PIN): _PIN,
            cv.Optional(CONF_RMT_RX_PIN): _PIN,
            cv.Optional(CONF_DISPLAY_LANGUAGE, default="zh-TW"): cv.one_of(
                "zh-TW", "en"
            ),
            cv.Optional(CONF_TIMER_DEFAULTS, default=DEFAULT_TIMER_MINUTES): cv.Schema(
                {
                    cv.Optional(key, default=value): cv.int_range(min=5, max=720)
                    for key, value in DEFAULT_TIMER_MINUTES.items()
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_mode,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_PANEL_RX_PIN in config:
        cg.add(var.set_panel_rx_pin(config[CONF_PANEL_RX_PIN]))
        cg.add(var.set_panel_tx_pin(config[CONF_PANEL_TX_PIN]))
        cg.add(var.set_machine_rx_pin(config[CONF_MACHINE_RX_PIN]))
    cg.add(var.set_machine_tx_pin(config[CONF_MACHINE_TX_PIN]))
    if CONF_RMT_RX_PIN in config:
        cg.add(var.set_rmt_rx_pin(config[CONF_RMT_RX_PIN]))

    translated = load_translation(config[CONF_DISPLAY_LANGUAGE])
    for key, value in translated["states"].items():
        cg.add(var.set_translation(key, value))

    # Timers are protocol behavior, not UI entities. Always initialize all
    # eight independent values even when their optional number entities are
    # omitted. A declared number can subsequently restore its NVS value.
    for index, timer_key in enumerate(TIMER_KEYS):
        cg.add(var.set_mode_timer(index, config[CONF_TIMER_DEFAULTS][timer_key]))
