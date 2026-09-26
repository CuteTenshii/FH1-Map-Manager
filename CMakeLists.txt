cmake_minimum_required(VERSION 3.21)

project(ForzaMapManager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FH1_BUILD_TESTS "Build the unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Sql Concurrent)
find_package(ZLIB REQUIRED)

qt_standard_project_setup()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Formats and data loading, independent of any widget code.
add_library(fh1core STATIC
    src/core/Activities.cpp
    src/core/Activities.h
    src/core/ForzaZip.cpp
    src/core/ForzaZip.h
    src/core/GameDatabase.cpp
    src/core/GameDatabase.h
    src/core/GameInstall.cpp
    src/core/GameInstall.h
    src/core/Loaders.cpp
    src/core/Loaders.h
    src/core/Lzx.cpp
    src/core/Lzx.h
    src/core/MapCalibration.cpp
    src/core/MapCalibration.h
    src/core/MapData.h
    src/core/MapLoader.cpp
    src/core/MapLoader.h
    src/core/RenderMesh.cpp
    src/core/RenderMesh.h
    src/core/StringTable.cpp
    src/core/StringTable.h
    src/core/TrackTextures.cpp
    src/core/TrackTextures.h
    src/core/WorldIndex.cpp
    src/core/WorldIndex.h
    src/core/WorldTiles.cpp
    src/core/WorldTiles.h
    src/core/XboxTexture.cpp
    src/core/XboxTexture.h
)
target_include_directories(fh1core PUBLIC src/core)
target_link_libraries(fh1core PUBLIC Qt6::Core Qt6::Gui Qt6::Sql PRIVATE Qt6::Concurrent ZLIB::ZLIB)

qt_add_executable(fh1zip tools/fh1zip.cpp)
target_link_libraries(fh1zip PRIVATE fh1core)

qt_add_executable(fh1meshscan tools/fh1meshscan.cpp)
target_link_libraries(fh1meshscan PRIVATE fh1core)

install(TARGETS fh1zip
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(FH1_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()

    function(fh1_add_test name)
        qt_add_executable(${name} tests/${name}.cpp tests/LzxTestEncoder.h)
        target_link_libraries(${name} PRIVATE ${ARGN} Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        # Widget tests run without a display.
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endfunction()

    fh1_add_test(tst_lzx fh1core)
    fh1_add_test(tst_forzazip fh1core ZLIB::ZLIB)
    fh1_add_test(tst_loaders fh1core)
    fh1_add_test(tst_labels fh1core)
    fh1_add_test(tst_realdata fh1core)
endif()
