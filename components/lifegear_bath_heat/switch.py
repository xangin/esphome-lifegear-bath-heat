import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    LifegearBathHeat,
    LifegearSwitch,
    LifegearSwitchKind,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

CONF_LEFT_LIGHT = "left_light"
CONF_RIGHT_LIGHT = "right_light"
CONF_BUZZER = "buzzer"
CONF_VENT24 = "vent24"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        cv.Optional(CONF_LEFT_LIGHT): switch.switch_schema(
            LifegearSwitch,
            icon="mdi:lightbulb",
            default_restore_mode="ALWAYS_OFF",
        ),
        cv.Optional(CONF_RIGHT_LIGHT): switch.switch_schema(
            LifegearSwitch,
            icon="mdi:lightbulb-outline",
            default_restore_mode="ALWAYS_OFF",
        ),
        cv.Optional(CONF_BUZZER): switch.switch_schema(
            LifegearSwitch,
            icon="mdi:bell-ring",
            default_restore_mode="RESTORE_DEFAULT_ON",
        ),
        cv.Optional(CONF_VENT24): switch.switch_schema(
            LifegearSwitch,
            icon="mdi:fan-clock",
            default_restore_mode="RESTORE_DEFAULT_ON",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])
    kinds = {
        CONF_LEFT_LIGHT: LifegearSwitchKind.SWITCH_LEFT_LIGHT,
        CONF_RIGHT_LIGHT: LifegearSwitchKind.SWITCH_RIGHT_LIGHT,
        CONF_BUZZER: LifegearSwitchKind.SWITCH_BUZZER,
        CONF_VENT24: LifegearSwitchKind.SWITCH_VENT24,
    }
    setters = {
        CONF_LEFT_LIGHT: parent.set_left_light_switch,
        CONF_RIGHT_LIGHT: parent.set_right_light_switch,
        CONF_BUZZER: parent.set_buzzer_switch,
        CONF_VENT24: parent.set_vent24_switch,
    }
    for key, kind in kinds.items():
        if key not in config:
            continue
        entity = await switch.new_switch(config[key], parent, kind)
        preserve_object_id(entity, key)
        cg.add(setters[key](entity))
