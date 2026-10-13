cmake_minimum_required(VERSION 3.24)
project(aggregate_audio LANGUAGES CXX RC)

add_executable(catalog_inspect tools/catalog_inspect.cpp)
target_compile_features(catalog_inspect PRIVATE cxx_std_20)
target_compile_definitions(catalog_inspect PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(catalog_inspect PRIVATE /EHsc /utf-8 /W4 /permissive-)
target_link_libraries(catalog_inspect PRIVATE ole32 uuid propsys setupapi)

if(NOT WIN32)
  message(FATAL_ERROR "The aggregate audio driver is Windows-only.")
endif()

add_executable(aggregate_audio_diagnostics tools/driver_diagnostics.cpp)
target_include_directories(aggregate_audio_diagnostics PRIVATE src)
target_compile_features(aggregate_audio_diagnostics PRIVATE cxx_std_20)
target_compile_definitions(aggregate_audio_diagnostics PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(aggregate_audio_diagnostics PRIVATE /EHsc /utf-8 /W4 /permissive-)
set_target_properties(aggregate_audio_diagnostics PROPERTIES OUTPUT_NAME AggregateAudioDiagnostics)

add_executable(aggregate_audio_config WIN32
  assets/aggregate-audio.rc
  src/config_ui.cpp
  src/config_ui.h
  src/config_instance.h
  src/config_ui_theme.cpp
  src/config_ui_theme.h
  src/main.cpp
  src/device_catalog.cpp
  src/settings.cpp
  src/audio_engine.cpp
  src/ks_probe.cpp
)
target_compile_features(aggregate_audio_config PRIVATE cxx_std_20)
target_include_directories(aggregate_audio_config PRIVATE assets)
target_compile_definitions(aggregate_audio_config PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(aggregate_audio_config PRIVATE /EHsc /utf-8 /W4 /permissive-)
target_link_libraries(aggregate_audio_config PRIVATE ole32 uuid propsys shell32 avrt setupapi ksuser comctl32 gdiplus uxtheme dwmapi)
set_target_properties(aggregate_audio_config PROPERTIES OUTPUT_NAME AggregateAudioConfig)

set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio-sdk")
add_library(asio_register_helpers STATIC
  ${ASIO_SDK_DIR}/common/register.cpp
)
target_compile_options(asio_register_helpers PRIVATE
  /EHsc /Zc:strictStrings- /FIwindows.h /FIole2.h /FIobjbase.h /D_CRT_SECURE_NO_WARNINGS)

add_library(AggregateAudio SHARED
  src/asio_driver.cpp
  src/audio_engine.cpp
  src/device_catalog.cpp
  src/settings.cpp
  ${ASIO_SDK_DIR}/common/combase.cpp
  ${ASIO_SDK_DIR}/common/dllentry.cpp
  src/aggregate_audio.def
)
target_compile_features(AggregateAudio PRIVATE cxx_std_20)
target_compile_definitions(AggregateAudio PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(AggregateAudio PRIVATE /EHsc /utf-8 /W4 /permissive-)
target_include_directories(AggregateAudio PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${ASIO_SDK_DIR}/common
)
target_link_libraries(AggregateAudio PRIVATE ole32 uuid propsys shell32 avrt setupapi ksuser winmm asio_register_helpers)
set_source_files_properties(${ASIO_SDK_DIR}/common/combase.cpp PROPERTIES
  COMPILE_OPTIONS "/FIrpc.h;/FIrpcndr.h;/FIwindows.h;/FIole2.h;/FIbasetyps.h;/FIunknwn.h;/FIobjbase.h")
set_source_files_properties(${ASIO_SDK_DIR}/common/dllentry.cpp PROPERTIES
  COMPILE_OPTIONS "/FIrpc.h;/FIrpcndr.h;/FIwindows.h;/FIole2.h;/FIbasetyps.h;/FIunknwn.h;/FIobjbase.h")

include(CTest)
if(BUILD_TESTING)
  add_executable(device_recovery_test tests/device_recovery_test.cpp)
  target_include_directories(device_recovery_test PRIVATE src)
  target_link_libraries(device_recovery_test PRIVATE ole32 uuid propsys)
  add_executable(config_tray_test tests/config_tray_test.cpp assets/aggregate-audio.rc)
  target_include_directories(config_tray_test PRIVATE src assets)
  target_link_libraries(config_tray_test PRIVATE shell32)
  add_executable(settings_test tests/settings_test.cpp)
  target_include_directories(settings_test PRIVATE src)
  target_link_libraries(settings_test PRIVATE ole32 shell32)
  add_executable(config_ui_test tests/config_ui_test.cpp assets/aggregate-audio.rc src/config_ui_theme.cpp
    src/device_catalog.cpp src/settings.cpp src/audio_engine.cpp src/ks_probe.cpp)
  target_link_libraries(config_ui_test PRIVATE ole32 uuid propsys shell32 avrt setupapi
    ksuser comctl32 gdiplus uxtheme dwmapi)
  target_include_directories(config_ui_test PRIVATE assets)
  add_executable(asio_output_test tests/asio_output_test.cpp ${ASIO_SDK_DIR}/common/combase.cpp)
  target_include_directories(asio_output_test PRIVATE src ${ASIO_SDK_DIR}/common)
  target_link_libraries(asio_output_test PRIVATE ole32 uuid shell32 avrt advapi32 winmm)
  add_executable(ks_output_test tests/ks_output_test.cpp)
  target_include_directories(ks_output_test PRIVATE src)
  target_link_libraries(ks_output_test PRIVATE ole32 uuid avrt ksuser)
  add_executable(ks_capture_test tests/ks_capture_test.cpp)
  target_include_directories(ks_capture_test PRIVATE src)
  target_link_libraries(ks_capture_test PRIVATE ole32 uuid avrt ksuser)
  add_executable(device_busy_test tests/device_busy_test.cpp)
  target_include_directories(device_busy_test PRIVATE src)
  target_link_libraries(device_busy_test PRIVATE ole32 uuid avrt ksuser)
  add_executable(catalog_test tests/catalog_test.cpp)
  target_include_directories(catalog_test PRIVATE src)
  target_link_libraries(catalog_test PRIVATE ole32 uuid propsys setupapi)
  foreach(test_target IN ITEMS config_ui_test asio_output_test ks_output_test ks_capture_test device_busy_test catalog_test settings_test device_recovery_test config_tray_test)
    target_compile_features(${test_target} PRIVATE cxx_std_20)
    target_compile_definitions(${test_target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${test_target} PRIVATE /EHsc /utf-8 /W4 /permissive-)
    add_test(NAME ${test_target} COMMAND ${test_target})
  endforeach()
endif()
