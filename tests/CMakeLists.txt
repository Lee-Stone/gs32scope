# 单元测试
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui)

add_executable(test_elf_parser
    test_elf_parser.cpp
    ${CMAKE_SOURCE_DIR}/app/symbols/elf_parser.cpp
)
target_include_directories(test_elf_parser PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(test_elf_parser PRIVATE Qt6::Core)

add_executable(test_dwarf_parser
    test_dwarf_parser.cpp
    ${CMAKE_SOURCE_DIR}/app/symbols/dwarf_parser.cpp
)
target_include_directories(test_dwarf_parser PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(test_dwarf_parser PRIVATE Qt6::Core)

add_executable(test_dwarf_real
    test_dwarf_real.cpp
    ${CMAKE_SOURCE_DIR}/app/symbols/dwarf_parser.cpp
)
target_include_directories(test_dwarf_real PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(test_dwarf_real PRIVATE Qt6::Core)

add_executable(test_monitor_model
    test_monitor_model.cpp
    ${CMAKE_SOURCE_DIR}/app/core/monitor_model.cpp
)
target_include_directories(test_monitor_model PRIVATE ${CMAKE_SOURCE_DIR}/app)
target_link_libraries(test_monitor_model PRIVATE Qt6::Core Qt6::Gui)
