import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv

from . import QuietCoolComponent, quietcool_ns


CONF_CONTROLLER_ID = "controller_id"

QuietCoolPermissionSwitch = quietcool_ns.class_(
    "QuietCoolPermissionSwitch", cg.Component, switch.Switch
)


CONFIG_SCHEMA = (
    switch.switch_schema(
        QuietCoolPermissionSwitch,
        # ALWAYS_OFF, not a persistent default: item 1 of the
        # permission-to-start design requires the flag to start false on
        # EVERY boot, including a graceful reboot where flash holds a
        # previous "on". A restore mode that could ever load a persisted
        # true would let a stale grant survive a reboot the operator meant
        # to be a fresh safety check. The controller's own
        # permission_to_start_ field mirrors this default independently
        # (quietcool_component.h), so the internal flag and this entity can
        # never disagree about what a boot means.
        default_restore_mode="ALWAYS_OFF",
        icon="mdi:fan-alert",
    )
    .extend(
        {
            cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    # Subdirectory headers are not auto-included; declare the entity class.
    cg.add_global(
        cg.RawStatement(
            '#include "quietcool/esphome/quietcool_permission_switch.h"'
        )
    )
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(var.set_controller(controller))
    cg.add(controller.set_permission_switch(var))
