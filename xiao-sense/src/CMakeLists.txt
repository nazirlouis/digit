set(cue_asm "${CMAKE_CURRENT_BINARY_DIR}/audio_cues.S")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/pack_cues.py"
                "${CMAKE_CURRENT_LIST_DIR}/../audio_cues" "${CMAKE_CURRENT_BINARY_DIR}/packed_cues"
                RESULT_VARIABLE cue_pack_result)
if(NOT cue_pack_result EQUAL 0)
    message(FATAL_ERROR "Audio cue packing failed")
endif()
file(WRITE "${cue_asm}" ".section .rodata\n")
foreach(cue hello following lost countdown photo clear obstacle person explore)
    file(APPEND "${cue_asm}" ".balign 4\n.global cue_${cue}_start\ncue_${cue}_start:\n.incbin \"${CMAKE_CURRENT_BINARY_DIR}/packed_cues/${cue}.adpcm\"\n.global cue_${cue}_end\ncue_${cue}_end:\n")
    list(APPEND cue_files "${CMAKE_CURRENT_LIST_DIR}/../audio_cues/${cue}.wav")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/../tools/pack_cues.py" ${cue_files})
endif()
idf_component_register(SRCS "main.cpp" "idf_adapters.cpp" "audio_engine.cpp"
                            "audio_cues.cpp" "emotion_voice.cpp" "${cue_asm}"
                            "robot_actions.cpp" "action_mission.cpp" "photo_store.cpp" "speech_commands.cpp"
                            "camera_service.cpp" "zumo_link.cpp" "vision_service.cpp" "battery_detector.cpp"
                            "person_follow.cpp"
                            "wake_word.cpp" "voice_direction.cpp" "digit_model.S"
                            "web_server.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_http_server
                                esp32-camera pedestrian_detect esp_new_jpeg driver esp_timer
                                esp_partition esp_driver_uart esp-tflite-micro
                                esp-micro-speech-features esp-sr)
set_source_files_properties("${cue_asm}" PROPERTIES OBJECT_DEPENDS "${cue_files}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DIGIT_MODEL_FILE="${CMAKE_CURRENT_LIST_DIR}/../wake_word_digit/digit.tflite")
set_source_files_properties(digit_model.S PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../wake_word_digit/digit.tflite")
