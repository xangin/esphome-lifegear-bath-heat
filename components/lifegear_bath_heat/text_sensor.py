import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    LifegearBathHeat,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

CONF_HOST_STATUS = "host_status"
CONF_CURRENT_MODE = "current_mode"
CONF_RAW_STATE = "raw_state"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        cv.Optional(CONF_HOST_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:engine-outline"
        ),
        cv.Optional(CONF_CURRENT_MODE): text_sensor.text_sensor_schema(
            icon="mdi:information-outline"
        ),
        cv.Optional(CONF_RAW_STATE): text_sensor.text_sensor_schema(
            icon="mdi:code-braces", entity_category="diagnostic"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])
    setters = {
        CONF_HOST_STATUS: parent.set_host_status_text_sensor,
        CONF_CURRENT_MODE: parent.set_current_mode_text_sensor,
        CONF_RAW_STATE: parent.set_raw_state_text_sensor,
    }
    for key, setter in setters.items():
        if key not in config:
            continue
        entity = await text_sensor.new_text_sensor(config[key])
        preserve_object_id(entity, key)
        cg.add(setter(entity))
