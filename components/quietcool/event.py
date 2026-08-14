import esphome.codegen as cg
from esphome.components import event
import esphome.config_validation as cv

from . import QuietCoolComponent, quietcool_ns


CONF_CONTROLLER_ID = "controller_id"

QuietCoolStartRefusedEvent = quietcool_ns.class_(
    "QuietCoolStartRefusedEvent", cg.Component, event.Event
)

# One fixed event type today. Declared as a list because ESPHome's event
# entity always carries a whole vocabulary via set_event_types() — a future
# refusal reason (e.g. a degraded controller) could extend this list without
# a schema change, so it is kept as a list rather than a single constant.
EVENT_TYPE_NO_PERMISSION = "no_permission"

CONFIG_SCHEMA = (
    event.event_schema(QuietCoolStartRefusedEvent)
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
            '#include "quietcool/esphome/quietcool_permission_event.h"'
        )
    )
    var = await event.new_event(config, event_types=[EVENT_TYPE_NO_PERMISSION])
    await cg.register_component(var, config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(controller.set_start_refused_event(var))
