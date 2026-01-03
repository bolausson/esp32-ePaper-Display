# Main component CMakeLists.txt

idf_component_register(
    SRCS "main.c" "epd_7in3e.c" "image_processor.c"
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
    REQUIRES driver esp_http_client nvs_flash led_strip
)
