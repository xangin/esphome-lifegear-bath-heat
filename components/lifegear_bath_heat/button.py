import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import (
    CONF_LIFEGEAR_BATH_HEAT_ID,
    LifegearBathHeat,
    LifegearButton,
    LifegearButtonKind,
    preserve_object_id,
)

DEPENDENCIES = ["lifegear_bath_heat"]

CONF_OFF = "off"
CONF_FILTER_RESET = "filter_reset"
CONF_RETURN_TO_PANEL = "return_to_panel"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LIFEGEAR_BATH_HEAT_ID): cv.use_id(LifegearBathHeat),
        cv.Optional(CONF_OFF): button.button_schema(
            LifegearButton, icon="mdi:power"
        ),
        cv.Optional(CONF_FILTER_RESET): button.button_schema(
            LifegearButton, icon="mdi:air-filter"
        ),
        cv.Optional(CONF_RETURN_TO_PANEL): button.button_schema(
            LifegearButton, icon="mdi:gesture-tap-button"
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LIFEGEAR_BATH_HEAT_ID])
    kinds = {
        CONF_OFF: LifegearButtonKind.BUTTON_OFF,
        CONF_FILTER_RESET: LifegearButtonKind.BUTTON_FILTER_RESET,
        CONF_RETURN_TO_PANEL: LifegearButtonKind.BUTTON_RETURN_TO_PANEL,
    }
    for key, kind in kinds.items():
        if key not in config:
            continue
        entity = await button.new_button(config[key], parent, kind)
        preserve_object_id(entity, key)
