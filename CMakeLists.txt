cmake_minimum_required(VERSION 3.5...4.1)

#compiler settings

#set(CMAKE_CXX_STANDARD 11)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_CXX_EXTENSIONS OFF)
#set(CMAKE_C_COMPILER gcc)
#set(CMAKE_CXX_COMPILER g++)

# common

set(PROJECT_NAME "xpeccy-plus")

# base version, without the "-dev" tail (see cmake/genversion.cmake)
file(STRINGS "VERSION" XVER)
project(${PROJECT_NAME} VERSION ${XVER})

# clean version w/o "-dev" and build date, for release builds
option(XRELEASE "build as a release version" OFF)

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

	set(LIBRARIES ${LIBRARIES} dl)

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

	# for vkeyboard_mac.mm
	find_library(APPKIT_LIBRARY AppKit)
	set(LIBRARIES ${LIBRARIES} ${APPKIT_LIBRARY})

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
	set(LIBRARIES ${LIBRARIES} winmm.dll dwmapi.dll)
	set(INC_PATHS $ENV{INCLUDE} $ENV{PATH})
	set(LIB_PATHS $ENV{LIBPATH} $ENV{PATH})

	# CMAKE_SYSTEM_PROCESSOR reports the *host* arch on Windows, so a 32-bit
	# MinGW build would otherwise be labelled AMD64. Ask the compiler instead.
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
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

# version.h is generated (not stored in the sources) before every build

set(VERSION_HEADER ${CMAKE_BINARY_DIR}/version.h)

add_custom_target(version_header ALL
	COMMAND ${CMAKE_COMMAND}
		-DIN_FILE=${CMAKE_SOURCE_DIR}/cmake/version.h.in
		-DOUT_FILE=${VERSION_HEADER}
		-DXVERSION_BASE=${XVER}
		-DXRELEASE=${XRELEASE}
		-P ${CMAKE_SOURCE_DIR}/cmake/genversion.cmake
	BYPRODUCTS ${VERSION_HEADER}
	COMMENT "Generating version.h"
)

# sources
file(GLOB_RECURSE SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)

# Objective-C++ is not in the glob: it only builds on macOS
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	if(NOT CMAKE_VERSION VERSION_LESS 3.16)
		enable_language(OBJCXX)
	endif()
	set(SOURCES ${SOURCES} ./src/vkeyboard_mac.mm)
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
	./src/xgui/debuga/dbg_sndchip.h
	./src/xgui/options/setupwin.h
	./src/xgui/options/padbinder.h
	./src/xgui/options/opt_romset.h
	./src/xgui/options/opt_diskcat.h
	./src/xgui/options/opt_tapecat.h
	./src/xgui/options/opt_hotkeytab.h
	./src/xgui/options/opt_paledit.h
	./src/xgui/options/opt_gamepad.h
	./src/vkeyboard.h
#	./src/xcore/xcore.h
	./src/xcore/gamepad.h
	./src/xgui/xgui.h
	./src/xgui/classes.h
	./src/xgui/labelist.h
)

# Qt5 / Qt6

if (DEFINED QT6BUILD)
	if (${QT6BUILD})
		set(QTVERSION 6)
	else(${QT6BUILD})
		set(QTVERSION 5)
	endif(${QT6BUILD})
elseif(NOT DEFINED QTVERSION)
	set(QTVERSION 5)
endif()

if(${QTVERSION} EQUAL 4)
	find_package(Qt4 COMPONENTS QtCore QtGui REQUIRED)
	include(${QT_USE_FILE})
	qt4_add_resources(RESOURCES ${QRCFILES})
	qt4_wrap_ui(UIHEADERS ${UIFILES})
	qt4_wrap_cpp(MOCHEADERS ${MOCFILES})
	set(CPACK_RPM_PACKAGE_REQUIRES "libqt4 >= 4.6, libqt4-x11 >= 4.6")
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqtcore4 (>=4.6), libqtgui4 (>=4.6)")

	if(${USEQTNETWORK})
		find_package(Qt4 COMPONENTS QtNetwork REQUIRED)
		set(CPACK_RPM_PACKAGE_REQUIRES "${CPACK_RPM_PACKAGE_REQUIRES}, libqt4-network >= 4.6")
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt4-network (>=4.6)")
		add_definitions("-DUSENETWORK")
	endif()
	if(${USEOPENGL})
		find_package(Qt4 COMPONENTS QtOpenGL REQUIRED)
		set(CPACK_RPM_PACKAGE_REQUIRES "${CPACK_RPM_PACKAGE_REQUIRES}, libqt4-opengl >= 4.6")
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt4-opengl (>=4.6)")
		add_definitions("-DUSEOPENGL")
	endif()

	set(LIBRARIES ${LIBRARIES} ${QT_LIBRARIES})
	set(QT_VER ${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}.${QT_VERSION_PATCH})

