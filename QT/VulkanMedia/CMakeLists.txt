cmake_minimum_required(VERSION 3.21)

project(VulkanMedia VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 Concurrent Multimedia)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(appVulkanMedia
    src/main.cpp
    src/filesystemmodel.h      src/filesystemmodel.cpp
    src/thumbnailprovider.h    src/thumbnailprovider.cpp
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT appVulkanMedia)

# The generated qmltyperegistrations.cpp includes the QML_ELEMENT headers by
# bare name, so src/ has to be on the include path.
target_include_directories(appVulkanMedia PRIVATE src)

# Theme.qml is a QML singleton. With qt_add_qml_module the qmldir is generated,
# so the singleton is declared through this source property (not a hand-written
# qmldir entry).
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(appVulkanMedia
    URI VulkanMedia
    VERSION 1.0
    QML_FILES
        qml/Theme.qml
        qml/Main.qml
        qml/MenuBarRow.qml
        qml/ToolBarRow.qml
        qml/LauncherPanel.qml
        qml/PlacesList.qml
        qml/FileGrid.qml
        qml/DetailsPanel.qml
        qml/CommandBar.qml
        qml/StatusBar.qml
        qml/PanelMenu.qml
        qml/VideoPanel.qml
        qml/PlaceholderPanel.qml
        qml/components/IconButton.qml
        qml/components/SegmentedControl.qml
        qml/components/Chip.qml
        qml/components/PanelHeader.qml
        qml/components/KeyValueRow.qml
        qml/Format.js
)

set_target_properties(appVulkanMedia PROPERTIES
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appVulkanMedia PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    Qt6::Concurrent
    Qt6::Multimedia
)

if(MSVC)
    get_filename_component(QT6_INSTALL_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
    set_target_properties(appVulkanMedia PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "PATH=${QT6_INSTALL_PREFIX}/bin;%PATH%
QML2_IMPORT_PATH=${QT6_INSTALL_PREFIX}/qml"
    )
endif()

install(TARGETS appVulkanMedia
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
