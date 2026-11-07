# SPDX-License-Identifier: GPL-3.0-only
# OPLBend as a VST3 instrument: the tracker's firmware (../firmware), compiled for the Mac with its own
# hardware layer (native/), embedded in the plugin as a library each instance loads. See README.md.
cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")

project(OPLBend VERSION 0.3.0 LANGUAGES C CXX)

set(JUCE_DIR "$ENV{HOME}/JUCE" CACHE PATH "JUCE checkout")
set(TRACKER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(FM1_GEN_DIR "${TRACKER_DIR}/build/gen" CACHE PATH "The firmware's generated headers (made by native/gen.py)")
find_program(FM1_PYTHON NAMES python3 HINTS "${TRACKER_DIR}/../.venv/bin" NO_DEFAULT_PATH)
find_program(FM1_PYTHON NAMES python3 REQUIRED)

set(JUCE_MODULES_ONLY OFF)
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)

# Ableton Link (GPL-2.0-or-later): Live follows the song's tempo, and START starts both.
set(LINK_DIR "$ENV{HOME}/link" CACHE PATH "Ableton Link checkout (Link-3.1.3, with its submodules)")
include("${LINK_DIR}/AbletonLinkConfig.cmake")

# The firmware's generated headers, as its own build makes them (they also hold the songs it ships with).
add_custom_target(fm1_gen
    COMMAND "${FM1_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/native/gen.py"
    WORKING_DIRECTORY "${TRACKER_DIR}")

# The firmware as a library: the same sources and options as tools/build.py, with FM1_HOST.
set(FW "${TRACKER_DIR}/firmware")
add_library(fm1_native SHARED
    "${FW}/src/tracker.c"
    "${FW}/amber/moteur/CustomReplayer/md_replayer.c"
    "${FW}/amber/main.cpp"
    "${FW}/amber/md_audio.cpp")
add_dependencies(fm1_native fm1_gen)
set_target_properties(fm1_native PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native")
target_compile_definitions(fm1_native PRIVATE
    FM1_HOST FM1 ARM9 __NDS__ MD_ZOOM=100 MD_PCM_BANK_BYTES=8192 _FORTIFY_SOURCE=0)
target_include_directories(fm1_native PRIVATE
    native/hal native
    "${FW}/compat" "${FW}/amber" "${FW}/amber/import" "${FW}/amber/include" "${FW}/amber/moteur"
    "${FW}/src" "${FM1_GEN_DIR}")
target_compile_options(fm1_native PRIVATE
    -O2 -w
    "-iquote${FW}/amber/include" "-iquote${FW}/amber/moteur"
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics>)
target_link_options(fm1_native PRIVATE -Wl,-x)

juce_add_binary_data(OPLBendFirmware
    HEADER_NAME Firmware.h
    NAMESPACE firmware
    SOURCES "${CMAKE_BINARY_DIR}/native/libfm1_native.dylib")
add_dependencies(OPLBendFirmware fm1_native)

juce_add_plugin(OPLBend
    COMPANY_NAME "Audren"
    BUNDLE_ID "fr.mazmusique.oplbend"
    PLUGIN_MANUFACTURER_CODE Audr
    PLUGIN_CODE OplB
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE   # unused, but Ableton Live refuses a VST3 instrument without an event input
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Instrument Synth
    FORMATS VST3
    PRODUCT_NAME "OPLBend")

target_sources(OPLBend PRIVATE
    Source/MacKeys.mm
    Source/Machine.cpp
    Source/PluginEditor.cpp
    Source/PluginProcessor.cpp)

target_compile_definitions(OPLBend PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(OPLBend
    PRIVATE
        OPLBendFirmware
        Ableton::Link
        juce::juce_audio_utils
        "-framework Carbon"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Tests: the engine and the editor without a host, then the installed plugin in a real VST3 host.
juce_add_console_app(OPLBendTest PRODUCT_NAME "OPLBendTest")
target_sources(OPLBendTest PRIVATE
    Source/MacKeys.mm
    Source/Machine.cpp
    Source/PluginEditor.cpp
    Source/PluginProcessor.cpp
    tests/plugin_test.cpp)
target_include_directories(OPLBendTest PRIVATE Source)
target_compile_definitions(OPLBendTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="OPLBend" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
target_link_libraries(OPLBendTest PRIVATE
    OPLBendFirmware Ableton::Link juce::juce_audio_utils "-framework Carbon"
    PUBLIC juce::juce_recommended_config_flags)

juce_add_console_app(OPLBendHostTest PRODUCT_NAME "OPLBendHostTest")
target_sources(OPLBendHostTest PRIVATE tests/host_test.cpp)
target_compile_definitions(OPLBendHostTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(OPLBendHostTest PRIVATE
    juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)