elseif(${QTVERSION} EQUAL 5)

	find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)
	set(LIBRARIES ${LIBRARIES} Qt5::Widgets)
	qt5_add_resources(RESOURCES ${QRCFILES})
	qt5_wrap_ui(UIHEADERS ${UIFILES})
	qt5_wrap_cpp(MOCHEADERS ${MOCFILES})
	set(CPACK_RPM_PACKAGE_REQUIRES "libQt5Gui5 >= 5.3, libQt5Widgets5 >= 5.3")
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt5gui5 (>=5.3), libqt5widgets5 (>=5.3)")

	if(${USEQTNETWORK})
		find_package(Qt5 COMPONENTS Network REQUIRED)
		set(LIBRARIES ${LIBRARIES} Qt5::Network)
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt5network5 (>=5.3)")
		set(CPACK_RPM_PACKAGE_REQUIRES "${CPACK_RPM_PACKAGE_REQUIRES}, libQt5Network5 >= 5.3")
		add_definitions("-DUSENETWORK")
	endif()
	if (${USEOPENGL})
		find_package(OpenGL REQUIRED)
		find_package(Qt5 COMPONENTS OpenGL REQUIRED)
		set(LIBRARIES ${LIBRARIES} Qt5::OpenGL ${OPENGL_LIBRARIES})
		set(CPACK_RPM_PACKAGE_REQUIRES "${CPACK_RPM_PACKAGE_REQUIRES}, libQt5OpenGL5 >= 5.3")
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt5opengl5 (>=5.3)")
		add_definitions("-DUSEOPENGL")
	endif()

	set(QT_VER ${Qt5Widgets_VERSION})
	get_target_property(QMAKE_EXE Qt5::qmake IMPORTED_LOCATION)

elseif(${QTVERSION} EQUAL 6)
#	message(FATAL_ERROR "Qt6 usage is not implemented yet")
# Qt6 requires c++17
	set(CMAKE_CXX_STANDARD 17)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)

	find_package(Qt6 COMPONENTS Core Gui Widgets Core5Compat REQUIRED)
	set(LIBRARIES ${LIBRARIES} Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Core5Compat)
	qt6_add_resources(RESOURCES ${QRCFILES})
	qt6_wrap_ui(UIHEADERS ${UIFILES})
	qt6_wrap_cpp(MOCHEADERS ${MOCFILES})
	set(CPACK_RPM_PACKAGE_REQUIRES "libQt6Gui6 >= 6.1, libQt6Widgets6 >= 6.1, libQt6Core5Compat6 >= 6.1")
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6gui6 (>=6.1), libqt6widgets6 (>=6.1), libqt6core5compat6 >= 6.1")
	if(${USEQTNETWORK})
		find_package(Qt6 COMPONENTS Network REQUIRED)
		set(LIBRARIES ${LIBRARIES} Qt6::Network)
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt6network6 (>=6.1)")
		set(CPACK_RPM_PACKAGE_REQUIRES "${CPACK_RPM_PACKAGE_REQUIRES}, libQt6Network6 >= 6.1")
		add_definitions("-DUSENETWORK")
	endif()
	if (${USEOPENGL})
		find_package(OpenGL REQUIRED)
		find_package(Qt6 COMPONENTS OpenGLWidgets REQUIRED)
		set(LIBRARIES ${LIBRARIES} Qt6::OpenGLWidgets ${OPENGL_LIBRARIES})
		set(CPACK_RPM_PACKAGE_REQUIRES "${CPACK_RPM_PACKAGE_REQUIRES}, libQt6OpenGL6 >= 6.1")
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt6opengl6 (>=6.1)")
		add_definitions("-DUSEOPENGL")
	endif()

	set(QT_VER ${Qt6Widgets_VERSION})
	get_target_property(QMAKE_EXE Qt6::qmake IMPORTED_LOCATION)
else()
	message(FATAL_ERROR "Not supported Qt version defined")
endif()

# SDL

