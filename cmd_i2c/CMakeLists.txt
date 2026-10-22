# i2cdetect probes every address on a bus that something else may be reading: on IDF 6.1.0
# that needs the kit's i2c_bus_guard (see its README), pulled in here so no user can miss it.
# OPTIONAL: a copy of this component alone (the component manager's `path:`) has no sibling.
include(${CMAKE_CURRENT_LIST_DIR}/../i2c_bus_guard/wanted.cmake OPTIONAL)

idf_component_register(SRCS "cmd_i2ctools.c"
                    INCLUDE_DIRS .
                    # IDF 6.x split the monolithic `driver` component: driver/i2c_master.h
                    # now comes from esp_driver_i2c, and `driver` carries only the legacy
                    # i2c/touch/twai drivers. argtable3 and esp_console come from `console`.
                    PRIV_REQUIRES console esp_driver_i2c ${I2C_BUS_GUARD_REQUIRES})
