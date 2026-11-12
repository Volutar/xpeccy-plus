cmake_minimum_required(VERSION 3.5...4.1)

#compiler settings

#set(CMAKE_CXX_STANDARD 11)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_CXX_EXTENSIONS OFF)
#set(CMAKE_C_COMPILER gcc)
#set(CMAKE_CXX_COMPILER g++)

# common

set(PROJECT_NAME "xpeccy-plus")

# The oldest macOS the bundle runs on, Qt 6.11's own. Has to be set before project().
# Without it clang takes the build machine's version. Info.plist reads it from here;
# macos.yml passes the same value to this build and to SDL's, and checks the bundle.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Oldest macOS the build runs on")

# base version, without the "-dev" tail (see cmake/genversion.cmake)
file(STRINGS "VERSION" XVER)
project(${PROJECT_NAME} VERSION ${XVER})

# clean version w/o "-dev" and build date, for release builds
option(XRELEASE "build as a release version" OFF)

# the release asset this build is, its file name less version and extension
# (win-x64, linux-x86_64...): set by the packaging scripts, empty for any
# other build, which then offers no update of its own (src/xcore/update.cpp)
set(XPACKAGE "" CACHE STRING "release asset suffix of this build, set by packaging/")

# the date a dev build carries, YYYYMMDD; empty: the day it is built. The Nix
# flake gives the commit's date: its sandbox clock says 1980
set(XDATE "" CACHE STRING "build date of a dev build, YYYYMMDD; empty: today")

# Windows: drop the Qt plugins windeployqt copies but this app never loads
# (see cmake/windeploy.cmake.in for the list and why each one goes)
option(TRIMDEPLOY "trim unused Qt plugins from the installed folder" ON)

# Build the xlogh() calls: the ones that would fire per port access, per dot or
# per sample. Off, they are not in the binary at all (see libxpeccy/xlog.h).
option(XLOGHOT "build the hot-path log calls" OFF)
if(XLOGHOT)
	add_definitions(-DXLOG_HOT)
endif()

# The log locks with a plain pthread mutex, the first thing in libxpeccy that
# needs the threads library by name rather than through Qt
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
set(LIBRARIES ${LIBRARIES} Threads::Threads)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)
include(${CMAKE_ROOT}/Modules/FindPackageHandleStandardArgs.cmake)

# endianess

include (TestBigEndian)
test_big_endian(BIG_ENDIAN)
if (BIG_ENDIAN)
	add_definitions(-DWORDS_BIG_ENDIAN)
else()
	add_definitions(-DWORDS_LITTLE_ENDIAN)
endif()

# compilation flags

set(CMAKE_C_FLAGS "-std=gnu99 -Wall -O2 ${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG ${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_DEBUG "-g -DISDEBUG ${CMAKE_C_FLAGS_DEBUG}")

# c++14 is ymfm's floor (sound/ymfm/), and Qt 6 raises it to 17 on its own
set(CMAKE_CXX_FLAGS "-std=c++14 -Wall -O2 ${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG ${CMAKE_CXX_FLAGS_RELEASE}")
set(CMAKE_CXX_FLAGS_DEBUG "-g -DISDEBUG ${CMAKE_CXX_FLAGS_DEBUG}")

include(CheckSymbolExists)
check_symbol_exists(__aarch64__ "" XAARCH64)

# Functions and loops start on cache lines: the emulation's speed then holds from
# build to build, where a change anywhere else could move it by up to 5 %.
# Not on Windows ARM64: clang stops there with "Failed to evaluate function length
# in SEH unwind info" (seen with clang 23)
if(WIN32 AND XAARCH64)
	set(XALIGN "")
else()
	set(XALIGN -falign-functions=64 -falign-loops=32 -falign-jumps=32)
endif()
include(CheckCCompilerFlag)
foreach(flag ${XALIGN})
	string(MAKE_C_IDENTIFIER "HAVE${flag}" var)
	check_c_compiler_flag(${flag} ${var})
	if (${var})
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${flag}")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}")
	endif()
endforeach()

# OS-depended section

