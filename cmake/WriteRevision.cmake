# Generate common/versions/revision.h from the CURRENT git HEAD.
#
# Run as a script (cmake -P) so it can execute at BUILD time, not just at
# configure time. See common/CMakeLists.txt for why that matters: a sha baked at
# configure time goes stale the moment you commit, so `Compiled Version:` in a
# log stops identifying the binary that produced it.
#
# Required: -DSRC_DIR=<repo root> -DOUT_FILE=<path to revision.h>

set(GIT_SHORT_SHA "")
set(GIT_TAG "")

find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${SRC_DIR}/.git")
  execute_process(
    WORKING_DIRECTORY "${SRC_DIR}"
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_SHORT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(
    WORKING_DIRECTORY "${SRC_DIR}"
    COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  # A commit can carry several tags; keep the last, matching the previous behaviour.
  if(GIT_TAG MATCHES "\n")
    string(REPLACE "\n" ";" GIT_TAG_LIST "${GIT_TAG}")
    list(LENGTH GIT_TAG_LIST GIT_TAG_LIST_LENGTH)
    math(EXPR GIT_TAG_LAST_INDEX "${GIT_TAG_LIST_LENGTH} - 1")
    list(GET GIT_TAG_LIST ${GIT_TAG_LAST_INDEX} GIT_TAG)
  endif()

  # Mark a dirty tree so a log can never imply a binary matches a clean commit.
  execute_process(
    WORKING_DIRECTORY "${SRC_DIR}"
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE GIT_DIRTY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT GIT_DIRTY STREQUAL "")
    set(GIT_SHORT_SHA "${GIT_SHORT_SHA}-dirty")
  endif()
endif()

set(NEW_CONTENTS "#define BUILT_TAG \"${GIT_TAG}\"\n#define BUILT_SHA \"${GIT_SHORT_SHA}\"\n")

# Only rewrite when the contents actually change, so an unchanged HEAD does not
# force a rebuild of everything that includes revision.h on every build.
set(OLD_CONTENTS "")
if(EXISTS "${OUT_FILE}")
  file(READ "${OUT_FILE}" OLD_CONTENTS)
endif()

if(NOT OLD_CONTENTS STREQUAL NEW_CONTENTS)
  file(WRITE "${OUT_FILE}" "${NEW_CONTENTS}")
  message(STATUS "revision.h updated: ${GIT_SHORT_SHA}")
endif()
