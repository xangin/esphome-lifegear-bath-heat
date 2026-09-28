import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    TIMER_CONF_KEYS,
    TIMER_KEYS,
    LifegearBathHeat,
    LifegearModeTimerNumber,
    get_timer_default,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

TIMER_SCHEMA = number.number_schema(
    LifegearModeTimerNumber,
    icon="mdi:timer-outline",
    entity_category="config",
    unit_of_measurement="min",
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        **{cv.Optional(key): TIMER_SCHEMA for key in TIMER_CONF_KEYS},
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])
    for index, (timer_key, config_key) in enumerate(
        zip(TIMER_KEYS, TIMER_CONF_KEYS)
    ):
        if config_key not in config:
            continue
        conf = config[config_key]
        entity = await number.new_number(
            conf,
            parent,
            index,
            get_timer_default(timer_key),
            min_value=5,
            max_value=720,
            step=1,
        )
        await cg.register_component(entity, conf)
        preserve_object_id(entity, config_key)
        cg.add(parent.set_mode_timer_number(index, entity))
