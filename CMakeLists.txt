cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(smba-cobra-mba VERSION 0.1.0 LANGUAGES CXX)

# The frozen SMBA boundary is intentionally built as C++23.  Do not inherit a
# compiler extension mode from a parent project: CoBRA and the offline tests
# need the same language mode as the plugin.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SMBA_BUILD_PLUGIN
    "Build the Binary Ninja adapter/plugin (OFF keeps the core build headless)"
    OFF
)
option(SMBA_BUILD_TESTS
    "Build the dependency-light offline SMBA core tests"
    ON
)
option(SMBA_REQUIRE_Z3
    "Fail configure when CoBRA's Z3 verifier is unavailable (configure-time gate)"
    OFF
)
option(SMBA_BN_ALLOW_STUBS
    "Allow Binary Ninja API stubs (compile-only; not a runnable plugin)"
    OFF
)

set(SMBA_COBRA_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../CoBRA"
    CACHE PATH
    "CoBRA source tree to add directly and link as cobra-core"
)
set(SMBA_DEPS_PREFIX
    "${SMBA_COBRA_DIR}/build-deps-smba/install"
    CACHE PATH
    "Install prefix produced by CoBRA/dependencies superbuild"
)
set(SMBA_BINARYNINJA_API_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../binaryninja-api"
    CACHE PATH
    "Binary Ninja C++ API source tree"
)

