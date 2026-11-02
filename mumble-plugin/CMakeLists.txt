cmake_minimum_required(VERSION 3.15)
project(valheim_radio VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(valheim_radio SHARED src/plugin.cpp src/RadioFx.cpp src/Settings.cpp)
target_include_directories(valheim_radio PRIVATE include src)

if(WIN32)
	target_link_libraries(valheim_radio PRIVATE ws2_32)
	if(MSVC)
		# Don't depend on the VC++ redistributable being installed
		set_property(TARGET valheim_radio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	else()
		target_link_options(valheim_radio PRIVATE -static -static-libgcc -static-libstdc++)
	endif()
else()
	find_package(Threads REQUIRED)
	target_link_libraries(valheim_radio PRIVATE Threads::Threads)
endif()

# Offline listening test: renders a WAV with the effect swept from 0 m to 100 m.
add_executable(radio_demo tools/radio_demo.cpp src/RadioFx.cpp)
target_include_directories(radio_demo PRIVATE src)
