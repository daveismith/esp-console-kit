idf_component_register(SRCS "i2c_bus_guard.c"
                    INCLUDE_DIRS "include"
                    # i2c_private.h includes the ring buffer and power management headers.
                    PRIV_REQUIRES esp_driver_i2c esp_ringbuf esp_pm
                    # Linked whole: the __wrap_ references come from libraries anywhere on
                    # the link line, and an archive scanned before them would not be
                    # searched again for the definitions.
                    WHOLE_ARCHIVE)

# The patch reads the driver's private bus struct, so it needs the driver's own directory,
# which holds i2c_private.h.
idf_component_get_property(i2c_dir esp_driver_i2c COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${i2c_dir}")

# Every call in the image goes through the guard -- the application's, IDF's touch and sensor
# drivers', the kit's servo attach and i2cdetect -- so requiring this component is all it
# takes. INTERFACE, so the options reach the final link.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=i2c_master_probe"
                      "-Wl,--wrap=i2c_master_receive"
                      "-Wl,--wrap=i2c_master_transmit_receive"
                      "-Wl,--wrap=i2c_master_execute_defined_operations")
