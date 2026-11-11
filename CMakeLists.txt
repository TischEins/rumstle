cmake_minimum_required(VERSION 3.25.0)

project(smtg-vst3-rumstle
    VERSION 1.12.0
    DESCRIPTION "Kastle VCO + LFO/Rungler port — VST3 instrument with patch-cable editor"
)

if(NOT SMTG_ENABLE_VSTGUI_SUPPORT)
    return()
endif()

set(kastleport_sources
    ${SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    ${SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.h
    kastle_vst3.cpp
    kastle_vst3.h
    kastle_vco.c
    kastle_vco.h
    kastle_lfo.c
    kastle_lfo.h
    kastle_noise_table.c
    kastle_noise_table.h
    kastle_editor.cpp
    kastle_editor.h
    resource/kastleport.uidesc
)

set(target rumstle)
smtg_add_vst3plugin(${target} ${kastleport_sources})

target_compile_features(${target} PUBLIC cxx_std_17)
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${target} PRIVATE sdk vstgui_support)

# The .uidesc has to sit in the bundle next to the binary; VST3Editor loads it
# from there by name at runtime.
smtg_target_add_plugin_resources(${target}
    RESOURCES
        resource/kastleport.uidesc
        resource/plate.png
        resource/plate_2x.png
        resource/knob_body.png
        resource/knob_body_2x.png
        resource/trim_body.png
        resource/trim_body_2x.png
        resource/trim.png
        resource/trim_2x.png
        resource/knob.png
        resource/knob_2x.png
        resource/switch_on.png
        resource/switch_on_2x.png
        resource/switch_off.png
        resource/switch_off_2x.png
        resource/led_on.png
        resource/led_on_2x.png
        resource/led_off.png
        resource/led_off_2x.png
)

# VSTGUI's built-in WYSIWYG editor. The SDK normally ties this to Debug builds;
# this option turns it on for any configuration so the UI can be designed in a
# Release build too. Switch OFF before shipping.
option(KASTLEPORT_LIVE_EDITING "Enable the VSTGUI inline UI editor" OFF)
if(KASTLEPORT_LIVE_EDITING)
    target_compile_definitions(${target} PRIVATE VSTGUI_LIVE_EDITING=1)
    # The context menu that opens the editor lives in vst3editor.cpp and the
    # editor itself in the uidescription library, so the definition has to
    # reach those targets too -- setting it on this plugin alone does nothing.
    foreach(vstgui_target vstgui vstgui_uidescription vstgui_support)
        if(TARGET ${vstgui_target})
            target_compile_definitions(${vstgui_target} PUBLIC VSTGUI_LIVE_EDITING=1)
        endif()
    endforeach()
endif()

# Standalone: the SDK's editorhost window code plus a WASAPI audio thread. It
# loads the bundle that sits next to it, so the bundle is copied there.
if(SMTG_WIN)
    set(eh ${SDK_ROOT}/public.sdk/samples/vst-hosting/editorhost/source)
    add_executable(rumstle_standalone
        kastle_standalone.cpp
        ${eh}/usediids.cpp
        ${eh}/platform/win32/platform.cpp
        ${eh}/platform/win32/window.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp
        ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
    )
    target_compile_features(rumstle_standalone PUBLIC cxx_std_17)
    target_link_libraries(rumstle_standalone PRIVATE sdk_hosting)
    set_target_properties(rumstle_standalone PROPERTIES LINK_FLAGS "/SUBSYSTEM:windows" OUTPUT_NAME RUMSTLE)
    add_dependencies(rumstle_standalone ${target})
    add_custom_command(TARGET rumstle_standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_FILE_DIR:${target}>/../.." "$<TARGET_FILE_DIR:rumstle_standalone>/rumstle.vst3"
    )
endif()

if(SMTG_MAC)
    smtg_target_set_bundle(${target}
        BUNDLE_IDENTIFIER "org.tischeins.vst3.rumstle"
        COMPANY_NAME "TischEins"
    )
elseif(SMTG_WIN)
    smtg_target_set_debug_executable(${target}
        "$(ProgramW6432)/Steinberg/VST3PluginTestHost/VST3PluginTestHost.exe"
        "-graph \"$(TargetPath)\""
    )
endif()
