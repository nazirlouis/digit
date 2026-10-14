set(dashboard_asm "${CMAKE_CURRENT_BINARY_DIR}/dashboard_html.S")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/generate_reactions.py" --check
                RESULT_VARIABLE reaction_catalog_result)
if(NOT reaction_catalog_result EQUAL 0)
    message(FATAL_ERROR "Reaction catalog is stale; run tools/generate_reactions.py before building")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/../../reactions/digit_reactions.json"
             "${CMAKE_CURRENT_LIST_DIR}/../../tools/generate_reactions.py")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/pack_dashboard.py"
                "${CMAKE_CURRENT_LIST_DIR}/web_server.cpp" "${CMAKE_CURRENT_BINARY_DIR}"
                RESULT_VARIABLE dashboard_pack_result)
if(NOT dashboard_pack_result EQUAL 0)
    message(FATAL_ERROR "Dashboard packing failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/web_server.cpp"
             "${CMAKE_CURRENT_LIST_DIR}/../tools/pack_dashboard.py")
endif()
idf_component_register(SRCS "main.cpp" "idf_adapters.cpp" "audio_engine.cpp" "emotion_voice.cpp"

                            "robot_actions.cpp" "robot_session.cpp" "action_console.cpp" "reaction_player.cpp" "personality_events.cpp"
                            "wake_word.cpp" "voice_direction.cpp" "local_stop.cpp" "digit_model.S" "stop_model.S"
                            "gemini_client.cpp" "gemini_live.cpp"
                            "camera_service.cpp" "zumo_link.cpp" "vision_service.cpp" "battery_detector.cpp"


                            "web_server.cpp" "${dashboard_asm}"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_http_server
                                json esp-tls tcp_transport mbedtls
                                esp32-camera pedestrian_detect esp_new_jpeg driver esp_timer
                                esp_partition esp_driver_uart esp-tflite-micro esp-micro-speech-features)
set_source_files_properties("${dashboard_asm}" PROPERTIES OBJECT_DEPENDS
                           "${CMAKE_CURRENT_BINARY_DIR}/dashboard.html.gz")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DIGIT_MODEL_FILE="${CMAKE_CURRENT_LIST_DIR}/../wake_word_hey_digit/hey_digit.tflite"
    STOP_MODEL_FILE="${CMAKE_CURRENT_LIST_DIR}/../wake_word_stop/stop.tflite")
set_source_files_properties(digit_model.S PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../wake_word_hey_digit/hey_digit.tflite")
set_source_files_properties(stop_model.S PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../wake_word_stop/stop.tflite")
