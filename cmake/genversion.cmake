# Generates version.h and buildid.h before every build, so a dev build always carries the
# current date. Called from CMakeLists.txt as:
#	cmake -DIN_FILE=... -DOUT_FILE=... -DID_IN_FILE=... -DID_OUT_FILE=... -DSRC_DIR=...
#		-DXVERSION_BASE=... -DXRELEASE=... -DXPACKAGE=... -P genversion.cmake

string(TIMESTAMP XBUILD_YMD "%Y%m%d")
if (XRELEASE)
	set(XBUILD_DATE "")
	set(XVERSION "${XVERSION_BASE}")
	set(XRELEASE_BUILD 1)
else()
	set(XBUILD_DATE "${XBUILD_YMD}")
	set(XVERSION "${XVERSION_BASE}-dev+${XBUILD_DATE}")
	set(XRELEASE_BUILD 0)
endif()

# write to a temp file and copy it only if the content changed: the date changes
# once a day, so nothing depending on version.h is rebuilt on every build
configure_file(${IN_FILE} ${OUT_FILE}.tmp @ONLY)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT_FILE}.tmp ${OUT_FILE})
file(REMOVE ${OUT_FILE}.tmp)

# the commit, empty where there is no git (a source archive): the update check
# tells two nightlies of one day apart by it
set(XCOMMIT "")
find_package(Git QUIET)
if (GIT_FOUND)
	execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
		WORKING_DIRECTORY ${SRC_DIR}
		OUTPUT_VARIABLE XCOMMIT
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE res)
	if (NOT res EQUAL 0)
		set(XCOMMIT "")
	endif()
endif()
configure_file(${ID_IN_FILE} ${ID_OUT_FILE}.tmp @ONLY)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ID_OUT_FILE}.tmp ${ID_OUT_FILE})
file(REMOVE ${ID_OUT_FILE}.tmp)