set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${XVER})
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DESCRIPTION "ZX Spectrum emulator with a built-in debugger, a fork of Xpeccy by SAM style")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ZX Spectrum emulator with a debugger")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE_eng")
set(CPACK_PACKAGE_VENDOR "Oleksandr Kovalchuk")
# deb Maintainer / rpm Packager: a project address, not a personal one
set(CPACK_PACKAGE_CONTACT "Oleksandr Kovalchuk <xpeccy-plus@kovalchuk.space>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/dotkoval/xpeccy-plus")

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

	set(INC_PATHS local/include include)
	set(LIB_PATHS lib lib64 local/lib local/lib64)

	if ("${PACK}" STREQUAL "rpm")

	# RPM package

	message(STATUS "Package type : RPM")
	set(CPACK_GENERATOR "RPM")
	set(CPACK_RPM_PACKAGE_LICENSE "MIT")
	set(CPACK_RPM_PACKAGE_GROUP "Games")

	else ()

	# DEB package

	message(STATUS "Package type : DEB")
	set(CPACK_GENERATOR "DEB")
	set(CPACK_DEBIAN_PACKAGE_SECTION "Games")
	set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

	endif()

	# install rules are in the "install" section at the end of this file

elseif(${CMAKE_SYSTEM_NAME} MATCHES "/BSD$/")

	set(INC_PATHS pkg/include include)
	set(LIB_PATHS lib lib64 pkg/lib local/lib64)

	add_definitions("-D__BSD")

elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")

	set(INC_PATHS local/include include)
	set(LIB_PATHS lib lib64 local/lib local/lib64)

	# an app bundle belongs in /Applications, but only when no prefix was asked for
	if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
		set(CMAKE_INSTALL_PREFIX "/Applications" CACHE PATH "install prefix" FORCE)
	endif()

	# OpenGL is deprecated on macOS since 10.14 and says so on every include
	add_definitions(-DGL_SILENCE_DEPRECATION)

	# for vkeyboard_mac.mm and dispsync_mac.mm
	find_library(APPKIT_LIBRARY AppKit)
	find_library(COREVIDEO_LIBRARY CoreVideo)
	set(LIBRARIES ${LIBRARIES} ${APPKIT_LIBRARY} ${COREVIDEO_LIBRARY})

	if(CMAKE_OSX_ARCHITECTURES)
		string(REPLACE ";" "-" XARCH "${CMAKE_OSX_ARCHITECTURES}")
	else()
		set(XARCH ${CMAKE_SYSTEM_PROCESSOR})
	endif()

	set(CPACK_GENERATOR "DragNDrop")
	set(CPACK_DMG_FORMAT UDZO)
	# no click-through license on the image: the license ships inside the bundle,
	# and an agreement makes the dmg impossible to mount from a script
	set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)
	# the file name is set further down - it needs the Qt version
	# the bundle sits at the root of the dmg, not in an /Applications subfolder
	set(CPACK_PACKAGING_INSTALL_PREFIX "/")
