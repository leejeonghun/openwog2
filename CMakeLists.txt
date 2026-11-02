cmake_minimum_required(VERSION 3.20)
project(genesis2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_mixer CONFIG REQUIRED)

add_executable(genesis2
    src/gene2.cpp
    src/video.cpp
    src/palette.cpp
    src/pcx.cpp
    src/font.cpp
    src/input.cpp
    src/fileutil.cpp
    src/exe_resource.cpp
    src/audio.cpp
    src/fli.cpp
    src/main.cpp
    src/ttl.cpp
    src/chp.cpp
    src/chp_option.cpp
    src/chp_equip.cpp
    src/chp_class.cpp
    src/chp_parser.cpp
    src/pack.cpp
    src/pack_shop.cpp
    src/pack_status.cpp
    src/pack_equip.cpp
    src/pack_class.cpp
    src/pack_itemlist.cpp
    src/pack_talk.cpp
    src/combatii.cpp
    src/combatii_map.cpp
    src/combatii_script.cpp
    src/combatii_battle.cpp
    src/combatii_ai.cpp
    src/combatii_ai_approach.cpp
    src/combatii_aimage.cpp
    src/combatii_fx_ice.cpp
    src/combatii_fx_field.cpp
    src/combatii_fx_gfx.cpp
    src/combatii_fx_fire.cpp
    src/combatii_fx_support.cpp
    src/combatii_fx_blade.cpp
    src/combatii_fx_dark.cpp
    src/combatii_fx_weapon.cpp
    src/combatii_fx_meteo.cpp
    src/combatii_fx_startear.cpp
    src/combatii_fx_thundwav.cpp
    src/combatii_fx_thun7.cpp
    src/combatii_fx_sunmoon.cpp
    src/combatii_fx_prospear.cpp
    src/combatii_fx_revible.cpp
    src/combatii_fx_dragon.cpp
    src/combatii_fx_healwind.cpp
    src/combatii_fx_hitcrit.cpp
    src/combatii_fx_swstr.cpp
    src/combatii_fx_madaxe.cpp
    src/combatii_fx_plight.cpp
    src/combatii_fx_axefly.cpp
    src/combatii_fx_swirl.cpp
    src/combatii_fx_wish.cpp
    src/combatii_wbody_asura.cpp
    src/combatii_wbody_bird.cpp
    src/combatii_wbody_sarah.cpp
    src/combatii_wbody_stormaxe.cpp
    src/combatii_wbody_shu.cpp
    src/combatii_wbody_rain.cpp
    src/combatii_aiturn.cpp
    src/combatii_range.cpp
    src/combatii_player.cpp
    src/combatii_ring.cpp
    src/combatii_equip.cpp
    src/combatii_move.cpp
    src/combatii_moveui.cpp
    src/combatii_deploy.cpp
    src/combatii_sysmenu.cpp
    src/spr_parser.cpp
    src/nc2_parser.cpp
    src/portrait.cpp
    src/talktbl.cpp
    src/game_timer.cpp
)

target_include_directories(genesis2 PRIVATE
    src
)

target_link_libraries(genesis2 PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    $<IF:$<TARGET_EXISTS:SDL2_mixer::SDL2_mixer>,SDL2_mixer::SDL2_mixer,SDL2_mixer::SDL2_mixer-static>
)

if(MSVC)
    target_compile_definitions(genesis2 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(genesis2 PRIVATE /utf-8 /MP)
else()
    target_compile_options(genesis2 PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# Copy game data directory path as compile definition
target_compile_definitions(genesis2 PRIVATE
    GAME_DATA_DIR="${CMAKE_SOURCE_DIR}/GENESIS2"
)
