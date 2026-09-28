import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    LifegearBathHeat,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

CONF_FILTER_HOURS = "filter_hours"
CONF_TIMER_LEFT = "timer_left"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        cv.Optional(CONF_FILTER_HOURS): sensor.sensor_schema(
            unit_of_measurement="h",
            icon="mdi:air-filter",
            accuracy_decimals=1,
            state_class="total_increasing",
        ),
        cv.Optional(CONF_TIMER_LEFT): sensor.sensor_schema(
            unit_of_measurement="min",
            icon="mdi:timer-sand",
            accuracy_decimals=0,
            device_class="duration",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])
    setters = {
        CONF_FILTER_HOURS: parent.set_filter_hours_sensor,
        CONF_TIMER_LEFT: parent.set_timer_left_sensor,
    }
    for key, setter in setters.items():
        if key not in config:
            continue
        entity = await sensor.new_sensor(config[key])
        preserve_object_id(entity, key)
        cg.add(setter(entity))
