# Sets I2C_BUS_GUARD_REQUIRES to `i2c_bus_guard` on a build that has the bug it patches, and to
# nothing otherwise, so the kit's own probing components (cmd_i2c, servo) require it there and
# only there. The conditions are the #errors' in i2c_bus_guard.c -- IDF 6.1.0, and a target
# whose I2C has no independent STOP (every one but the original ESP32) -- so a build that never
# asked for the guard can never reach them; keep the two in step. And only when the component
# is registered at all: a project that names the kit's directories one by one gets nothing
# rather than an unresolvable requirement. (That reads IDF's internal __COMPONENT_TARGETS and
# its ___<prefix>_<name> targets, so a firmware that must have the guard lists it itself.)
# Included before idf_component_register(), so it runs in IDF's requirements pass too, which
# has the IDF_VERSION_* variables and these build properties.
set(_i2c_bus_guard_idf "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
idf_build_get_property(_i2c_bus_guard_target IDF_TARGET)
idf_build_get_property(_i2c_bus_guard_components __COMPONENT_TARGETS)
list(FILTER _i2c_bus_guard_components INCLUDE REGEX "^___([^_]+_)?i2c_bus_guard$")
if(_i2c_bus_guard_idf VERSION_EQUAL 6.1.0 AND NOT _i2c_bus_guard_target STREQUAL "esp32"
   AND _i2c_bus_guard_components)
    set(I2C_BUS_GUARD_REQUIRES i2c_bus_guard)
else()
    set(I2C_BUS_GUARD_REQUIRES "")
    # A newer IDF may still carry the same driver: say so, once, rather than guard it blind or
    # leave it out unremarked. From the real pass: the requirements pass's output is shown
    # only when it fails.
    get_property(_i2c_bus_guard_warned GLOBAL PROPERTY I2C_BUS_GUARD_WARNED)
    if(_i2c_bus_guard_idf VERSION_GREATER 6.1.0 AND NOT _i2c_bus_guard_target STREQUAL "esp32"
       AND NOT CMAKE_SCRIPT_MODE_FILE AND NOT _i2c_bus_guard_warned)
        set_property(GLOBAL PROPERTY I2C_BUS_GUARD_WARNED TRUE)
        message(WARNING "IDF ${_i2c_bus_guard_idf}: esp-console-kit's i2c_bus_guard was verified "
                "against 6.1.0 only, so cmd_i2c and servo leave it out. Check whether "
                "i2c_master_probe() still writes through a stale pointer after a timed-out read, "
                "then update esp-console-kit/i2c_bus_guard.")
    endif()
endif()
unset(_i2c_bus_guard_idf)
unset(_i2c_bus_guard_target)
unset(_i2c_bus_guard_components)
unset(_i2c_bus_guard_warned)
