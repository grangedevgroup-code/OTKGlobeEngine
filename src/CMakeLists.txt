add_library(otk_kernel STATIC
    core/defaults.cpp
    core/geo.cpp
    core/image.cpp
    core/mgrs.cpp
    core/prj.cpp
    core/navigation.cpp
    core/astro.cpp
    core/thread_name.cpp
    core/wmm.cpp
    core/geoid.cpp
    net/http_faults.cpp
    net/reachability.cpp
    formats/formats.cpp
    formats/cot.cpp
    formats/iconsets.cpp
    formats/importer.cpp
    formats/text_formats.cpp
    formats/exporters.cpp
    formats/package.cpp
    formats/klv.cpp
    raster/tile_loader.cpp
    raster/tile_matrix.cpp
    raster/tile_cache.cpp
    raster/tile_package.cpp
    raster/tile_provider.cpp
    raster/mobac.cpp
    raster/pmtiles.cpp
    raster/vector_tile.cpp
    raster/canopy.cpp
    elevation/elevation.cpp
    elevation/elevation_service.cpp
    elevation/quantized_mesh.cpp
    feature/parsers.cpp
    feature/feature_query.cpp
    feature/text.cpp
    feature/milsym.cpp
    feature/buildings.cpp
    labels/label_data.cpp
    model/model_convert.cpp)

target_include_directories(otk_kernel
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${stb_SOURCE_DIR}
        ${json_SOURCE_DIR}
        ${earcut_SOURCE_DIR}
        ${cgltf_SOURCE_DIR}
        ${misb_SOURCE_DIR})
target_link_libraries(otk_kernel PRIVATE otk_sqlite pugixml::pugixml otk_miniz)

if(WIN32)
    target_sources(otk_kernel PRIVATE net/http_winhttp.cpp)
    target_link_libraries(otk_kernel PRIVATE winhttp)
