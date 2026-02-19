cmake_minimum_required(VERSION 3.16)

# Protokoll-Header dem Include-Pfad hinzufuegen
set(EXTRA_COMPONENT_DIRS "../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_client)
