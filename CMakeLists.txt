cmake_minimum_required(VERSION 3.22)
project(DeHowl VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

# JUCE is downloaded automatically the first time you configure the project
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(DeHowl
    COMPANY_NAME "Shamaapps"
    COMPANY_WEBSITE "https://shamaapps.com"
    BUNDLE_ID com.shamaapps.dehowl
    PLUGIN_MANUFACTURER_CODE Shma
    PLUGIN_CODE Dhwl
    # Standalone app is the primary target.
    # To also build DAW plug-ins, change the next line to:  FORMATS Standalone AU VST3
    FORMATS Standalone
    PRODUCT_NAME "DeHowl"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon_small.png"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "DeHowl needs audio input to detect feedback."
    VST3_CATEGORIES Fx Restoration
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(DeHowl PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(DeHowl PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(DeHowl
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Link-time optimisation on macOS release builds (small extra speed, free)
if(APPLE)
    target_link_libraries(DeHowl PUBLIC juce::juce_recommended_lto_flags)
endif()
