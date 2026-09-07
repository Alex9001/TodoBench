cmake_minimum_required(VERSION 3.24)

project(TodoBench VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/utf-8)

endif()

option(TODOBENCH_BUILD_TESTS "Build TodoBench tests" ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg Concurrent Test Network)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(cmark-gfm CONFIG QUIET)
find_package(cmark-gfm-extensions CONFIG QUIET)
find_library(CMARK_EXTENSIONS_LIBRARY NAMES cmark-gfm-extensions)
find_path(CMARK_INCLUDE_DIR NAMES cmark-gfm.h)
find_library(CMARK_LIBRARY NAMES cmark-gfm cmark)

add_library(todobench_core
    native/src/domain/model.cpp
    native/src/domain/markdown_validation.cpp
    native/src/domain/filter.cpp
    native/src/domain/filter_session.cpp
    native/src/domain/project_filter_labels.cpp
    native/src/domain/task_sort.cpp
    native/src/domain/formatting_rules.cpp
    native/src/domain/keyboard_bindings.cpp
    native/src/domain/reminders.cpp
    native/src/domain/reminder_scheduler.cpp
    native/src/domain/recurrence.cpp
    native/src/storage/front_matter_codec.cpp
    native/src/storage/workspace_scanner.cpp
    native/src/storage/workspace_store.cpp
    native/src/storage/tutorial_workspace.cpp
    native/src/storage/workspace_creation.cpp
    native/src/storage/sample_workspaces.cpp
    native/resources/workflows.qrc
    native/src/storage/settings_codec.cpp
    native/src/storage/conflict_copy.cpp
    native/src/storage/schedule_validation.cpp
    native/src/storage/history_store.cpp
    native/src/storage/archive_safety.cpp
    native/src/storage/workspace_archive.cpp
    native/src/storage/recovery_journal.cpp
    native/src/storage/trash_store.cpp
    native/src/storage/external_reconciler.cpp
    native/src/storage/workspace_monitor.cpp
    native/src/storage/attachment_store.cpp
    native/src/storage/workspace_lock.cpp
    native/src/storage/login_item.cpp
    native/src/storage/machine_state.cpp
)

target_include_directories(todobench_core PUBLIC native/src)
target_link_libraries(todobench_core PUBLIC Qt6::Core yaml-cpp::yaml-cpp LibArchive::LibArchive)
target_compile_features(todobench_core PUBLIC cxx_std_20)
if(TARGET libcmark-gfm AND TARGET libcmark-gfm-extensions)
    target_compile_definitions(todobench_core PRIVATE TODOBENCH_HAS_CMARK=1)
    target_link_libraries(todobench_core PRIVATE libcmark-gfm libcmark-gfm-extensions)
elseif(CMARK_INCLUDE_DIR AND CMARK_LIBRARY AND CMARK_EXTENSIONS_LIBRARY)
    target_compile_definitions(todobench_core PRIVATE TODOBENCH_HAS_CMARK=1)
    target_include_directories(todobench_core PRIVATE ${CMARK_INCLUDE_DIR})
    target_link_libraries(todobench_core PRIVATE ${CMARK_LIBRARY} ${CMARK_EXTENSIONS_LIBRARY})
else()
    message(STATUS "cmark-gfm not found: visual Markdown editing uses the Qt Markdown subset")
endif()

