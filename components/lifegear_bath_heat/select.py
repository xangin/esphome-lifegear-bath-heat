import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    LifegearBathHeat,
    LifegearFilterThresholdSelect,
    LifegearHeatTemperatureSelect,
    LifegearModeSelect,
    get_display_translation,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

CONF_MODE = "mode"
CONF_FILTER_THRESHOLD = "filter_threshold"
CONF_HEAT_TEMPERATURE = "heat_temperature"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        cv.Optional(CONF_MODE): select.select_schema(
            LifegearModeSelect, icon="mdi:hvac"
        ),
        cv.Optional(CONF_FILTER_THRESHOLD): select.select_schema(
            LifegearFilterThresholdSelect,
            icon="mdi:air-filter",
            entity_category="config",
        ).extend(cv.COMPONENT_SCHEMA),
        cv.Optional(CONF_HEAT_TEMPERATURE): select.select_schema(
            LifegearHeatTemperatureSelect, icon="mdi:thermometer"
        ).extend(cv.COMPONENT_SCHEMA),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])

    if CONF_MODE in config:
        entity = await select.new_select(
            config[CONF_MODE], parent, options=get_display_translation()["mode_options"]
        )
        preserve_object_id(entity, CONF_MODE)
        cg.add(parent.set_mode_select(entity))

    if CONF_FILTER_THRESHOLD in config:
        conf = config[CONF_FILTER_THRESHOLD]
        entity = await select.new_select(
            conf, parent, options=["720", "1440", "2160"]
        )
        await cg.register_component(entity, conf)
        preserve_object_id(entity, CONF_FILTER_THRESHOLD)
        cg.add(parent.set_filter_threshold_select(entity))

    if CONF_HEAT_TEMPERATURE in config:
        conf = config[CONF_HEAT_TEMPERATURE]
        entity = await select.new_select(
            conf,
            parent,
            options=[str(value) for value in range(25, 36)],
        )
        await cg.register_component(entity, conf)
        preserve_object_id(entity, CONF_HEAT_TEMPERATURE)
        cg.add(parent.set_heat_temperature_select(entity))