if(${SDL1BUILD})
	find_package(SDL REQUIRED)
	if(${SDL_FOUND})
		add_definitions(-DHAVESDL1)
		include_directories(${SDL_INCLUDE_DIR})
		set(LIBRARIES ${LIBRARIES} ${SDL_LIBRARY})
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libSDL1.2debian (>=1.2)")
		set(CPACK_RPM_PACKAGE_DEPENDS "${CPACK_RPM_PACKAGE_DEPENDS}, SDL >= 1.2")
		set(SDL_VER ${SDL_VERSION_STRING})
		set(SDL_NAME "SDL")
	endif(${SDL_FOUND})
else(${SDL1BUILD})
	find_package(SDL2 REQUIRED)
	if (${SDL2_FOUND})
		add_definitions(-DHAVESDL2)
		include_directories(${SDL2_INCLUDE_DIR})
		set(LIBRARIES ${LIBRARIES} ${SDL2_LIBRARY})
		set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libsdl2-2.0-0 (>=2.0) | libSDL2 (>=2.0)")
		set(CPACK_RPM_PACKAGE_DEPENDS "${CPACK_RPM_PACKAGE_DEPENDS}, SDL2 >= 2.0")
		set(SDL_VER "2.x")
		set(SDL_NAME "SDL2")
		message(STATUS "SDL2_INCLUDE_DIR = " ${SDL2_INCLUDE_DIR})
	endif(${SDL2_FOUND})
endif(${SDL1BUILD})

# zlib (for rzx)

find_package(ZLIB)
if(ZLIB_FOUND)
	add_definitions(-DHAVEZLIB)
	set(INCLUDIRS ${INCLUDIRS} ${ZLIB_INCLUDE_DIR})
	set(LIBRARIES ${LIBRARIES} ${ZLIB_LIBRARY})
	set(CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}, zlib1g (>=1.2)")
	set(CPACK_RPM_PACKAGE_DEPENDS "${CPACK_RPM_PACKAGE_DEPENDS}, zlib >= 1.2")
endif(ZLIB_FOUND)

# other

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS})
elseif(${CMAKE_SYSTEM_NAME} MATCHES "/BSD$/")
	add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS})
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${SOURCES} ${HEADERS} ${UIHEADERS} ${RESOURCES} ${MOCHEADERS} ${BUNDLE_ICON_PATH})
	set_target_properties(${PROJECT_NAME} PROPERTIES
		MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist
	)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
	# icon + VERSIONINFO (product name and version in the exe properties)
	configure_file(${CMAKE_SOURCE_DIR}/xpeccy.rc.in ${CMAKE_BINARY_DIR}/xpeccy.rc @ONLY)
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
		${CMAKE_SOURCE_DIR}/README.md
		${CMAKE_SOURCE_DIR}/CHANGELOG.md
		DESTINATION ${BUNDLE_RES}/doc
	)

	# Everything the bundle needs but the build does not produce: Qt, SDL3 and a
	# signature. Runs on the installed bundle, after the files above are in place
	# (see cmake/macdeploy.cmake.in).
	get_filename_component(QT_BIN_DIR "${QMAKE_EXE}" DIRECTORY)
	find_program(MACDEPLOYQT macdeployqt HINTS ${QT_BIN_DIR})
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
		${CMAKE_SOURCE_DIR}/README.md
		${CMAKE_SOURCE_DIR}/CHANGELOG.md
		DESTINATION share/doc/${PROJECT_NAME}
	)

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
		DESTINATION .
	)

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
	# arch, Qt version, and a build that is not a release says so. Set here and
	# not in the section above, which runs before the Qt version is known.
	if(XRELEASE)
		set(XPKGVER ${XVER})
	else()
		string(TIMESTAMP XBUILD_DATE "%Y%m%d")
		set(XPKGVER "${XVER}-dev+${XBUILD_DATE}")
	endif()
	set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${XPKGVER}-macos-${XARCH}-qt${QTVERSION}")
endif()

include(CPack)

# print messages

message(STATUS "\n")
message(STATUS "Project will be compiled with following options:\n")
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Xpeccy+ version : ${XVER}")
message(STATUS "OS : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Qt version : ${QT_VER}")
message(STATUS "SDL version : ${SDL_VER}")

if (${USEQTNETWORK})
	message(STATUS "Network: yes")
else()
	message(STATUS "Network: no")
endif()

if (${USEOPENGL})
	message(STATUS "OpenGL: yes")
else()
	message(STATUS "OpenGL: no")
endif()

if (BIG_ENDIAN)
	message(STATUS "Endianness: BE")
else ()
	message(STATUS "Endianness: LE")
endif()

message(STATUS "\n")
