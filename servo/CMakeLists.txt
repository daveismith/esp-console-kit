set(srcs
    "src/Servo.cpp"
    "src/ServoManager.cpp"
    "src/Esp32PwmServo.cpp"
    "src/PCA9685ServoController.cpp"
    "src/PCA9685Servo.cpp"
    "src/servo_api.cpp"
    "src/cmd_servo.cpp"
)

# A PCA9685 board's attach probes the bus: on IDF 6.1.0 that needs the kit's i2c_bus_guard
# (see its README), pulled in here so no user can miss it.
# OPTIONAL: a copy of this component alone (the component manager's `path:`) has no sibling.
include(${CMAKE_CURRENT_LIST_DIR}/../i2c_bus_guard/wanted.cmake OPTIONAL)

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "include"
                    # IDF 6.x split the monolithic `driver` component: driver/i2c_master.h
                    # now comes from esp_driver_i2c and driver/mcpwm_*.h from
                    # esp_driver_mcpwm, and `driver` itself carries only the legacy
                    # i2c/touch/twai drivers. Both are public because their handle types
                    # appear in this component's headers.
                    REQUIRES esp_driver_i2c esp_driver_mcpwm
                    PRIV_REQUIRES console nvs_flash ${I2C_BUS_GUARD_REQUIRES})
