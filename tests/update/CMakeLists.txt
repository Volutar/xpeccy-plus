# The update check's logic (src/xcore/update.cpp) over saved GitHub answers. Qt Core only:
#
#	cmake -S tests/update -B build/out/tests-update -DQTVERSION=6 -DCMAKE_PREFIX_PATH=<Qt prefix>
#	cmake --build build/out/tests-update
#	ctest --test-dir build/out/tests-update --output-on-failure

cmake_minimum_required(VERSION 3.13...4.1)
project(update-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(QTVERSION 6 CACHE STRING "Qt major version")
find_package(Qt${QTVERSION} REQUIRED COMPONENTS Core)

add_executable(update-check main.cpp ../../src/xcore/update.cpp)
target_include_directories(update-check PRIVATE ../../src/xcore)
target_link_libraries(update-check PRIVATE Qt${QTVERSION}::Core)
if(MINGW)
	target_link_options(update-check PRIVATE -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_test(NAME update-check COMMAND update-check ${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_SOURCE_DIR}/../../CHANGELOG.md)