target_compile_options(todobench_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_library(todobench_icons native/src/app/icons.cpp native/resources/icons.qrc)
target_include_directories(todobench_icons PUBLIC native/src)
target_link_libraries(todobench_icons PUBLIC Qt6::Widgets Qt6::Svg)
target_link_libraries(todobench_core PUBLIC todobench_icons)
target_compile_definitions(todobench_core PUBLIC TODOBENCH_VERSION="${PROJECT_VERSION}")

add_library(todobench_theme
    native/src/app/theme.cpp
    native/src/app/theme_editor.cpp
)
target_link_libraries(todobench_theme PUBLIC todobench_core Qt6::Widgets)

add_executable(TodoBench
    native/src/app/main.cpp
    native/src/app/workspace_controller.cpp
    native/src/app/main_window.cpp
    native/src/app/onboarding_wizard.cpp
    native/src/app/markdown_editor.cpp
    native/src/app/task_schedule_editor.cpp
    native/src/app/settings_dialog.cpp
    native/src/app/instance_guard.cpp
)
target_link_libraries(TodoBench PRIVATE todobench_theme Qt6::Gui Qt6::Widgets Qt6::Network)
set_target_properties(TodoBench PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME TodoBench
)

include(CTest)
if(TODOBENCH_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(test_interoperability native/tests/test_interoperability.cpp native/src/app/markdown_editor.cpp)
    target_link_libraries(test_interoperability PRIVATE todobench_core Qt6::Test)
    add_test(NAME test_interoperability COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_interoperability>)
    set_tests_properties(test_interoperability PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(interop_driver native/tests/interop_driver.cpp)
    target_link_libraries(interop_driver PRIVATE todobench_core)
    add_test(NAME reference_interop COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/reference-interop.py $<TARGET_FILE:interop_driver>)

    add_executable(test_front_matter native/tests/test_front_matter.cpp)
    target_link_libraries(test_front_matter PRIVATE todobench_core Qt6::Test)
    target_compile_features(test_front_matter PRIVATE cxx_std_20)
    add_test(NAME test_front_matter COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_front_matter>)

    add_executable(test_project_filter_labels native/tests/test_project_filter_labels.cpp)
    target_link_libraries(test_project_filter_labels PRIVATE todobench_core Qt6::Test)
    add_test(NAME test_project_filter_labels COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_project_filter_labels>)

    add_executable(test_domain native/tests/test_domain.cpp)
    target_link_libraries(test_domain PRIVATE todobench_core Qt6::Test)
    target_compile_features(test_domain PRIVATE cxx_std_20)
    add_test(NAME test_domain COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_domain>)

    add_executable(test_reliable_storage native/tests/test_reliable_storage.cpp)
    target_link_libraries(test_reliable_storage PRIVATE todobench_core Qt6::Widgets Qt6::Test)
    target_sources(test_reliable_storage PRIVATE native/src/app/workspace_controller.cpp)
    target_compile_features(test_reliable_storage PRIVATE cxx_std_20)
    add_test(NAME test_reliable_storage COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_reliable_storage>)

    add_executable(test_editor_attachment
        native/tests/test_editor_attachment.cpp
        native/src/app/markdown_editor.cpp
    )
    target_link_libraries(test_editor_attachment PRIVATE todobench_core Qt6::Widgets Qt6::Test)
    target_compile_features(test_editor_attachment PRIVATE cxx_std_20)
    add_test(NAME test_editor_attachment COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_editor_attachment>)

    add_executable(test_task_commands native/tests/test_task_commands.cpp)
    target_link_libraries(test_task_commands PRIVATE todobench_core Qt6::Widgets Qt6::Test)
    target_sources(test_task_commands PRIVATE native/src/app/workspace_controller.cpp)
    target_compile_features(test_task_commands PRIVATE cxx_std_20)
    add_test(NAME test_task_commands COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_task_commands>)

    add_executable(test_views native/tests/test_views.cpp)
    target_link_libraries(test_views PRIVATE todobench_core Qt6::Test)
    target_compile_features(test_views PRIVATE cxx_std_20)
    add_test(NAME test_views COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_views>)

    add_executable(test_archive native/tests/test_archive.cpp)
    target_link_libraries(test_archive PRIVATE todobench_core Qt6::Test)
    target_compile_features(test_archive PRIVATE cxx_std_20)
    add_test(NAME test_archive COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_archive>)

    add_executable(test_theme native/tests/test_theme.cpp)
    target_link_libraries(test_theme PRIVATE todobench_theme Qt6::Test)
    add_test(NAME test_theme COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_theme>)

    add_executable(test_sample_workspaces native/tests/test_sample_workspaces.cpp)
    target_link_libraries(test_sample_workspaces PRIVATE todobench_core Qt6::Test)
    add_test(NAME test_sample_workspaces COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_sample_workspaces>)

    add_executable(test_onboarding native/tests/test_onboarding.cpp native/src/app/onboarding_wizard.cpp)
    target_link_libraries(test_onboarding PRIVATE todobench_theme Qt6::Test)
    add_test(NAME test_onboarding COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_onboarding>)

    add_executable(test_tutorial_workspace native/tests/test_tutorial_workspace.cpp)
    target_link_libraries(test_tutorial_workspace PRIVATE todobench_core Qt6::Test)
    add_test(NAME test_tutorial_workspace COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_tutorial_workspace>)

    add_executable(test_workspace_scanner native/tests/test_workspace_scanner.cpp)
    target_link_libraries(test_workspace_scanner PRIVATE todobench_core Qt6::Test)
    target_compile_features(test_workspace_scanner PRIVATE cxx_std_20)
    add_test(NAME test_workspace_scanner COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_workspace_scanner>)

    add_executable(test_task_schedule_editor
        native/tests/test_task_schedule_editor.cpp
        native/src/app/task_schedule_editor.cpp
    )
    target_link_libraries(test_task_schedule_editor PRIVATE todobench_core Qt6::Widgets Qt6::Test)
    target_compile_features(test_task_schedule_editor PRIVATE cxx_std_20)
    add_test(NAME test_task_schedule_editor COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_task_schedule_editor>)

    add_executable(test_main_window
        native/tests/test_main_window.cpp
        native/src/app/main_window.cpp
        native/src/app/onboarding_wizard.cpp
        native/src/app/markdown_editor.cpp
        native/src/app/task_schedule_editor.cpp
        native/src/app/settings_dialog.cpp
        native/src/app/workspace_controller.cpp
    )
    target_link_libraries(test_main_window PRIVATE todobench_theme Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test)
    target_compile_features(test_main_window PRIVATE cxx_std_20)
    add_test(NAME test_main_window COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run-qt-test.py $<TARGET_FILE:test_main_window>)

    set_tests_properties(
        test_front_matter
        test_domain
        test_project_filter_labels
        test_reliable_storage
        test_editor_attachment
        test_task_commands
        test_views
        test_archive
        test_workspace_scanner
        test_tutorial_workspace
        test_sample_workspaces
        test_onboarding
        test_theme
        test_task_schedule_editor
        test_main_window
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()

include(GNUInstallDirs)
install(TARGETS TodoBench
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .
)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/TodoBench OPTIONAL)
install(FILES LICENSE README.md packaging/THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/TodoBench)
install(FILES packaging/linux/todobench.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/linux/todobench.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/linux/com.todobench.TodoBench.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

option(TODOBENCH_BUNDLE_RUNTIME "Bundle Qt runtime files next to the application" OFF)
if(TODOBENCH_BUNDLE_RUNTIME)
    qt_generate_deploy_app_script(
        TARGET TodoBench
        OUTPUT_SCRIPT todobench_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${todobench_deploy_script})
endif()

if(WIN32)
    configure_file(packaging/windows/todobench.rc.in ${CMAKE_CURRENT_BINARY_DIR}/todobench.rc @ONLY)
    target_sources(TodoBench PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/todobench.rc)
elseif(APPLE)
    target_sources(TodoBench PRIVATE packaging/icons/todobench.icns)
    set_source_files_properties(packaging/icons/todobench.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(TodoBench PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks"
        MACOSX_BUNDLE_ICON_FILE todobench.icns
        MACOSX_BUNDLE_GUI_IDENTIFIER com.todobench.TodoBench
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
endif()
install(DIRECTORY native/resources/icons/lucide/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/TodoBench/lucide)

install(DIRECTORY packaging/licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/TodoBench/licenses)

# Let CMake merge the application manifest through its native manifest tool.
if(MSVC)
    get_property(native_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(native_target IN LISTS native_targets)
        get_target_property(native_type ${native_target} TYPE)
        if(native_type STREQUAL "EXECUTABLE")
            target_sources(${native_target} PRIVATE packaging/windows/long-paths.manifest)
        endif()
    endforeach()
endif()
