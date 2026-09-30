idf_component_register(SRCS "main.cpp" "idf_adapters.cpp" "audio_engine.cpp"
                            "camera_service.cpp" "zumo_link.cpp" "vision_service.cpp"
                            "person_follow.cpp"
                            "wake_word.cpp" "voice_direction.cpp" "digit_model.S"
                            "web_server.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_http_server
                                esp32-camera pedestrian_detect esp_new_jpeg driver esp_timer
                                esp_partition esp_driver_uart esp-tflite-micro
                                esp-micro-speech-features)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DIGIT_MODEL_FILE="${CMAKE_CURRENT_LIST_DIR}/../wake_word_digit/digit.tflite")
set_source_files_properties(digit_model.S PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../wake_word_digit/digit.tflite")
