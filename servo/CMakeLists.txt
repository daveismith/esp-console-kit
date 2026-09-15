set(srcs
    "src/Servo.cpp"
    "src/ServoManager.cpp"
    "src/Esp32PwmServo.cpp"
    "src/PCA9685ServoController.cpp"
    "src/PCA9685Servo.cpp"
    "src/servo_api.cpp"
    "src/cmd_servo.cpp"
)

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "include"
                    # IDF 6.x split the monolithic `driver` component: driver/i2c_master.h
                    # now comes from esp_driver_i2c and driver/mcpwm_*.h from
                    # esp_driver_mcpwm, and `driver` itself carries only the legacy
                    # i2c/touch/twai drivers. Both are public because their handle types
                    # appear in this component's headers.
                    REQUIRES esp_driver_i2c esp_driver_mcpwm
                    PRIV_REQUIRES console nvs_flash)
