import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_PIN,
    CONF_TRIGGER_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_TIMER,
    STATE_CLASS_MEASUREMENT,
    UNIT_MILLISECOND,
)

CODEOWNERS = []
AUTO_LOAD = ["sensor", "text_sensor"]
MULTI_CONF = True

CONF_FRAME_GAP = "frame_gap"
CONF_GLITCH_FILTER = "glitch_filter"
CONF_STATUS_TIMEOUT = "status_timeout"
CONF_BIT_TIME = "bit_time"
CONF_FRAME_BITS = "frame_bits"
CONF_AUTO_BIT_TIME = "auto_bit_time"
CONF_MEASURED_BIT_TIME = "measured_bit_time"
CONF_FRAME = "frame"
CONF_FRAME_BINARY = "frame_binary"
CONF_FRAME_US = "frame_us"
CONF_STATUS = "status"
CONF_FRAME_INTERVAL = "frame_interval"
CONF_ON_FRAME = "on_frame"

parkside_bus_ns = cg.esphome_ns.namespace("parkside_bus")
ParksideBus = parkside_bus_ns.class_("ParksideBus", cg.Component)
FrameTrigger = parkside_bus_ns.class_(
    "FrameTrigger", automation.Trigger.template(cg.std_vector.template(cg.uint8))
)


def _validate_frame_bits(value):
    value = cv.int_range(min=8, max=64)(value)
    if value % 8:
        raise cv.Invalid("frame_bits musi być wielokrotnością 8")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ParksideBus),
        cv.Required(CONF_PIN): pins.internal_gpio_input_pin_schema,
        # cisza na linii, po której ramka uznawana jest za zakończoną
        cv.Optional(
            CONF_FRAME_GAP, default="100ms"
        ): cv.positive_time_period_microseconds,
        # zbocza bliżej niż to są traktowane jako szpilka i usuwane parami
        cv.Optional(
            CONF_GLITCH_FILTER, default="200us"
        ): cv.positive_time_period_microseconds,
        # brak ramek dłużej niż to -> status idle_high / line_low
        cv.Optional(
            CONF_STATUS_TIMEOUT, default="1s"
        ): cv.positive_time_period_milliseconds,
        # nominalny czas bitu (1 ms wg Aifryz/hackaday; zmierzone 0,96-1,005 ms)
        cv.Optional(CONF_BIT_TIME, default="1ms"): cv.positive_time_period_microseconds,
        # dopasuj czas bitu do zegara konkretnej baterii (estymata z każdej ramki)
        cv.Optional(CONF_AUTO_BIT_TIME, default=True): cv.boolean,
        # długość ramki w bitach (40 = 5 bajtów, MSB first)
        cv.Optional(CONF_FRAME_BITS, default=40): _validate_frame_bits,
        cv.Optional(CONF_FRAME): text_sensor.text_sensor_schema(icon="mdi:code-array"),
        cv.Optional(CONF_FRAME_BINARY): text_sensor.text_sensor_schema(
            icon="mdi:numeric-1-box-outline"
        ),
        # debug: surowe czasy odcinków w us
        cv.Optional(CONF_FRAME_US): text_sensor.text_sensor_schema(
            icon="mdi:timer-outline"
        ),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:battery-charging"
        ),
        cv.Optional(CONF_FRAME_INTERVAL): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLISECOND,
            icon=ICON_TIMER,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MEASURED_BIT_TIME): sensor.sensor_schema(
            unit_of_measurement="µs",
            icon=ICON_TIMER,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_ON_FRAME): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FrameTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
    cg.add(var.set_frame_gap(config[CONF_FRAME_GAP]))
    cg.add(var.set_glitch_filter(config[CONF_GLITCH_FILTER]))
    cg.add(var.set_status_timeout(config[CONF_STATUS_TIMEOUT]))
    cg.add(var.set_bit_time(config[CONF_BIT_TIME]))
    cg.add(var.set_frame_bits(config[CONF_FRAME_BITS]))
    cg.add(var.set_auto_bit_time(config[CONF_AUTO_BIT_TIME]))

    if CONF_FRAME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FRAME])
        cg.add(var.set_frame_text_sensor(sens))
    if CONF_FRAME_BINARY in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FRAME_BINARY])
        cg.add(var.set_frame_binary_text_sensor(sens))
    if CONF_FRAME_US in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FRAME_US])
        cg.add(var.set_frame_us_text_sensor(sens))
    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_text_sensor(sens))
    if CONF_FRAME_INTERVAL in config:
        sens = await sensor.new_sensor(config[CONF_FRAME_INTERVAL])
        cg.add(var.set_frame_interval_sensor(sens))
    if CONF_MEASURED_BIT_TIME in config:
        sens = await sensor.new_sensor(config[CONF_MEASURED_BIT_TIME])
        cg.add(var.set_measured_bit_time_sensor(sens))

    for conf in config.get(CONF_ON_FRAME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_vector.template(cg.uint8), "x")], conf
        )
