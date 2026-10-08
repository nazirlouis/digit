# Backport only the observed synchronous NACK hang. The original SDK is read
# without modification, and the exact-block check rejects an unreviewed SDK.
idf_component_get_property(digit_i2c_lib esp_driver_i2c COMPONENT_LIB)
idf_component_get_property(digit_i2c_dir esp_driver_i2c COMPONENT_DIR)
set(digit_i2c_original "${digit_i2c_dir}/i2c_master.c")
set(digit_i2c_patched "${CMAKE_BINARY_DIR}/digit_patches/i2c_master.c")
set(digit_i2c_patch_script "${CMAKE_CURRENT_LIST_DIR}/../tools/patch_i2c_timeout.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${digit_i2c_original}" "${digit_i2c_patch_script}")
execute_process(COMMAND "${PYTHON}" "${digit_i2c_patch_script}"
                "${digit_i2c_original}" "${digit_i2c_patched}"
                RESULT_VARIABLE digit_i2c_patch_result)
if(NOT digit_i2c_patch_result EQUAL 0)
    message(FATAL_ERROR "The build-local I2C timeout backport failed; inspect the SDK change")
endif()
get_target_property(digit_i2c_sources ${digit_i2c_lib} SOURCES)
list(FIND digit_i2c_sources "${digit_i2c_original}" digit_i2c_source_index)
if(digit_i2c_source_index EQUAL -1)
    message(FATAL_ERROR "Original I2C master source is not in its expected component")
endif()
list(REMOVE_ITEM digit_i2c_sources "${digit_i2c_original}")
list(APPEND digit_i2c_sources "${digit_i2c_patched}")
set_property(TARGET ${digit_i2c_lib} PROPERTY SOURCES "${digit_i2c_sources}")
# The generated source still uses the SDK's private, same-version headers.
target_include_directories(${digit_i2c_lib} PRIVATE "${digit_i2c_dir}")
