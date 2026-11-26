cmake_minimum_required(VERSION 3.16)
project(kusak_v13 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build tipi ayarla (varsayılan: RelWithDebInfo) ────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ── Research binary opsiyonu ──────────────────────────────────────────────────
option(BUILD_RESEARCH "Build research binary" OFF)

# ── nlohmann/json ────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
# ─────────────────────────────────────────────────────────────────────────────

# Ortak include dizinleri
set(KUSAK_INCLUDES
    ${CMAKE_SOURCE_DIR}/include
)

# ── Ana binary ───────────────────────────────────────────────────────────────
add_executable(kusak_v13
    src/main.cpp
)

target_include_directories(kusak_v13 PRIVATE ${KUSAK_INCLUDES})

target_link_libraries(kusak_v13 PRIVATE
    nlohmann_json::nlohmann_json
    pthread
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
)

target_compile_options(kusak_v13 PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Debug>:-g3 -O0 -fno-omit-frame-pointer>
    $<$<CONFIG:RelWithDebInfo>:-g -O2 -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
)

# ── Research binary (varsayılan: OFF) ────────────────────────────────────────
if(BUILD_RESEARCH)
    add_executable(research
        src/research_main.cpp
        src/app/research/research_cli.cpp
        src/app/research/backtest_runner.cpp
        src/app/research/replay_runner.cpp
    )

    target_include_directories(research PRIVATE ${KUSAK_INCLUDES})

    target_link_libraries(research PRIVATE
        nlohmann_json::nlohmann_json
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )

    target_compile_options(research PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        $<$<CONFIG:Debug>:-g3 -O0 -fno-omit-frame-pointer>
        $<$<CONFIG:RelWithDebInfo>:-g -O2 -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
    )
endif()

# ── research_record: izole, hafif hedef (varsayilan: OFF) ───────────────────
# [FIX-RECORD-ISOLATE] Sadece --record modunu derler; paper_runner.cpp,
# optimize_runner.cpp, backtest_runner.cpp, regime_runner.cpp gibi henuz
# bozuk olan (paper_trader.hpp / genetic_optimizer.hpp'ye bagimli) dosyalara
# HIC dokunmaz/baglanmaz. KUSAK_RECORD_ONLY makrosu, research_main.cpp
# icindeki diger mod dallarini (backtest/genetic/paper/wfo/regime/monitor)
# derleme-zamaninda devre disi birakir.
option(BUILD_RESEARCH_RECORD "Build isolated record-only research binary" OFF)
if(BUILD_RESEARCH_RECORD)
    add_executable(research_record
        src/research_main.cpp
        src/app/research/research_cli.cpp
    )

    target_include_directories(research_record PRIVATE ${KUSAK_INCLUDES})
    target_compile_definitions(research_record PRIVATE KUSAK_RECORD_ONLY)

    target_link_libraries(research_record PRIVATE
        nlohmann_json::nlohmann_json
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )

    target_compile_options(research_record PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        $<$<CONFIG:Debug>:-g3 -O0 -fno-omit-frame-pointer>
        $<$<CONFIG:RelWithDebInfo>:-g -O2 -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
    )
endif()

# ── replay_harness: strategy_core.hpp'yi (canli motorla AYNI karar mantigi)
# kaydedilmis .tick dosyalariyla besleyen izole arac (varsayilan: OFF) ────────
option(BUILD_REPLAY_HARNESS "Build the offline replay harness" OFF)
if(BUILD_REPLAY_HARNESS)
    add_executable(replay_harness
        tools/replay_harness.cpp
    )

    target_include_directories(replay_harness PRIVATE ${KUSAK_INCLUDES})

    target_link_libraries(replay_harness PRIVATE
        nlohmann_json::nlohmann_json
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
    )

    target_compile_options(replay_harness PRIVATE
        -Wall
        -Wextra
        $<$<CONFIG:Debug>:-g3 -O0 -fno-omit-frame-pointer>
        $<$<CONFIG:RelWithDebInfo>:-g -O2 -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
    )
endif()

add_executable(mvp_order_bridge_test tools/mvp_order_bridge_test.cpp)
target_include_directories(mvp_order_bridge_test PRIVATE include)
target_link_libraries(mvp_order_bridge_test PRIVATE nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)