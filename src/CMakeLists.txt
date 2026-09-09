idf_component_register(SRCS "main.cpp" "ble/TelephonyHid.cpp" "app/FirmwareSwitch.cpp"
                            "app/MuteApp.cpp" "ui/Renderer.cpp" "storage/Settings.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hid bt nvs_flash app_update esp_partition esp_driver_gpio esp_timer)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
# Observe the generated HID database without forking/modifying the installed IDF.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ble_gatts_create_attr_tab")

