idf_component_register(SRCS "main.cpp" "idf_adapters.cpp" "audio_engine.cpp"
                            "camera_service.cpp" "zumo_link.cpp" "vision_service.cpp"
                            "person_follow.cpp"
                            "web_server.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_http_server
                                esp32-camera pedestrian_detect esp_new_jpeg driver esp_timer
                                esp_partition esp_driver_uart)
