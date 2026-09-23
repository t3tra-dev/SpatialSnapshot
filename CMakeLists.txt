cmake_minimum_required(VERSION 3.20)
project(SpatialSnapshot VERSION 1.0.0 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
option(SS_BUILD_TOOLS "Build SSPS command line tools" ON)
option(SS_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SS_BUILD_FUZZERS "Build libFuzzer entry points (Clang)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(SpatialSnapshotC
    Sources/SpatialSnapshotC/core.c
    Sources/SpatialSnapshotC/codec.c
    Sources/SpatialSnapshotC/document.c
    Sources/SpatialSnapshotC/scene.c
    Sources/SpatialSnapshotC/geometry.c
    Sources/SpatialSnapshotC/writer.c
    Sources/SpatialSnapshotC/trim.c
    Sources/SpatialSnapshotC/bmff.c
    Sources/SpatialSnapshotC/heif.c
    Sources/SpatialSnapshotC/quicktime.c
    Sources/SpatialSnapshotC/orientation.c
    Sources/SpatialSnapshotC/media_config.c)
add_library(SpatialSnapshot::C ALIAS SpatialSnapshotC)
set_target_properties(SpatialSnapshotC PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    OUTPUT_NAME spatialsnapshot VERSION ${PROJECT_VERSION} SOVERSION 1
    EXPORT_NAME C POSITION_INDEPENDENT_CODE YES)
target_include_directories(SpatialSnapshotC PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Sources/SpatialSnapshotC/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(SpatialSnapshotC PUBLIC SS_SHARED PRIVATE SS_BUILDING_LIBRARY)
endif()
if(NOT MSVC)
    target_compile_options(SpatialSnapshotC PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_link_libraries(SpatialSnapshotC PUBLIC m)
    set(SS_PC_PRIVATE_LIBS "-lm")
endif()
if(SS_ENABLE_SANITIZERS)
    target_compile_options(SpatialSnapshotC PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(SpatialSnapshotC PUBLIC -fsanitize=address,undefined)
endif()
if(SS_BUILD_TOOLS)
    foreach(tool ssinspect ssvalidate ssdump)
        add_executable(${tool} Tools/sscli.c)
        target_link_libraries(${tool} PRIVATE SpatialSnapshotC)
        if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
            set_target_properties(${tool} PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_LIBDIR}")
        elseif(APPLE)
            set_target_properties(${tool} PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
        elseif(UNIX)
            set_target_properties(${tool} PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
        endif()
        install(TARGETS ${tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endforeach()
endif()
if(BUILD_TESTING)
    add_executable(ss_binding_tests Tests/Bindings/test_bindings.c)
    target_link_libraries(ss_binding_tests PRIVATE SpatialSnapshotC)
    target_compile_definitions(ss_binding_tests PRIVATE SS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Fixtures")
    add_test(NAME ss_binding_tests COMMAND ss_binding_tests)
    add_executable(ss_core_tests Tests/CoreTests/test_core.c)
    target_link_libraries(ss_core_tests PRIVATE SpatialSnapshotC)
    target_compile_definitions(ss_core_tests PRIVATE SS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Fixtures")
    find_path(SS_LZ4_INCLUDE_DIR lz4.h)
    find_library(SS_LZ4_LIBRARY NAMES lz4)
    if(SS_LZ4_INCLUDE_DIR AND SS_LZ4_LIBRARY)
        target_include_directories(ss_core_tests PRIVATE ${SS_LZ4_INCLUDE_DIR})
        target_link_libraries(ss_core_tests PRIVATE ${SS_LZ4_LIBRARY})
        target_compile_definitions(ss_core_tests PRIVATE SS_HAVE_LZ4=1)
    endif()
    add_test(NAME ss_core_tests COMMAND ss_core_tests)
    find_package(Python3 3.10 COMPONENTS Interpreter)
    if(Python3_FOUND AND SS_BUILD_TOOLS)
        add_test(NAME ss_binding_conformance COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Bindings/test_bindings.py $<TARGET_FILE:ssvalidate>)
        add_test(NAME ss_conformance COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Conformance/test_conformance.py $<TARGET_FILE:ssvalidate>)
    endif()
endif()
if(SS_BUILD_FUZZERS)
    foreach(fuzzer ssps geometry lz4 bindings)
        add_executable(fuzz_${fuzzer} Fuzz/${fuzzer}.c)
        target_link_libraries(fuzz_${fuzzer} PRIVATE SpatialSnapshotC)
        target_compile_options(fuzz_${fuzzer} PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_${fuzzer} PRIVATE -fsanitize=fuzzer)
    endforeach()
    target_compile_options(SpatialSnapshotC PRIVATE -fsanitize=fuzzer-no-link)
endif()
install(TARGETS SpatialSnapshotC EXPORT SpatialSnapshotTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY Sources/SpatialSnapshotC/include/spatialsnapshot DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES Sources/SpatialSnapshotC/include/module.modulemap DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SpatialSnapshotTargets NAMESPACE SpatialSnapshot:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SpatialSnapshot)
configure_file(cmake/SpatialSnapshotConfig.cmake.in SpatialSnapshotConfig.cmake @ONLY)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/SpatialSnapshotConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
get_filename_component(SS_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}" ABSOLUTE BASE_DIR "${CMAKE_INSTALL_PREFIX}")
get_filename_component(SS_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}" ABSOLUTE BASE_DIR "${CMAKE_INSTALL_PREFIX}")
file(RELATIVE_PATH SS_PC_PREFIX_RELATIVE "${SS_PC_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
file(RELATIVE_PATH SS_PC_INCLUDE_RELATIVE "${SS_PC_LIBDIR}/pkgconfig" "${SS_PC_INCLUDEDIR}")
configure_file(cmake/spatialsnapshot.pc.in spatialsnapshot.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SpatialSnapshotConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/SpatialSnapshotConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SpatialSnapshot)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/spatialsnapshot.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/SpatialSnapshot)
