import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    LifegearBathHeat,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

CONF_FILTER_DUE = "filter_due"
CONF_OVERRIDE = "override"
CONF_LEFT_LIGHT_STATE = "left_light_state"
CONF_RIGHT_LIGHT_STATE = "right_light_state"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        cv.Optional(CONF_FILTER_DUE): binary_sensor.binary_sensor_schema(
            icon="mdi:air-filter", device_class="problem"
        ),
        cv.Optional(CONF_OVERRIDE): binary_sensor.binary_sensor_schema(
            icon="mdi:remote"
        ),
        cv.Optional(CONF_LEFT_LIGHT_STATE): binary_sensor.binary_sensor_schema(
            icon="mdi:lightbulb-on-outline", entity_category="diagnostic"
        ),
        cv.Optional(CONF_RIGHT_LIGHT_STATE): binary_sensor.binary_sensor_schema(
            icon="mdi:lightbulb-on-outline", entity_category="diagnostic"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])
    setters = {
        CONF_FILTER_DUE: parent.set_filter_due_binary_sensor,
        CONF_OVERRIDE: parent.set_override_binary_sensor,
        CONF_LEFT_LIGHT_STATE: parent.set_left_light_state_binary_sensor,
        CONF_RIGHT_LIGHT_STATE: parent.set_right_light_state_binary_sensor,
    }
    for key, setter in setters.items():
        if key not in config:
            continue
        entity = await binary_sensor.new_binary_sensor(config[key])
        preserve_object_id(entity, key)
        cg.add(setter(entity))
