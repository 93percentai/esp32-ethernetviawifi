cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../firmware/components/dns_server")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(korvo_wifi_ethernet)
