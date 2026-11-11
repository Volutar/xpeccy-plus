# The quick test tier: libxpeccy as a static library with no Qt and no SDL, and the
# headless benches linked against it. A project of its own, so the app's build never
# sees it:
#
#	cmake -S tests -B build/out/tests
#	cmake --build build/out/tests
#
# tests/regress.py quick does both and runs the benches.

cmake_minimum_required(VERSION 3.13...4.1)
project(xpeccy-tests C CXX)

set(XROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(XLIB ${XROOT}/src/libxpeccy)

include(TestBigEndian)
test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
	set(XENDIAN WORDS_BIG_ENDIAN)
else()
	set(XENDIAN WORDS_LITTLE_ENDIAN)
endif()

# the app's own flags, so the core is compiled as it ships
set(CMAKE_C_FLAGS "-std=gnu99 -Wall -O2 ${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "-std=c++14 -Wall -O2 ${CMAKE_CXX_FLAGS}")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# CONFIGURE_DEPENDS: a build tree made before a core file was added picks it up
file(GLOB_RECURSE XLIB_SOURCES CONFIGURE_DEPENDS ${XLIB}/*.c ${XLIB}/*.cpp)
# included by lh1_decoder.c, not a translation unit of its own
list(REMOVE_ITEM XLIB_SOURCES ${XLIB}/filetypes/unpackers/lh1/bit_stream_reader.c)
add_library(xpeccycore STATIC ${XLIB_SOURCES})
target_include_directories(xpeccycore PUBLIC ${XLIB} ${ZLIB_INCLUDE_DIR})
target_compile_definitions(xpeccycore PUBLIC ${XENDIAN})
target_link_libraries(xpeccycore PUBLIC ${ZLIB_LIBRARY} Threads::Threads)

set(BENCHES tsconf baseconf divmmc tape pok zxnext kbd brk dsk gs)
foreach(BENCH ${BENCHES})
	add_executable(${BENCH}-bench core/${BENCH}/main.c)
	target_link_libraries(${BENCH}-bench xpeccycore)
endforeach()
target_sources(zxnext-bench PRIVATE core/zxnext/debugger.c core/zxnext/ram.c)

option(XPECCY_QT_TESTS "Build label and expression checks with Qt" OFF)
if(XPECCY_QT_TESTS)
	set(QTVERSION 6 CACHE STRING "Qt major version")
	find_package(Qt${QTVERSION} REQUIRED COMPONENTS Widgets)
	if(QTVERSION EQUAL 6)
		find_package(Qt6 REQUIRED COMPONENTS Core5Compat)
	endif()
	find_path(SDL2_INCLUDE_DIR SDL_joystick.h PATH_SUFFIXES SDL2)
	if(NOT SDL2_INCLUDE_DIR)
		message(FATAL_ERROR "Set SDL2_INCLUDE_DIR to the SDL2 headers")
	endif()
	add_executable(labels-check core/labels/main.cpp ${XROOT}/src/xcore/labels.cpp ${XROOT}/src/xcore/xexpr.cpp)
	set_target_properties(labels-check PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_include_directories(labels-check PRIVATE ${SDL2_INCLUDE_DIR})
	target_link_libraries(labels-check PRIVATE xpeccycore Qt${QTVERSION}::Widgets)
	if(QTVERSION EQUAL 6)
		target_link_libraries(labels-check PRIVATE Qt6::Core5Compat)
	endif()
	if(MINGW)
		target_link_options(labels-check PRIVATE -static-libgcc -static-libstdc++)
	endif()
	enable_testing()
	add_test(NAME labels COMMAND labels-check ${CMAKE_CURRENT_SOURCE_DIR}/core/labels)
endif()

# the emulated ESP-01 on the Next's UART, with its host sockets: the app's own sources
set(XESP ${XROOT}/src/xcore)
add_executable(esp-bench core/esp/main.cpp ${XESP}/esp.cpp ${XESP}/esp_basic.cpp ${XESP}/esp_wifi.cpp ${XESP}/esp_tcp.cpp
	${XESP}/espline.cpp ${XESP}/espnet.cpp)
target_link_libraries(esp-bench xpeccycore)
if(WIN32)
	target_link_libraries(esp-bench ws2_32 iphlpapi)
endif()

# sibling images: the side/part sets of file names, the app's own source
add_executable(siblings-bench core/siblings/main.cpp ${XROOT}/src/xcore/siblings.cpp)

# a folder volume taking writes, with FatFs (thirdparty/fatfs, ChaN's 1-clause BSD) as the guest
set(FATFS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/fatfs)
add_executable(vfat-bench core/vfat/main.c ${FATFS}/ff.c ${FATFS}/ffunicode.c)
target_include_directories(vfat-bench PRIVATE ${FATFS})
target_link_libraries(vfat-bench xpeccycore)
set_source_files_properties(${FATFS}/ff.c ${FATFS}/ffunicode.c PROPERTIES COMPILE_FLAGS -w)

# GPL v2+ (see tools/z80test/LICENSE): the Z80 sources only, never the library
file(GLOB Z80_SOURCES CONFIGURE_DEPENDS ${XLIB}/cpu/Z80/*.c)
add_executable(z80-coretest ${XROOT}/tools/z80test/coretest.c ${Z80_SOURCES})

# no MinGW runtime on PATH needed to run them
if(MINGW)
	foreach(BENCH ${BENCHES})
		target_link_options(${BENCH}-bench PRIVATE -static)
	endforeach()
	target_link_options(z80-coretest PRIVATE -static)
	target_link_options(vfat-bench PRIVATE -static)
	target_link_options(esp-bench PRIVATE -static)
	target_link_options(siblings-bench PRIVATE -static)
endif()