#	set(CPACK_DMG_BACKGROUND_IMAGE ${CMAKE_SOURCE_DIR}/background.png)

	set(MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME})
	set(MACOSX_BUNDLE_ICON_FILE ${PROJECT_NAME}.icns)
	set(MACOSX_BUNDLE_BUNDLE_VERSION ${XVER})
	set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${XVER})

	set(BUNDLE_ICON_PATH ${CMAKE_SOURCE_DIR}/images/${MACOSX_BUNDLE_ICON_FILE})

	set_source_files_properties(${BUNDLE_ICON_PATH} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

elseif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")

	set(CPACK_GENERATOR "ZIP")
	set(LIBRARIES ${LIBRARIES} winmm.dll dwmapi.dll dxgi ws2_32 iphlpapi)
	set(INC_PATHS $ENV{INCLUDE} $ENV{PATH})
	set(LIB_PATHS $ENV{LIBPATH} $ENV{PATH})

	# CMAKE_SYSTEM_PROCESSOR reports the *host* arch on Windows, so a 32-bit
	# MinGW build would otherwise be labelled AMD64. Ask the compiler instead.
	if(XAARCH64)
		set(XARCH "arm64")
	elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(XARCH "x64")
	else()
		set(XARCH "x86")
	endif()
	set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-win-${XARCH}")

else()

	message(FATAL_ERROR "Unsupported system : ${CMAKE_SYSTEM_NAME}")

endif()

set(INCLUDIRS
	${CMAKE_BINARY_DIR}
	./src
)

# version.h and buildid.h are generated (not stored in the sources) before every
# build; buildid.h has the commit, so it is kept apart from the many files that
# include version.h

set(VERSION_HEADER ${CMAKE_BINARY_DIR}/version.h)
set(BUILDID_HEADER ${CMAKE_BINARY_DIR}/buildid.h)

add_custom_target(version_header ALL
	COMMAND ${CMAKE_COMMAND}
		-DIN_FILE=${CMAKE_SOURCE_DIR}/cmake/version.h.in
		-DOUT_FILE=${VERSION_HEADER}
		-DID_IN_FILE=${CMAKE_SOURCE_DIR}/cmake/buildid.h.in
		-DID_OUT_FILE=${BUILDID_HEADER}
		-DSRC_DIR=${CMAKE_SOURCE_DIR}
		-DXVERSION_BASE=${XVER}
		-DXRELEASE=${XRELEASE}
		-DXPACKAGE=${XPACKAGE}
		-DXDATE=${XDATE}
		-P ${CMAKE_SOURCE_DIR}/cmake/genversion.cmake
	BYPRODUCTS ${VERSION_HEADER} ${BUILDID_HEADER}
	COMMENT "Generating version.h"
)

# sources
file(GLOB_RECURSE SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)
# included by lh1_decoder.c, not a translation unit of its own
list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/libxpeccy/filetypes/unpackers/lh1/bit_stream_reader.c)

# Objective-C++ is not in the glob: it only builds on macOS
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	if(NOT CMAKE_VERSION VERSION_LESS 3.16)
		enable_language(OBJCXX)
	endif()
	set(SOURCES ${SOURCES} ./src/vkeyboard_mac.mm ./src/xgui/dispsync_mac.mm)
endif()