elseif(APPLE)
    target_sources(otk_kernel PRIVATE net/http_apple.mm)
    target_compile_options(otk_kernel PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(otk_kernel PUBLIC "-framework Foundation")
elseif(ANDROID)
    target_sources(otk_kernel PRIVATE net/http_android.cpp)
    target_link_libraries(otk_kernel PUBLIC log android)
elseif(EMSCRIPTEN)
    target_sources(otk_kernel PRIVATE net/http_emscripten.cpp)
    target_link_options(otk_kernel PUBLIC -sFETCH=1)
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_sources(otk_kernel PRIVATE net/http_curl.cpp)
    target_link_libraries(otk_kernel PRIVATE CURL::libcurl PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(OTK_WITH_WEBP)
    target_compile_definitions(otk_kernel PRIVATE OTK_WITH_WEBP)
    target_link_libraries(otk_kernel PRIVATE otk_webp)
endif()
if(OTK_WITH_GDAL)
    find_package(GDAL CONFIG)
    if(NOT GDAL_FOUND)
        find_package(GDAL REQUIRED)
    endif()
    target_sources(otk_kernel PRIVATE gdal/gdal_sources.cpp)
    target_link_libraries(otk_kernel PRIVATE GDAL::GDAL)
    target_compile_definitions(otk_kernel PUBLIC OTK_WITH_GDAL)
else()
    target_sources(otk_kernel PRIVATE gdal/gdal_stub.cpp)
endif()
if(MSVC)
    target_compile_options(otk_kernel PRIVATE /W3 /utf-8)
    target_compile_definitions(otk_kernel PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()
otk_embed(otk_kernel font_bold ${PROJECT_SOURCE_DIR}/assets/fonts/NotoSans-Bold-subset.ttf)
otk_embed(otk_kernel geoid_egm96 ${PROJECT_SOURCE_DIR}/assets/geoid/egm96-1deg.bin)
otk_embed(otk_kernel label_font ${PROJECT_SOURCE_DIR}/assets/fonts/Roboto-SemiBold-subset.ttf)
if(OTK_LABELS)
    otk_embed(otk_kernel labels_data ${PROJECT_SOURCE_DIR}/assets/labels/labels.bin)
    target_compile_definitions(otk_kernel PRIVATE OTK_LABELS)
endif()

if(NOT OTK_ENGINE)
    return()
endif()

add_library(otkglobeengine STATIC
    engine.cpp
    capi.cpp
    input/input_controller.cpp
    core/camera.cpp
    feature/feature_layer.cpp
    feature/grid.cpp
    model/model_layer.cpp
    model/gltf_codecs.cpp
    model/tile_content.cpp
    model/pointcloud.cpp
    tiles3d/tileset.cpp
    render/globe_renderer.cpp
    render/label_layer.cpp
    render/building_layer.cpp
    render/gfx_impl.cpp)
target_link_libraries(otkglobeengine PUBLIC otk_kernel)

if(WIN32)
    if(OTK_GFX_BACKEND STREQUAL "SOKOL_GLCORE")
        target_link_libraries(otkglobeengine PUBLIC opengl32)
    endif()
elseif(APPLE)
    set_source_files_properties(render/gfx_impl.cpp PROPERTIES LANGUAGE OBJCXX)
    target_compile_options(otkglobeengine PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(otkglobeengine PUBLIC "-framework Metal" "-framework QuartzCore")
elseif(ANDROID)
    target_link_libraries(otkglobeengine PUBLIC GLESv3 EGL)
elseif(EMSCRIPTEN)
    target_link_options(otkglobeengine PUBLIC -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2)
elseif(OTK_GFX_BACKEND STREQUAL "SOKOL_GLES3")
    target_link_libraries(otkglobeengine PUBLIC GLESv2 EGL)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(otkglobeengine PUBLIC OpenGL::GL)
endif()

target_include_directories(otkglobeengine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${sokol_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${stb_SOURCE_DIR}
        ${json_SOURCE_DIR}
        ${earcut_SOURCE_DIR}
        ${cgltf_SOURCE_DIR})
target_compile_definitions(otkglobeengine PUBLIC ${OTK_GFX_BACKEND})
target_link_libraries(otkglobeengine PRIVATE otk_sqlite pugixml::pugixml otk_miniz)
foreach(_codec DRACO MESHOPT KTX2)
    if(OTK_WITH_${_codec})
        target_compile_definitions(otkglobeengine PRIVATE OTK_WITH_${_codec})
    endif()
endforeach()
if(OTK_WITH_DRACO)
    target_link_libraries(otkglobeengine PRIVATE otk_draco)
endif()
if(OTK_WITH_MESHOPT)
    target_link_libraries(otkglobeengine PRIVATE otk_meshopt)
endif()
if(OTK_WITH_KTX2)
    target_link_libraries(otkglobeengine PRIVATE otk_basisu)
endif()
if(OTK_WITH_GDAL)
    target_link_libraries(otkglobeengine PRIVATE GDAL::GDAL)
endif()
if(MSVC)
    target_compile_options(otkglobeengine PRIVATE /W3 /utf-8)
    target_compile_definitions(otkglobeengine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
endif()

set(_otk_shaders
    render/shaders/globe.glsl
    render/shaders/surface.glsl
    render/shaders/sky.glsl
    render/shaders/feature.glsl
    render/shaders/mip.glsl
    render/shaders/model.glsl
    render/shaders/vector.glsl
    render/shaders/points.glsl
    render/shaders/snapshot.glsl
    render/shaders/labels.glsl
    render/shaders/buildings.glsl)
if(OTK_CONTROLS)
    target_sources(otkglobeengine PRIVATE ui/controls.cpp)
    target_compile_definitions(otkglobeengine PRIVATE OTK_CONTROLS)
    list(APPEND _otk_shaders ui/controls.glsl)
endif()
otk_add_shaders(otkglobeengine ${_otk_shaders})
otk_embed(otkglobeengine building_textures ${PROJECT_SOURCE_DIR}/assets/buildings/facades.jpg)
if(OTK_EMBED_BACKDROP)
    otk_embed(otkglobeengine world_backdrop ${PROJECT_SOURCE_DIR}/assets/world/blue-marble-2048.jpg)
    target_compile_definitions(otkglobeengine PRIVATE OTK_EMBED_BACKDROP)
endif()

if(EMSCRIPTEN)
    return()
endif()

add_library(otkglobeengine_c SHARED capi_shared.cpp)
target_link_libraries(otkglobeengine_c PRIVATE otkglobeengine)
target_compile_definitions(otkglobeengine_c PRIVATE OTK_SHARED OTK_BUILDING)
if(MSVC)
    target_link_options(otkglobeengine_c PRIVATE /WHOLEARCHIVE:otkglobeengine)
elseif(APPLE)
    target_link_options(otkglobeengine_c PRIVATE -Wl,-force_load,$<TARGET_FILE:otkglobeengine>)
else()
    target_link_options(otkglobeengine_c PRIVATE -Wl,--whole-archive $<TARGET_FILE:otkglobeengine> -Wl,--no-whole-archive)
endif()
set_source_files_properties(capi.cpp PROPERTIES COMPILE_DEFINITIONS "OTK_SHARED;OTK_BUILDING")
if(WIN32 AND OTK_BUILD_VIEWER)
    add_custom_command(TARGET otkglobeengine_c POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:otkglobeengine_c> ${CMAKE_BINARY_DIR})
endif()

if(ANDROID)
    add_library(otkglobeengine_jni SHARED ${PROJECT_SOURCE_DIR}/bindings/android/jni/otk_jni.cpp)
    target_link_libraries(otkglobeengine_jni PRIVATE otkglobeengine)
endif()