# Resolve user-supplied relative paths against this adapter directory.  The
# cache entries remain useful in CMake GUIs while all checks below use stable
# absolute paths.
get_filename_component(_smba_cobra_dir "${SMBA_COBRA_DIR}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(_smba_deps_prefix "${SMBA_DEPS_PREFIX}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(_smba_bn_api_dir "${SMBA_BINARYNINJA_API_DIR}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(SMBA_BUILD_PLUGIN
    AND (NOT EXISTS "${_smba_bn_api_dir}/CMakeLists.txt"
        OR NOT EXISTS "${_smba_bn_api_dir}/binaryninjaapi.h"))
    message(FATAL_ERROR
        "SMBA_BINARYNINJA_API_DIR does not point to a Binary Ninja API "
        "tree: '${_smba_bn_api_dir}'. Expected CMakeLists.txt and "
        "binaryninjaapi.h. "
        "Pass -DSMBA_BINARYNINJA_API_DIR=/path/to/binaryninja-api, or "
        "disable the adapter with -DSMBA_BUILD_PLUGIN=OFF.")
endif()

if(NOT EXISTS "${_smba_cobra_dir}/CMakeLists.txt"
    OR NOT EXISTS "${_smba_cobra_dir}/include/cobra/core/Expr.h")
    message(FATAL_ERROR
        "SMBA_COBRA_DIR does not point to a CoBRA source tree: "
        "'${_smba_cobra_dir}'. Expected CMakeLists.txt and "
        "include/cobra/core/Expr.h. "
        "Keep the frozen layout or pass -DSMBA_COBRA_DIR=/path/to/CoBRA.")
endif()

if(NOT IS_DIRECTORY "${_smba_deps_prefix}")
    message(FATAL_ERROR
        "SMBA_DEPS_PREFIX is missing: '${_smba_deps_prefix}'. "
        "Configure and build CoBRA's dependency superbuild first, for example "
        "cmake -S '${_smba_cobra_dir}/dependencies' "
        "-B '${_smba_cobra_dir}/build-deps-smba' "
        "-DCOBRA_ENABLE_Z3=ON, then pass "
        "-DSMBA_DEPS_PREFIX='${_smba_cobra_dir}/build-deps-smba/install'.")
endif()

# A prefix containing only a directory is not enough: the in-tree CoBRA
# project resolves absl and highway through package config files.  Check the
# two mandatory package families here so a typo is reported at this adapter's
# boundary instead of as an opaque nested find_package failure.
function(_smba_prefix_has_config out_var package_name)
    file(GLOB _smba_configs LIST_DIRECTORIES FALSE
        "${_smba_deps_prefix}/lib/cmake/${package_name}/*Config.cmake"
        "${_smba_deps_prefix}/lib/cmake/${package_name}/*-config.cmake"
        "${_smba_deps_prefix}/lib64/cmake/${package_name}/*Config.cmake"
        "${_smba_deps_prefix}/lib64/cmake/${package_name}/*-config.cmake"
        "${_smba_deps_prefix}/share/${package_name}/*Config.cmake"
        "${_smba_deps_prefix}/share/${package_name}/*-config.cmake"
    )
    if(_smba_configs)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

_smba_prefix_has_config(_smba_has_absl absl)
_smba_prefix_has_config(_smba_has_hwy hwy)
if(NOT _smba_has_absl OR NOT _smba_has_hwy)
    message(FATAL_ERROR
        "SMBA_DEPS_PREFIX='${_smba_deps_prefix}' is not a complete CoBRA "
        "dependency prefix. Expected package config files for both absl and "
        "hwy below lib/cmake (or lib64/cmake/share). Re-run the dependency "
        "superbuild with -B '<build-deps>/build-deps-smba' and pass its "
        "install directory, or point SMBA_DEPS_PREFIX at an existing install. "
        "absl=${_smba_has_absl}, hwy=${_smba_has_hwy}.")
endif()

# CoBRA is deliberately consumed from ../CoBRA, rather than through a copied
# archive or an independently installed adapter package.  Its own CMakeLists
# performs the mandatory absl/highway checks and optionally creates
# cobra-verify when a Z3 package is present.
list(PREPEND CMAKE_PREFIX_PATH "${_smba_deps_prefix}")
set(COBRA_BUILD_TESTS OFF CACHE BOOL "Disable CoBRA's tests in the adapter build" FORCE)
set(COBRA_BUILD_LLVM_PASS OFF CACHE BOOL "Disable CoBRA's LLVM pass in the adapter build" FORCE)
add_subdirectory("${_smba_cobra_dir}" "${CMAKE_BINARY_DIR}/cobra" EXCLUDE_FROM_ALL)

if(NOT TARGET cobra-core)
    message(FATAL_ERROR
        "The CoBRA tree at '${_smba_cobra_dir}' did not provide target "
        "'cobra-core'; cannot build the frozen SMBA core boundary.")
endif()

add_library(smba-core STATIC src/Engine.cpp)
add_library(smba::core ALIAS smba-core)
set_target_properties(smba-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(smba-core PUBLIC cxx_std_23)
target_include_directories(smba-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
# Engine.h exposes cobra::Expr in its public API, so both CoBRA's include
# directory and static link dependency must propagate to consumers/tests.
target_link_libraries(smba-core PUBLIC cobra-core)

set(_smba_z3_available OFF)
find_package(Z3 QUIET CONFIG)
if(TARGET cobra-verify)
    set(_smba_z3_available ON)
    target_compile_definitions(smba-core PRIVATE SMBA_HAS_Z3=1)
    target_link_libraries(smba-core PRIVATE cobra-verify)
    message(STATUS "SMBA: CoBRA Z3 verifier detected (cobra-verify)")
elseif(Z3_FOUND)
    message(WARNING
        "SMBA: a Z3 package was detected, but the in-tree CoBRA project did "
        "not provide cobra-verify; strict verification remains unavailable")
else()
    message(STATUS
        "SMBA: CoBRA Z3 verifier not detected; TransformFunction requires "
        "a Z3 proof and will reject candidates")
endif()

if(SMBA_REQUIRE_Z3 AND NOT _smba_z3_available)
    message(FATAL_ERROR
        "SMBA_REQUIRE_Z3=ON but CoBRA's cobra-verify target is unavailable. "
        "Build the dependency superbuild with -DCOBRA_ENABLE_Z3=ON, or set "
        "SMBA_REQUIRE_Z3=OFF only for offline/core development builds. "
        "The runtime TransformFunction path still requires a Z3 proof.")
endif()

# A registered workflow activity performs automatic MLIL application through
# TransformFunction/AnalysisContext. It has no probabilistic fallback, so a
# plugin build is never valid without CoBRA's verifier, even if the optional
# core-only configure gate above was disabled.
if(SMBA_BUILD_PLUGIN AND NOT _smba_z3_available)
    message(FATAL_ERROR
        "SMBA_BUILD_PLUGIN=ON requires CoBRA's cobra-verify target. Automatic "
        "workflow application is Z3-only; configure a core-only build with "
        "SMBA_BUILD_PLUGIN=OFF when Z3 is unavailable.")
endif()

if(SMBA_BUILD_TESTS)
    enable_testing()
    add_executable(smba-core-tests tests/core_tests.cpp)
    target_compile_features(smba-core-tests PRIVATE cxx_std_23)
    target_link_libraries(smba-core-tests PRIVATE smba-core)
    if(_smba_z3_available)
        target_compile_definitions(smba-core-tests PRIVATE SMBA_HAS_Z3=1)
    endif()
    # This target intentionally links only smba-core/cobra-core (and, when
    # available, cobra-verify).  It must never pull binaryninjaapi or a BN
    # runtime so it can run in CI and on a dependency-only host.
    add_test(NAME smba-core-tests COMMAND smba-core-tests)
endif()

if(SMBA_BUILD_PLUGIN)
    # Build the API headless and without examples; the plugin itself is the
    # only Binary Ninja-facing target.  BN_ALLOW_STUBS is opt-in because a
    # stub-linked module cannot be loaded by a real Binary Ninja installation.
    set(BN_API_BUILD_EXAMPLES OFF CACHE BOOL "Disable Binary Ninja API examples" FORCE)
    set(HEADLESS ON CACHE BOOL "Build Binary Ninja API headless" FORCE)
    set(BN_ALLOW_STUBS "${SMBA_BN_ALLOW_STUBS}" CACHE BOOL
        "Allow Binary Ninja API stubs" FORCE)
    add_subdirectory("${_smba_bn_api_dir}" "${CMAKE_BINARY_DIR}/binaryninja-api" EXCLUDE_FROM_ALL)

    if(NOT TARGET binaryninjaapi)
        message(FATAL_ERROR
            "Binary Ninja API tree '${_smba_bn_api_dir}' did not provide "
            "target 'binaryninjaapi'.")
    endif()

    add_library(smba-cobra-mba SHARED
        src/BNAdapter.cpp
        src/Plugin.cpp
    )
    add_library(smba::plugin ALIAS smba-cobra-mba)
    target_compile_features(smba-cobra-mba PRIVATE cxx_std_23)
    target_include_directories(smba-cobra-mba
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${_smba_bn_api_dir}"
    )
    target_link_libraries(smba-cobra-mba PRIVATE smba-core binaryninjaapi)
    set_target_properties(smba-cobra-mba PROPERTIES
        PREFIX ""
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
    )

    if(COMMAND bn_install_plugin)
        bn_install_plugin(smba-cobra-mba)
    else()
        message(WARNING
            "Binary Ninja API did not define bn_install_plugin; the plugin "
            "will build but no default install rule is available.")
    endif()
endif()

message(STATUS "SMBA: C++ standard ${CMAKE_CXX_STANDARD}")
message(STATUS "SMBA: CoBRA source ${_smba_cobra_dir}")
message(STATUS "SMBA: dependency prefix ${_smba_deps_prefix}")
message(STATUS "SMBA: plugin=${SMBA_BUILD_PLUGIN}, tests=${SMBA_BUILD_TESTS}")