# headers
file(GLOB_RECURSE HEADERS
	./src/*.h
)

# Qt user interface
file(GLOB_RECURSE UIFILES
	./ui/*.ui
)

# Qt resources
set(QRCFILES ./xpeccy.qrc)

# Qt moc files
set(MOCFILES
	./src/ethread.h
	./src/emulwin.h
	./src/xapp.h
	./src/xgui/debuga/debuger.h
	./src/xgui/debuga/dbg_sprscan.h
	./src/xgui/debuga/dbg_memfill.h
	./src/xgui/debuga/dbg_finder.h
	./src/xgui/debuga/dbg_dump.h
	./src/xgui/debuga/dbg_disasm.h
	./src/xgui/debuga/dbg_brkpoints.h
	./src/xgui/debuga/dbg_diskdump.h
	./src/xgui/debuga/dbg_cmos_dump.h
	./src/xgui/debuga/dbg_widgets.h
	./src/xgui/debuga/dbg_rdump.h
	./src/xgui/debuga/dbg_palette.h
	./src/xgui/debuga/dbg_heat.h
	./src/watcher.h
	./src/xgui/scrwin.h
	./src/xgui/debuga/dbg_zxscr.h
	./src/xgui/sndwin.h
	./src/xgui/heatwin.h
	./src/xgui/debuga/dbg_sndchip.h
	./src/xgui/evowin.h
	./src/xgui/debuga/dbg_evo.h
	./src/xgui/debuga/dbg_next.h
	./src/xgui/options/setupwin.h
	./src/xgui/options/opt_romset.h
	./src/xgui/options/opt_diskcat.h
	./src/xgui/options/opt_tapecat.h
	./src/xgui/options/opt_hotkeytab.h
	./src/xgui/options/opt_paledit.h
	./src/vkeyboard.h
#	./src/xcore/xcore.h
	./src/xcore/gamepad.h
	./src/xgui/xgui.h
	./src/xgui/classes.h
	./src/xgui/iconengine.h
	./src/xgui/labelist.h
)

# Qt 5 or 6. -DQTVERSION=5 or 6 picks one; without it Qt 6 is taken when all of it
# that we need is there, Qt 5 otherwise. QtNetwork (DeZog, the control port) and
# OpenGL are always in: a machine with no GL falls back to QPainter at run time.

set(QT6_PARTS Core Gui Widgets Core5Compat Network OpenGLWidgets)
if(NOT QTVERSION)
	find_package(Qt6 QUIET COMPONENTS ${QT6_PARTS})
	if(Qt6_FOUND)
		set(QTVERSION 6)
	else()
		string(REPLACE ";" ", " QT6_LIST "${QT6_PARTS}")
		message(STATUS "Qt 6 (${QT6_LIST}) not found, trying Qt 5")
		set(QTVERSION 5)
	endif()
endif()
set(QTVERSION ${QTVERSION} CACHE STRING "Qt major version: 5 or 6")

find_package(OpenGL REQUIRED)

if(${QTVERSION} EQUAL 5)

	find_package(Qt5 COMPONENTS Core Gui Widgets Network OpenGL REQUIRED)
	set(LIBRARIES ${LIBRARIES} Qt5::Widgets Qt5::Network Qt5::OpenGL ${OPENGL_LIBRARIES})
	qt5_add_resources(RESOURCES ${QRCFILES})
	qt5_wrap_ui(UIHEADERS ${UIFILES})
	qt5_wrap_cpp(MOCHEADERS ${MOCFILES})
	set(CPACK_RPM_PACKAGE_REQUIRES "libQt5Gui5 >= 5.3, libQt5Widgets5 >= 5.3, libQt5Network5 >= 5.3, libQt5OpenGL5 >= 5.3")
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt5gui5 (>=5.3), libqt5widgets5 (>=5.3), libqt5network5 (>=5.3), libqt5opengl5 (>=5.3)")
	set(QT_VER ${Qt5Widgets_VERSION})
	get_target_property(QMAKE_EXE Qt5::qmake IMPORTED_LOCATION)

elseif(${QTVERSION} EQUAL 6)
# Qt6 requires c++17
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)

	find_package(Qt6 COMPONENTS ${QT6_PARTS} REQUIRED)
	# 6.11.1 traps at startup on macOS 27.2 inside QImage::toCGImage(); 6.11.2
	# and 6.11.3 start. The 6.11 series only: older Qt 6 was not tried.
	if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin" AND Qt6_VERSION
			AND Qt6_VERSION VERSION_GREATER_EQUAL 6.11.0
			AND Qt6_VERSION VERSION_LESS 6.11.2)
		message(WARNING
			"Qt ${Qt6_VERSION} traps at startup on macOS 27.2 while the menu bar is built; "
			"6.11.2 and 6.11.3 start. Prefer Qt 6.11.2 or newer for a macOS build.")
	endif()
	set(LIBRARIES ${LIBRARIES} Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Core5Compat
		Qt6::Network Qt6::OpenGLWidgets ${OPENGL_LIBRARIES})
	qt6_add_resources(RESOURCES ${QRCFILES})
	qt6_wrap_ui(UIHEADERS ${UIFILES})
	qt6_wrap_cpp(MOCHEADERS ${MOCFILES})
	set(CPACK_RPM_PACKAGE_REQUIRES "libQt6Gui6 >= 6.1, libQt6Widgets6 >= 6.1, libQt6Core5Compat6 >= 6.1, libQt6Network6 >= 6.1, libQt6OpenGL6 >= 6.1")
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6gui6 (>=6.1), libqt6widgets6 (>=6.1), libqt6core5compat6 >= 6.1, libqt6network6 (>=6.1), libqt6opengl6 (>=6.1)")
	set(QT_VER ${Qt6Widgets_VERSION})
	get_target_property(QMAKE_EXE Qt6::qmake IMPORTED_LOCATION)
else()
	message(FATAL_ERROR "QTVERSION=${QTVERSION}: only 5 and 6 are supported")
endif()

# SDL2

find_package(SDL2 REQUIRED)
# main.cpp keeps its own main(): no SDL_main rename (it clashed with Qt6's qMain)
add_definitions(-DSDL_MAIN_HANDLED)
include_directories(${SDL2_INCLUDE_DIR})
set(LIBRARIES ${LIBRARIES} ${SDL2_LIBRARY})
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libsdl2-2.0-0 (>=2.0) | libSDL2 (>=2.0)")
set(CPACK_RPM_PACKAGE_DEPENDS "${CPACK_RPM_PACKAGE_DEPENDS}, SDL2 >= 2.0")
message(STATUS "SDL2_INCLUDE_DIR = " ${SDL2_INCLUDE_DIR})

# zlib: rzx and compressed tzx blocks

find_package(ZLIB REQUIRED)
set(INCLUDIRS ${INCLUDIRS} ${ZLIB_INCLUDE_DIR})
set(LIBRARIES ${LIBRARIES} ${ZLIB_LIBRARY})
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, zlib1g (>=1.2)")
set(CPACK_RPM_PACKAGE_DEPENDS "${CPACK_RPM_PACKAGE_DEPENDS}, zlib >= 1.2")

# libcurl: the program's downloads (src/xcore/download.cpp). Windows links the static
# Schannel build make-dist.ps1 makes, macOS the SDK's. Linux loads the system's
# libcurl.so.4 when a download starts: linked, the binary would ask for the build
# host's symbol versions (Debian's CURL_OPENSSL_4) and not start where they differ

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
	find_package(CURL CONFIG REQUIRED)
	set(LIBRARIES ${LIBRARIES} CURL::libcurl)
	message(STATUS "libcurl ${CURL_VERSION} (static)")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	find_package(CURL REQUIRED)
	set(INCLUDIRS ${INCLUDIRS} ${CURL_INCLUDE_DIRS})
	set(LIBRARIES ${LIBRARIES} ${CURL_LIBRARIES})
else()
	# a distribution's own package builds against its own libcurl and can link it
	option(CURL_DLOPEN "Load libcurl.so.4 at run time instead of linking it" ON)
	if(CURL_DLOPEN)
		find_path(CURL_INCLUDE_DIR curl/curl.h)
		if(NOT CURL_INCLUDE_DIR)
			message(FATAL_ERROR "curl/curl.h not found: install libcurl's development package")
		endif()
		add_definitions(-DXP_CURL_DLOPEN)
		set(INCLUDIRS ${INCLUDIRS} ${CURL_INCLUDE_DIR})
		set(LIBRARIES ${LIBRARIES} ${CMAKE_DL_LIBS})
	else()
		find_package(CURL REQUIRED)
		set(INCLUDIRS ${INCLUDIRS} ${CURL_INCLUDE_DIRS})
		set(LIBRARIES ${LIBRARIES} ${CURL_LIBRARIES})
	endif()
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libcurl4 | libcurl4t64")
	set(CPACK_RPM_PACKAGE_DEPENDS "${CPACK_RPM_PACKAGE_DEPENDS}, libcurl")
endif()

# FreeType: the on-screen keyboard's lettering. Our own copy on every platform, so
# the keyboard comes out the same everywhere (thirdparty/freetype/README.xpeccy.md)

set(FT_DIR ${CMAKE_SOURCE_DIR}/thirdparty/freetype)
add_library(xpfreetype STATIC
	${FT_DIR}/src/base/ftsystem.c
	${FT_DIR}/src/base/ftinit.c
	${FT_DIR}/src/base/ftdebug.c
	${FT_DIR}/src/base/ftbase.c
	${FT_DIR}/src/base/ftbitmap.c
	${FT_DIR}/src/sfnt/sfnt.c
	${FT_DIR}/src/truetype/truetype.c
	${FT_DIR}/src/autofit/autofit.c
	${FT_DIR}/src/smooth/smooth.c
)
# xpeccy/ first: its config headers stand in for the ones under include/
target_include_directories(xpfreetype PUBLIC ${FT_DIR}/xpeccy ${FT_DIR}/include)
target_compile_definitions(xpfreetype PRIVATE FT2_BUILD_LIBRARY DARWIN_NO_CARBON)
# with PostScript names off, ttpost.h still declares its two static functions
target_compile_options(xpfreetype PRIVATE -Wno-unused-function)
set(LIBRARIES ${LIBRARIES} xpfreetype)
if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	# Qt links the system's FreeType: ours must not be exported over it
	set(LIBRARIES ${LIBRARIES} "-Wl,--exclude-libs,ALL")
endif()

# LZMA SDK: the 7z reader, for images packed in a .7z (thirdparty/lzma/README.xpeccy.md)

set(LZMA_DIR ${CMAKE_SOURCE_DIR}/thirdparty/lzma/C)
file(GLOB LZMA_SOURCES ${LZMA_DIR}/*.c)
add_library(xplzma STATIC ${LZMA_SOURCES})
target_include_directories(xplzma PUBLIC ${LZMA_DIR})
target_compile_definitions(xplzma PRIVATE Z7_PPMD_SUPPORT Z7_EXTRACT_ONLY)
set(LIBRARIES ${LIBRARIES} xplzma)
set(INCLUDIRS ${INCLUDIRS} ${LZMA_DIR})

# other

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS})
elseif(${CMAKE_SYSTEM_NAME} MATCHES "/BSD$/")
	add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS})
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS} ${BUNDLE_ICON_PATH})
	# qt.io's Qt names its frameworks by @rpath, which macdeployqt copies here
	set_target_properties(${PROJECT_NAME} PROPERTIES
		MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist
		INSTALL_RPATH "@executable_path/../Frameworks"
	)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
	# icon + VERSIONINFO (product name and version in the exe properties) + manifest
	configure_file(${CMAKE_SOURCE_DIR}/xpeccy.rc.in ${CMAKE_BINARY_DIR}/xpeccy.rc @ONLY)
	set_source_files_properties(${CMAKE_BINARY_DIR}/xpeccy.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/xpeccy.manifest)
	add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS} ${CMAKE_BINARY_DIR}/xpeccy.rc)
endif()

include_directories(${INCLUDIRS})
target_link_libraries(${PROJECT_NAME} ${LIBRARIES})
add_dependencies(${PROJECT_NAME} version_header)

# install
#
# Windows: a self-contained, ready-to-run folder. Build it with
#	cmake --install <build-dir> --prefix <dist-dir>
# Note that --install never deletes anything, so wipe the target dir first.
#
# Linux: a normal prefix layout, used both by CPack and by the AppImage
# (see packaging/make-appimage.sh).
#
# macOS: a self-contained .app, the same way. CPack (DragNDrop) packs the very
# same tree into a dmg.
#
# The user manual goes in when MANUAL_DIR names a built one (tools/manual.py);
# CMake does not build it. It lies beside README in each layout, where
# MainWin::openManual() looks for it.

set(MANUAL_DIR "" CACHE PATH "a built user manual to install (tools/manual.py)")
set(SYSNEXT_DIR "" CACHE PATH "System/Next to install beside the program, for tests: a release has none (packaging/make-dist.ps1 -SysNext)")

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")

	set(BUNDLE_RES ${PROJECT_NAME}.app/Contents/Resources)

	install(TARGETS ${PROJECT_NAME} BUNDLE DESTINATION .)
	# machine configuration: the emulator copies it into the user's config
	# directory on the first run (see sysConfigDir() in src/xcore/config.cpp)
	install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION ${BUNDLE_RES}/config)
	install(FILES
		${CMAKE_SOURCE_DIR}/LICENSE_eng
		${CMAKE_SOURCE_DIR}/LICENSE_rus
		${CMAKE_SOURCE_DIR}/LICENSE_DEJAVU
		${CMAKE_SOURCE_DIR}/LICENSE_YMFM
		${CMAKE_SOURCE_DIR}/LICENSE_FREETYPE
		${CMAKE_SOURCE_DIR}/LICENSE_LZMA
		${CMAKE_SOURCE_DIR}/LICENSE_LIBERATION
		${CMAKE_SOURCE_DIR}/LICENSE_ICONS
		${CMAKE_SOURCE_DIR}/LICENSE_CURL
		${CMAKE_SOURCE_DIR}/README.md
		${CMAKE_SOURCE_DIR}/CHANGELOG.md
		DESTINATION ${BUNDLE_RES}/doc
	)
	if(MANUAL_DIR)
		install(DIRECTORY ${MANUAL_DIR}/ DESTINATION ${BUNDLE_RES}/doc/manual)
	endif()
	if(SYSNEXT_DIR)
		install(DIRECTORY ${SYSNEXT_DIR}/ DESTINATION ${BUNDLE_RES}/sysnext)
	endif()

	# Everything the bundle needs but the build does not produce: Qt, SDL3 and a
	# signature. Runs on the installed bundle, after the files above are in place
	# (see cmake/macdeploy.cmake.in).
	get_filename_component(QT_BIN_DIR "${QMAKE_EXE}" DIRECTORY)
	find_program(MACDEPLOYQT macdeployqt HINTS ${QT_BIN_DIR})
	get_filename_component(QT_LIB_DIR "${QT_BIN_DIR}/../lib" ABSOLUTE)
	# SDL2 built from source (macos.yml) names itself by @rpath, like qt.io's Qt
	get_filename_component(SDL2_LIB_DIR "${SDL2_INCLUDE_DIR}/../../lib" ABSOLUTE)
	# SDL2 from Homebrew is the sdl2-compat shim and loads this one at runtime
	find_library(SDL3_LIBRARY NAMES SDL3
		HINTS ${SDL2_INCLUDE_DIR}/../..
		PATHS /opt/homebrew /usr/local
		PATH_SUFFIXES lib
	)
	if(SDL3_LIBRARY)
		get_filename_component(SDL3_LIBRARY "${SDL3_LIBRARY}" REALPATH)
	endif()
	if(MACDEPLOYQT)
		configure_file(
			${CMAKE_SOURCE_DIR}/cmake/macdeploy.cmake.in
			${CMAKE_BINARY_DIR}/macdeploy.cmake @ONLY
		)
		install(SCRIPT ${CMAKE_BINARY_DIR}/macdeploy.cmake)
	else()
		message(WARNING "macdeployqt not found: the bundle won't run on another Mac")
	endif()

elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

	install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
	# machine configuration: the emulator copies it into the user's config
	# directory on the first run, so a package works right after installing
	install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION share/${PROJECT_NAME}/config)
	# the icon file name must match Icon= in the .desktop file
	foreach(ICON_SIZE 16 24 32 48 64 128 256 512)
		install(FILES ${CMAKE_SOURCE_DIR}/images/icons/${PROJECT_NAME}-${ICON_SIZE}.png
			DESTINATION share/icons/hicolor/${ICON_SIZE}x${ICON_SIZE}/apps
			RENAME ${PROJECT_NAME}.png
		)
	endforeach()
	install(FILES ${CMAKE_SOURCE_DIR}/${PROJECT_NAME}.desktop DESTINATION share/applications)
	install(FILES
		${CMAKE_SOURCE_DIR}/LICENSE_eng
		${CMAKE_SOURCE_DIR}/LICENSE_rus
		${CMAKE_SOURCE_DIR}/LICENSE_DEJAVU
		${CMAKE_SOURCE_DIR}/LICENSE_YMFM
		${CMAKE_SOURCE_DIR}/LICENSE_FREETYPE
		${CMAKE_SOURCE_DIR}/LICENSE_LZMA
		${CMAKE_SOURCE_DIR}/LICENSE_LIBERATION
		${CMAKE_SOURCE_DIR}/LICENSE_ICONS
		${CMAKE_SOURCE_DIR}/LICENSE_CURL
		${CMAKE_SOURCE_DIR}/README.md
		${CMAKE_SOURCE_DIR}/CHANGELOG.md
		DESTINATION share/doc/${PROJECT_NAME}
	)
	if(MANUAL_DIR)
		install(DIRECTORY ${MANUAL_DIR}/ DESTINATION share/doc/${PROJECT_NAME}/manual)
	endif()
	if(SYSNEXT_DIR)
		install(DIRECTORY ${SYSNEXT_DIR}/ DESTINATION share/${PROJECT_NAME}/sysnext)
	endif()

elseif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")

	install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION .)
	install(DIRECTORY ${CMAKE_SOURCE_DIR}/config/ DESTINATION config)
	install(FILES
		${CMAKE_SOURCE_DIR}/README.md
		${CMAKE_SOURCE_DIR}/CHANGELOG.md
		${CMAKE_SOURCE_DIR}/LICENSE_eng
		${CMAKE_SOURCE_DIR}/LICENSE_rus
		${CMAKE_SOURCE_DIR}/LICENSE_DEJAVU
		${CMAKE_SOURCE_DIR}/LICENSE_YMFM
		${CMAKE_SOURCE_DIR}/LICENSE_FREETYPE
		${CMAKE_SOURCE_DIR}/LICENSE_LZMA
		${CMAKE_SOURCE_DIR}/LICENSE_LIBERATION
		${CMAKE_SOURCE_DIR}/LICENSE_ICONS
		${CMAKE_SOURCE_DIR}/LICENSE_CURL
		DESTINATION .
	)
	if(MANUAL_DIR)
		install(DIRECTORY ${MANUAL_DIR}/ DESTINATION manual)
	endif()
	if(SYSNEXT_DIR)
		install(DIRECTORY ${SYSNEXT_DIR}/ DESTINATION sysnext)
	endif()

	# FindSDL2 only reports the import library, so locate the runtime dll too
	find_file(SDL2_RUNTIME_LIBRARY SDL2.dll
		HINTS $ENV{SDL2DIR} ${SDL2_INCLUDE_DIR}/..
		PATH_SUFFIXES bin lib lib/x64 lib/x86
	)
	if(SDL2_RUNTIME_LIBRARY)
		install(FILES ${SDL2_RUNTIME_LIBRARY} DESTINATION .)
	else()
		message(WARNING "SDL2.dll not found: the installed folder won't run")
	endif()

	# Qt runtime: dlls, platforms\qwindows.dll and the compiler runtime.
	# Runs against the staged folder, not the build dir - windeployqt scans
	# whatever directory the binary it is given lives in.
	get_filename_component(QT_BIN_DIR "${QMAKE_EXE}" DIRECTORY)
	get_filename_component(CXX_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
	find_program(WINDEPLOYQT windeployqt HINTS ${QT_BIN_DIR})
	if(WINDEPLOYQT)
		configure_file(
			${CMAKE_SOURCE_DIR}/cmake/windeploy.cmake.in
			${CMAKE_BINARY_DIR}/windeploy.cmake @ONLY
		)
		install(SCRIPT ${CMAKE_BINARY_DIR}/windeploy.cmake)
	else()
		message(WARNING "windeployqt not found: the installed folder won't run")
	endif()

endif()

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	# Named the way packaging/make-dist.ps1 and make-appimage.sh name theirs:
	# the package's name, and a build that is not a release says so.
	if(XRELEASE)
		set(XPKGVER ${XVER})
	else()
		string(TIMESTAMP XBUILD_DATE "%Y%m%d")
		set(XPKGVER "${XVER}-dev+${XBUILD_DATE}")
	endif()
	# the dmg is the asset the update check looks for: named by XPACKAGE when set
	if(XPACKAGE)
		set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${XPKGVER}-${XPACKAGE}")
	else()
		set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${XPKGVER}-macos-${XARCH}")
	endif()
endif()

include(CPack)

# print messages

message(STATUS "\n")
message(STATUS "Project will be compiled with following options:\n")
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Xpeccy+ version : ${XVER}")
message(STATUS "OS : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Qt version : ${QT_VER}")
message(STATUS "SDL version : 2.x")

if (BIG_ENDIAN)
	message(STATUS "Endianness: BE")
else ()
	message(STATUS "Endianness: LE")
endif()

message(STATUS "\n")
