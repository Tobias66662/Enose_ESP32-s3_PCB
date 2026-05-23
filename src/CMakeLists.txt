# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(EI_LIB_DIR "${CMAKE_SOURCE_DIR}/lib/thesis_enose-cpp-mcu-v10-impulse-#1")

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
FILE(GLOB_RECURSE ei_sources
    "${EI_LIB_DIR}/tflite-model/tflite_learn_934763_5_compiled.cpp"
    "${EI_LIB_DIR}/edge-impulse-sdk/classifier/ei_run_classifier_c.cpp"
    "${EI_LIB_DIR}/edge-impulse-sdk/dsp/memory.cpp"
    "${EI_LIB_DIR}/edge-impulse-sdk/tensorflow/lite/c/common.c"
    "${EI_LIB_DIR}/edge-impulse-sdk/porting/espressif/*.cpp"
)

idf_component_register(
    SRCS ${app_sources} ${ei_sources}
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include" "${EI_LIB_DIR}" "${EI_LIB_DIR}/edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
)
