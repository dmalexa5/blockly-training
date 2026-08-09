cmake_minimum_required(VERSION 3.16)

set(MODULE_TYPE "" CACHE STRING "Module type: button or rebounder")
set_property(CACHE MODULE_TYPE PROPERTY STRINGS button rebounder)

if(NOT MODULE_TYPE STREQUAL "button" AND NOT MODULE_TYPE STREQUAL "rebounder")
    message(FATAL_ERROR "MODULE_TYPE must be set to 'button' or 'rebounder'")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(module)
