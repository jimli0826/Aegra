find_package(
    Qt6 6.8 REQUIRED
    COMPONENTS Core Gui Network Qml Quick QuickControls2
)

add_executable(aegra_desktop WIN32
    resources/desktop.rc
    resources/resources.qrc
    src/main.cpp
    src/service_client.cpp
    src/service_client.h
)

set_target_properties(
    aegra_desktop
    PROPERTIES
        AUTOMOC ON
        AUTORCC ON
)

target_link_libraries(
    aegra_desktop
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
)
aegra_configure_target(aegra_desktop)
