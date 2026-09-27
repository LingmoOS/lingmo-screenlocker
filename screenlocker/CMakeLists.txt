set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(PROJECT_SOURCES
    main.cpp
    application.cpp
    authenticator.cpp
    kcheckpass-enums.h
    fixx11h.h
    qml.qrc
)

add_executable(lingmo-screenlocker
    ${PROJECT_SOURCES}
)

target_link_libraries(lingmo-screenlocker
    PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::Quick

    X11::X11
    X11::xcb
)

install(TARGETS lingmo-screenlocker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
