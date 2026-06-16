cmake_minimum_required(VERSION 3.16)
project(pvr.amazontv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Source files ──────────────────────────────────────────────────────────────
set(SOURCES
  src/addon.cpp
  src/ClientInstance.cpp
  src/AmazonTVData.cpp
)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
# The easiest approach is the single-header version:
#   Place json.hpp in src/nlohmann/json.hpp
# Or find it via the system package / FetchContent:
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# ── Kodi addon build system ───────────────────────────────────────────────────
# When building as a Kodi addon, the Kodi cmake macros are already in scope.
# For a standalone build you can point KODI_DIR at your Kodi source tree.

find_package(Kodi REQUIRED)

build_addon(pvr.amazontv SOURCES "${SOURCES}" DEPENDS nlohmann_json::nlohmann_json)
