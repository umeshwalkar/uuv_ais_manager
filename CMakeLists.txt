cmake_minimum_required(VERSION 3.16)
project(ais_manager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# nlohmann/json — prefer installed, fall back to FetchContent
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Mosquitto C client (libmosquitto-dev)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

set(SOURCES
    src/main.cpp
    src/Config.cpp
    src/Logger.cpp
    src/Transport.cpp
    src/MqttClient.cpp
    src/AisDevice.cpp
    src/AisManager.cpp
)

add_executable(ais_manager ${SOURCES})
target_include_directories(ais_manager PRIVATE src ${MOSQUITTO_INCLUDE_DIRS})
target_link_libraries(ais_manager PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    ${MOSQUITTO_LIBRARIES}
)

install(TARGETS ais_manager DESTINATION bin)
install(FILES config/ais_config.json DESTINATION etc/ais_manager)
