"""Climate platform for TCL AC."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart
from esphome.const import CONF_ID

CODEOWNERS = ["@Kannix2005"]
DEPENDENCIES = ["uart"]

# Import from parent component
from . import tcl_ac_ns, TclAcClimate, CONF_BEEPER, CONF_DISPLAY, CONF_VERTICAL_DIRECTION, CONF_HORIZONTAL_DIRECTION

# Climate platform schema
CONFIG_SCHEMA = climate._CLIMATE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(TclAcClimate),
        cv.Optional(CONF_BEEPER, default=True): cv.boolean,
        cv.Optional(CONF_DISPLAY, default=False): cv.boolean,
        cv.Optional(CONF_VERTICAL_DIRECTION, default="max_down"): cv.one_of(
            "max_up", "up", "center", "down", "max_down", "swing", lower=True
        ),
        cv.Optional(CONF_HORIZONTAL_DIRECTION, default="max_right"): cv.one_of(
            "max_left", "left", "center", "right", "max_right", "swing", lower=True
        ),
    }
).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)


async def to_code(config):
    """Generate C++ code for the climate platform."""
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)
    await uart.register_uart_device(var, config)

    # Set configuration options
    cg.add(var.set_beeper_enabled(config[CONF_BEEPER]))
    cg.add(var.set_display_enabled(config[CONF_DISPLAY]))
    
    # Vertical direction mapping
    vertical_map = {
        "max_up": 1,
        "up": 2,
        "center": 3,
        "down": 4,
        "max_down": 5,
        "swing": 255,
    }
    cg.add(var.set_vertical_direction(vertical_map[config[CONF_VERTICAL_DIRECTION]]))
    
    # Horizontal direction mapping
    horizontal_map = {
        "max_left": 1,
        "left": 2,
        "center": 3,
        "right": 4,
        "max_right": 5,
        "swing": 255,
    }
    cg.add(var.set_horizontal_direction(horizontal_map[config[CONF_HORIZONTAL_DIRECTION]]))
