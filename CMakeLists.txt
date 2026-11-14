cmake_minimum_required(VERSION 3.28)
project(openciv4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Keep symbols in optimised builds: crash traces from the game core need them.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo,Release>:ProgramDatabase>")
if(MSVC)
	add_link_options($<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "openciv4 is 64-bit only")
endif()

# Whole-program optimisation: the game core spends much of a turn in small accessors
# defined in other translation units, which only link-time code generation can inline.
option(OC4_LTO "Link-time code generation for optimised builds" ON)
if(OC4_LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT OC4_IPO_SUPPORTED LANGUAGES C CXX)
	if(OC4_IPO_SUPPORTED)
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
	endif()
endif()

# Settings for openciv4's own code; the game core (Firaxis SDK) has its own in gamecore/.
add_library(oc4_options INTERFACE)
if(MSVC)
	target_compile_options(oc4_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /MP)
	target_compile_definitions(oc4_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

if(NOT WIN32)
	# The small part of Win32 the engine's tools and the renderer use, and the wmain entry point.
	add_library(oc4_posix STATIC gamecore/compat/posix/oc4_posix.cpp src/posix/oc4_wmain.cpp)
	target_include_directories(oc4_posix PUBLIC gamecore/compat/posix)
	target_compile_features(oc4_posix PUBLIC cxx_std_20)
	target_link_libraries(oc4_posix PUBLIC ${CMAKE_DL_LIBS})
	target_link_libraries(oc4_options INTERFACE oc4_posix)
	set(OC4_DIRECTXMATH_DIR "${PROJECT_SOURCE_DIR}/external/DirectXMath" CACHE PATH "DirectXMath (MIT), part of the Windows SDK on Windows")
	target_include_directories(oc4_options SYSTEM INTERFACE "${OC4_DIRECTXMATH_DIR}/Inc")
endif()

# Visual Studio solution layout
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(OC4_CIV4_DIR "" CACHE PATH "Civilization IV: Beyond the Sword install root (the folder with Assets, Warlords, Beyond the Sword); used for debugger arguments")

# ---------------------------------------------------------------- game core (x64 port of the BtS SDK)
set(OC4_GAMECORE_DIR "${PROJECT_SOURCE_DIR}/gamecore")
include(gamecore/GameCore.cmake)
add_subdirectory(gamecore)

# Mod game cores ported to x64, built when their sources are there (they are not part of this
# repository). Each lands in build/<preset>/mods/<name>, for the mod's Assets folder.
set(OC4_AND2_SOURCE_DIR "${PROJECT_SOURCE_DIR}/mods/and2/CvGameCoreDLL" CACHE PATH "Rise of Mankind: A New Dawn 2 game core sources, ported to x64")
if(EXISTS "${OC4_AND2_SOURCE_DIR}/CvGameCoreDLL.h")
	oc4_add_game_core(CvGameCoreDLL_and2 SOURCE_DIR "${OC4_AND2_SOURCE_DIR}" EXCLUDE CvTextScreens.cpp
		DEFINITIONS _MOD_SENTRY _MOD_FRACTRADE _MOD_GOVWORKERS QC_MASTERY_VICTORY LOG_AI _MOD_GWARM _MOD_SHAM_SPOILER C2C_BUILD USE_RAPID_XML
		DEF_FILE "${OC4_AND2_SOURCE_DIR}/CvGameCoreDLL64.def"
		OUTPUT_DIR "${CMAKE_BINARY_DIR}/mods/and2")
endif()
set(OC4_COM_SOURCE_DIR "${PROJECT_SOURCE_DIR}/external/mods/com-port" CACHE PATH "Chronicles of Mankind game core sources, ported to x64")
if(EXISTS "${OC4_COM_SOURCE_DIR}/CvGameCoreDLL.h")
	# CoM's interface modes have no GO_TO_SENTRY: AND2's flags without _MOD_SENTRY
	oc4_add_game_core(CvGameCoreDLL_com SOURCE_DIR "${OC4_COM_SOURCE_DIR}" EXCLUDE CvTextScreens.cpp
		DEFINITIONS _MOD_FRACTRADE _MOD_GOVWORKERS QC_MASTERY_VICTORY LOG_AI _MOD_GWARM _MOD_SHAM_SPOILER C2C_BUILD USE_RAPID_XML
		DEF_FILE "${OC4_COM_SOURCE_DIR}/CvGameCoreDLL64.def"
		OUTPUT_DIR "${CMAKE_BINARY_DIR}/mods/com")
endif()

# ---------------------------------------------------------------- core
add_library(oc4_core STATIC
	src/core/AssetResolver.cpp
	src/core/BinaryReader.cpp
	src/core/FileUtil.cpp
	src/core/FpkArchive.cpp
)
target_include_directories(oc4_core PUBLIC src)
target_link_libraries(oc4_core PRIVATE oc4_options)

# ---------------------------------------------------------------- nif
add_library(oc4_nif STATIC
	src/nif/KfmFile.cpp
	src/nif/NifFile.cpp
	src/nif/NifBlocks.cpp
)
target_link_libraries(oc4_nif PUBLIC oc4_core PRIVATE oc4_options)

# ---------------------------------------------------------------- gfx / scene
add_library(oc4_gfx STATIC
	src/gfx/DdsImage.cpp
	src/gfx/TgaImage.cpp
)
target_link_libraries(oc4_gfx PUBLIC oc4_core PRIVATE oc4_options)

add_library(oc4_scene STATIC
	src/scene/NifScene.cpp
)
target_link_libraries(oc4_scene PUBLIC oc4_nif PRIVATE oc4_options)

add_library(oc4_anim STATIC
	src/anim/Animation.cpp
)
target_link_libraries(oc4_anim PUBLIC oc4_scene PRIVATE oc4_options)

# Movies (Bink) through FFmpeg's shared libraries (LGPL), delay-loaded: without them beside the
# program, movies are not played.
set(OC4_FFMPEG_DIR "${PROJECT_SOURCE_DIR}/external/ffmpeg-8.1" CACHE PATH "FFmpeg shared build (include, lib, bin) for the movies")
set(OC4_FFMPEG_DLLS avutil-60.dll avcodec-62.dll avformat-62.dll)
add_library(oc4_media STATIC src/media/Movie.cpp)
target_include_directories(oc4_media PUBLIC src)
target_link_libraries(oc4_media PRIVATE oc4_options)
if(NOT WIN32 AND EXISTS "${OC4_FFMPEG_DIR}/include/libavcodec/avcodec.h")
	target_include_directories(oc4_media SYSTEM PRIVATE "${OC4_FFMPEG_DIR}/include")
	target_compile_definitions(oc4_media PRIVATE OC4_HAVE_FFMPEG)
	target_link_directories(oc4_media PUBLIC "${OC4_FFMPEG_DIR}/lib")
	target_link_libraries(oc4_media PUBLIC avutil avcodec avformat swscale swresample)
	set(OC4_FFMPEG_DLLS)
elseif(EXISTS "${OC4_FFMPEG_DIR}/include/libavcodec/avcodec.h")
	target_include_directories(oc4_media SYSTEM PRIVATE "${OC4_FFMPEG_DIR}/include")
	target_compile_definitions(oc4_media PRIVATE OC4_HAVE_FFMPEG)
	# (the build's import libraries are MinGW's, which MSVC cannot delay-load: made again from the .def files)
	foreach(dll IN LISTS OC4_FFMPEG_DLLS)
		get_filename_component(name "${dll}" NAME_WE)
		set(lib "${CMAKE_BINARY_DIR}/ffmpeg/${name}.lib")
		if(NOT EXISTS "${lib}")
			file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ffmpeg")
			execute_process(COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${OC4_FFMPEG_DIR}/lib/${name}.def" "/name:${dll}" "/out:${lib}"
				COMMAND_ERROR_IS_FATAL ANY)
		endif()
		target_link_libraries(oc4_media PUBLIC "${lib}")
		target_link_options(oc4_media INTERFACE "/DELAYLOAD:${dll}")
	endforeach()
	target_link_libraries(oc4_media PUBLIC delayimp)
	list(APPEND OC4_FFMPEG_DLLS swresample-6.dll) # (avcodec's own)
else()
	message(STATUS "FFmpeg not found (OC4_FFMPEG_DIR): movies will not play")
	set(OC4_FFMPEG_DLLS)
endif()

# Window, input and sound through SDL3, built from its sources and linked statically.
set(OC4_SDL3_DIR "${PROJECT_SOURCE_DIR}/external/SDL3-3.4.18" CACHE PATH "SDL3 source tree")
if(NOT EXISTS "${OC4_SDL3_DIR}/include/SDL3/SDL.h")
	message(FATAL_ERROR "SDL3 not found: set OC4_SDL3_DIR (see README)")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${OC4_SDL3_DIR}" "${CMAKE_BINARY_DIR}/SDL3" EXCLUDE_FROM_ALL)

# Glyphs: DirectWrite's on Windows, FreeType's elsewhere (and in its test on Windows).
set(OC4_FREETYPE_DIR "${PROJECT_SOURCE_DIR}/external/freetype-2.13.3" CACHE PATH "FreeType source tree")
if(WIN32)
	option(OC4_FREETYPE "Glyphs through FreeType instead of DirectWrite" OFF)
else()
	set(OC4_FREETYPE ON)
endif()
if(EXISTS "${OC4_FREETYPE_DIR}/include/ft2build.h")
	foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
		set(FT_DISABLE_${dep} ON CACHE BOOL "" FORCE)
	endforeach()
	set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
	add_subdirectory("${OC4_FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/freetype" EXCLUDE_FROM_ALL)
elseif(OC4_FREETYPE)
	message(FATAL_ERROR "FreeType not found: set OC4_FREETYPE_DIR (see README)")
endif()

# Rendering of models, the map view and the overlay through SDL's GPU API, shared by nifview and oc4core.
add_library(oc4_render STATIC
	src/render/ButtonArt.cpp
	src/render/Cursor.cpp
	$<IF:$<BOOL:${OC4_FREETYPE}>,src/render/FontFreeType.cpp,src/render/Font.cpp>
	src/render/Input.cpp
	src/render/Leaderhead.cpp
	src/render/ModelView.cpp
	src/render/MapView.cpp
	src/render/MapWindow.cpp
	src/render/Overlay.cpp
	src/render/Particles.cpp
	src/render/PopupPanel.cpp
	src/render/UiLayer.cpp
	src/render/PngWriter.cpp
	src/render/Renderer.cpp
	src/render/TextLayout.cpp
	src/render/Theme.cpp
)
target_link_libraries(oc4_render PUBLIC oc4_anim oc4_gfx oc4_media SDL3::SDL3-static PRIVATE oc4_options)
# SPIR-V versions of the shaders (src/render/SpirvShaders.inc, from tools/shaders/gen_spirv.py): with them,
# OC4_GPU=vulkan draws through SDL's Vulkan backend (MoltenVK on macOS). A start for Linux.
option(OC4_SPIRV_SHADERS "Also build the SPIR-V shaders (OC4_GPU=vulkan selects SDL's Vulkan backend)" OFF)
if(OC4_SPIRV_SHADERS)
	target_compile_definitions(oc4_render PRIVATE OC4_SPIRV_SHADERS)
endif()
if(WIN32)
	target_link_libraries(oc4_render PUBLIC d3dcompiler dwrite windowscodecs ole32)
endif()
if(OC4_FREETYPE)
	target_link_libraries(oc4_render PRIVATE freetype)
endif()

# Sound: the game's WAV and MP3 files (dr_mp3) mixed and played through SDL3.
set(OC4_DR_LIBS_DIR "${PROJECT_SOURCE_DIR}/external/dr_libs" CACHE PATH "dr_libs (dr_mp3.h)")
if(NOT EXISTS "${OC4_DR_LIBS_DIR}/dr_mp3.h")
	message(FATAL_ERROR "dr_mp3.h not found: set OC4_DR_LIBS_DIR (see README)")
endif()
add_library(oc4_audio STATIC
	src/audio/Device.cpp
	src/audio/Media.cpp
	src/audio/Wav.cpp
)
target_include_directories(oc4_audio PUBLIC src)
target_include_directories(oc4_audio SYSTEM PRIVATE "${OC4_DR_LIBS_DIR}")
target_link_libraries(oc4_audio PUBLIC SDL3::SDL3-static PRIVATE oc4_options)

# ---------------------------------------------------------------- tools
add_executable(nifscan tools/nifscan/main.cpp)
target_link_libraries(nifscan PRIVATE oc4_options oc4_nif)

add_executable(nifdump tools/nifdump/main.cpp)
target_link_libraries(nifdump PRIVATE oc4_options oc4_nif)

add_executable(themedump tools/themedump/main.cpp)
target_link_libraries(themedump PRIVATE oc4_options oc4_render)

add_executable(animscan tools/animscan/main.cpp)
target_link_libraries(animscan PRIVATE oc4_options oc4_anim)

add_executable(soundscan tools/soundscan/main.cpp)
target_link_libraries(soundscan PRIVATE oc4_options oc4_audio)

# ---------------------------------------------------------------- apps
add_executable(nifview apps/nifview/main.cpp)
target_link_libraries(nifview PRIVATE oc4_options oc4_render)

set_target_properties(oc4_core oc4_nif oc4_gfx oc4_scene oc4_anim oc4_media oc4_render oc4_audio PROPERTIES FOLDER "libs")
set_target_properties(nifscan nifdump themedump animscan soundscan PROPERTIES FOLDER "tools")
# ---------------------------------------------------------------- zlib (saves)
set(OC4_ZLIB_DIR "${PROJECT_SOURCE_DIR}/external/zlib-1.3.1" CACHE PATH "zlib source tree (saves are zlib streams)")
if(NOT EXISTS "${OC4_ZLIB_DIR}/zlib.h")
	message(FATAL_ERROR "zlib not found: set OC4_ZLIB_DIR (see README)")
endif()
add_library(oc4_zlib STATIC)
foreach(f adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil)
	target_sources(oc4_zlib PRIVATE "${OC4_ZLIB_DIR}/${f}.c")
endforeach()
target_include_directories(oc4_zlib SYSTEM PUBLIC "${OC4_ZLIB_DIR}")
target_compile_definitions(oc4_zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
if(MSVC)
	target_compile_options(oc4_zlib PRIVATE /W0)
endif()
set_target_properties(oc4_zlib PROPERTIES FOLDER "libs")

# ---------------------------------------------------------------- engine
add_executable(oc4core
	apps/oc4core/main.cpp
	apps/oc4core/Audio.cpp
	apps/oc4core/Sounds.cpp
	apps/oc4core/CrashHandler.cpp
	apps/oc4core/DataStream.cpp
	apps/oc4core/Diplomacy.cpp
	apps/oc4core/Net.cpp
	apps/oc4core/EngineClasses.cpp
	apps/oc4core/FAStar.cpp
	apps/oc4core/GameCoreImports.cpp
	apps/oc4core/GameFont.cpp
	apps/oc4core/GameSession.cpp
	apps/oc4core/Interface.cpp
	apps/oc4core/MapCapture.cpp
	apps/oc4core/Profiler.cpp
	apps/oc4core/PythonHost.cpp
	apps/oc4core/SaveFile.cpp
	apps/oc4core/Screens.cpp
	apps/oc4core/Text.cpp
	apps/oc4core/TurnWorker.cpp
	apps/oc4core/Unimplemented.cpp
	apps/oc4core/Utility.cpp
	apps/oc4core/Xml.cpp
	$<$<PLATFORM_ID:Windows>:apps/oc4core/oc4core.rc>
)
# The scripts in python/ are compiled into the engine as byte arrays.
foreach(_py IN ITEMS oc4engine:kOc4EnginePy oc4screens:kOc4ScreensPy oc4menu:kOc4MenuPy oc4turnlog:kOc4TurnLogPy oc4options:kOc4OptionsPy oc4wb:kOc4WbPy)
	string(REPLACE ":" ";" _py "${_py}")
	list(GET _py 0 _py_name)
	list(GET _py 1 _py_symbol)
	set(_py_file "${PROJECT_SOURCE_DIR}/apps/oc4core/python/${_py_name}.py")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_py_file}")
	file(READ "${_py_file}" _py_hex HEX)
	string(REGEX REPLACE "(..)" "0x\\1," _py_hex "${_py_hex}")
	file(WRITE "${CMAKE_BINARY_DIR}/generated/${_py_name}_py.h.tmp" "static const unsigned char ${_py_symbol}[] = {${_py_hex}0x00};\n")
	configure_file("${CMAKE_BINARY_DIR}/generated/${_py_name}_py.h.tmp" "${CMAKE_BINARY_DIR}/generated/${_py_name}_py.h" COPYONLY)
endforeach()
target_include_directories(oc4core PRIVATE apps/oc4core "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(oc4core PRIVATE CvGameCoreDLL oc4_core oc4_render oc4_audio oc4_zlib)
if(NOT MSVC)
	# (only the POSIX layer and DirectXMath: on MSVC oc4core keeps the SDK's own dialect)
	target_link_libraries(oc4core PRIVATE oc4_options)
endif()
if(WIN32)
	target_link_libraries(oc4core PRIVATE dbghelp bcrypt winmm shell32 ole32 advapi32 ws2_32)
endif()
# Includes the SDK headers, so it shares the game core's dialect (no UNICODE, lenient conformance).
target_compile_definitions(oc4core PRIVATE OC4_GAMECORE_IMPORT _USRDLL _CRT_SECURE_NO_WARNINGS "OC4_DEFAULT_PYTHON_HOME=\"${OC4_PYTHON27_DIR}\""
	"OC4_GAME_CORE_DLL=\"$<TARGET_FILE_NAME:CvGameCoreDLL>\"")
if(WIN32)
	target_link_options(oc4core PRIVATE "/DELAYLOAD:${OC4_GAME_CORE_NAME}.dll")
	target_link_libraries(oc4core PRIVATE delayimp)
endif()
set_target_properties(oc4core PROPERTIES CXX_STANDARD 20 CXX_SCAN_FOR_MODULES OFF)
if(NOT MSVC)
	target_compile_options(oc4core PRIVATE -fms-extensions -ferror-limit=0 -Wno-everything -ffp-contract=off)
endif()
if(MSVC)
	target_compile_options(oc4core PRIVATE /permissive /W3 /EHsc /MP /source-charset:windows-1252 /execution-charset:windows-1252
		/wd4244 /wd4267 /wd4996 /wd4251 /wd4018 /wd4800)
endif()
if(WIN32)
list(TRANSFORM OC4_FFMPEG_DLLS PREPEND "${OC4_FFMPEG_DIR}/bin/" OUTPUT_VARIABLE OC4_FFMPEG_DLL_FILES)
add_custom_command(TARGET oc4core POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OC4_PYTHON27_DIR}/python27.dll" ${OC4_FFMPEG_DLL_FILES} "$<TARGET_FILE_DIR:oc4core>")
endif()
# ---------------------------------------------------------------- tests
enable_testing()
add_executable(asset_paths_test tests/asset_paths_test.cpp)
target_link_libraries(asset_paths_test PRIVATE oc4_options oc4_core)
add_test(NAME asset_paths COMMAND asset_paths_test)

add_executable(save_file_test tests/save_file_test.cpp apps/oc4core/SaveFile.cpp)
target_include_directories(save_file_test PRIVATE apps/oc4core)
target_link_libraries(save_file_test PRIVATE oc4_options oc4_core oc4_zlib)
if(WIN32)
	target_link_libraries(save_file_test PRIVATE bcrypt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(OpenSSL REQUIRED COMPONENTS Crypto)
	target_link_libraries(save_file_test PRIVATE OpenSSL::Crypto)
endif()
add_test(NAME save_file COMMAND save_file_test)

add_executable(hash_map_test tests/hash_map_test.cpp)
target_include_directories(hash_map_test PRIVATE gamecore/compat)
target_link_libraries(hash_map_test PRIVATE oc4_options)
add_test(NAME hash_map COMMAND hash_map_test)
set_target_properties(hash_map_test PROPERTIES FOLDER "tests")

add_executable(ui_scale_test tests/ui_scale_test.cpp)
target_include_directories(ui_scale_test PRIVATE src)
target_link_libraries(ui_scale_test PRIVATE oc4_options)
add_test(NAME ui_scale COMMAND ui_scale_test)
set_target_properties(ui_scale_test PROPERTIES FOLDER "tests")

add_executable(frame_stats_test tests/frame_stats_test.cpp)
target_include_directories(frame_stats_test PRIVATE src)
target_link_libraries(frame_stats_test PRIVATE oc4_options)
add_test(NAME frame_stats COMMAND frame_stats_test)
set_target_properties(frame_stats_test PROPERTIES FOLDER "tests")

add_executable(key_nav_test tests/key_nav_test.cpp)
target_include_directories(key_nav_test PRIVATE src)
target_link_libraries(key_nav_test PRIVATE oc4_options)
add_test(NAME key_nav COMMAND key_nav_test)
set_target_properties(key_nav_test PROPERTIES FOLDER "tests")

add_executable(wasd_pan_test tests/wasd_pan_test.cpp)
target_include_directories(wasd_pan_test PRIVATE src)
target_link_libraries(wasd_pan_test PRIVATE oc4_options)
add_test(NAME wasd_pan COMMAND wasd_pan_test)
set_target_properties(wasd_pan_test PROPERTIES FOLDER "tests")

add_executable(edge_scroll_test tests/edge_scroll_test.cpp)
target_include_directories(edge_scroll_test PRIVATE src)
add_test(NAME edge_scroll COMMAND edge_scroll_test)
set_target_properties(edge_scroll_test PROPERTIES FOLDER "tests")

add_executable(auto_end_turn_test tests/auto_end_turn_test.cpp)
target_include_directories(auto_end_turn_test PRIVATE apps/oc4core)
target_link_libraries(auto_end_turn_test PRIVATE oc4_options)
add_test(NAME auto_end_turn COMMAND auto_end_turn_test)
set_target_properties(auto_end_turn_test PROPERTIES FOLDER "tests")

add_executable(xml_decode_test tests/xml_decode_test.cpp)
target_include_directories(xml_decode_test PRIVATE apps/oc4core)
target_link_libraries(xml_decode_test PRIVATE oc4_options)
add_test(NAME xml_decode COMMAND xml_decode_test)
set_target_properties(xml_decode_test PROPERTIES FOLDER "tests")

add_executable(pad_map_test tests/pad_map_test.cpp)
target_include_directories(pad_map_test PRIVATE src)
add_test(NAME pad_map COMMAND pad_map_test)
set_target_properties(pad_map_test PROPERTIES FOLDER "tests")

add_executable(globe_bend_test tests/globe_bend_test.cpp)
target_include_directories(globe_bend_test PRIVATE src)
target_link_libraries(globe_bend_test PRIVATE oc4_options)
add_test(NAME globe_bend COMMAND globe_bend_test)
set_target_properties(globe_bend_test PROPERTIES FOLDER "tests")

add_executable(theme_combine_test tests/theme_combine_test.cpp)
target_include_directories(theme_combine_test PRIVATE src)
target_link_libraries(theme_combine_test PRIVATE oc4_options oc4_render)
add_test(NAME theme_combine COMMAND theme_combine_test)
set_target_properties(theme_combine_test PROPERTIES FOLDER "tests")

if(WIN32 AND TARGET freetype)
	add_executable(text_layout_freetype_test tests/text_layout_test.cpp src/render/TextLayout.cpp src/render/FontFreeType.cpp)
	target_include_directories(text_layout_freetype_test PRIVATE src)
	target_link_libraries(text_layout_freetype_test PRIVATE oc4_options freetype dwrite)
	add_test(NAME text_layout_freetype COMMAND text_layout_freetype_test)
	set_target_properties(text_layout_freetype_test PROPERTIES FOLDER "tests")
endif()

if(NOT WIN32)
	add_executable(posix_compat_test gamecore/tests/posix_compat_test.cpp)
	target_link_libraries(posix_compat_test PRIVATE oc4_posix)
	add_test(NAME posix_compat COMMAND posix_compat_test)
	set_target_properties(posix_compat_test PROPERTIES FOLDER "tests")
endif()

if(WIN32)
	add_executable(text_layout_test tests/text_layout_test.cpp)
	target_link_libraries(text_layout_test PRIVATE oc4_options oc4_render)
	add_test(NAME text_layout COMMAND text_layout_test)
	set_target_properties(text_layout_test PROPERTIES FOLDER "tests")
endif()

set_target_properties(nifview oc4core PROPERTIES FOLDER "apps")
get_property(OC4_CORE_DIRS GLOBAL PROPERTY OC4_GAME_CORE_DIRS)
if(WIN32)
foreach(dir IN LISTS OC4_CORE_DIRS)
	add_custom_command(TARGET oc4core POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:oc4core>" "$<TARGET_PDB_FILE:oc4core>" "${OC4_PYTHON27_DIR}/python27.dll" ${OC4_FFMPEG_DLL_FILES} "${dir}")
endforeach()
endif()
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT oc4core)
if(OC4_CIV4_DIR)
	set_target_properties(nifview PROPERTIES VS_DEBUGGER_ENVIRONMENT "OC4_CIV4_DIR=${OC4_CIV4_DIR}"
		VS_DEBUGGER_COMMAND_ARGUMENTS "--filter art/units/")
	set_target_properties(nifscan PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS
		"\"${OC4_CIV4_DIR}/Beyond the Sword/Assets/Art\" \"${OC4_CIV4_DIR}/Warlords/Assets/Art\" \"${OC4_CIV4_DIR}/Assets\"")
endif()
