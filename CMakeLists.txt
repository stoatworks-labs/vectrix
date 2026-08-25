cmake_minimum_required(VERSION 3.15)
project(Vectrix VERSION 0.1.3 LANGUAGES C CXX)

#-----------------------------------------------------------------------------
# Architecture. Must sit immediately after project() and before any target
# exists: CMake latches the architecture list when the first target is created,
# and setting it later is silently ignored -- the build succeeds and produces a
# single-architecture binary. Verify the result with `lipo`, never with the log.
#-----------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()
if(WIN32)
    # windows.h defines min/max as macros, which breaks std::min/std::max.
    add_compile_definitions(NOMINMAX)
endif()

option(VECTRIX_BUILD_TOOLS "Build the offline render and measurement harness" ON)
option(VECTRIX_WITH_OPUS   "Decode .opus files (vendored libogg/libopus/libopusfile)" ON)
option(BUILD_OFX           "Build the OpenFX plugin" ON)

#-----------------------------------------------------------------------------
# The FFGL SDK
#-----------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

#-----------------------------------------------------------------------------
# Audio decoding
#
# dr_wav / dr_mp3 / dr_flac and stb_vorbis are single-header public-domain
# decoders, compiled into AudioFile.cpp and nowhere else. They cost nothing here.
#
# Opus is the one real dependency, and it is built FROM SOURCE rather than
# through vcpkg on purpose: vcpkg triplets are per-architecture, so a vcpkg
# libopus cannot produce the arm64+x86_64 universal binary macOS needs, while
# add_subdirectory inherits CMAKE_OSX_ARCHITECTURES and just works.
#
# opusfile's own CMakeLists is not usable here -- it calls find_package(Ogg) and
# find_package(Opus), which cannot see in-tree add_subdirectory targets, and it
# also builds opusurl (needing OpenSSL), examples and Doxygen docs. Its library
# is four C files with no generated header (config.h is behind HAVE_CONFIG_H,
# which we do not define), so we declare that target ourselves. Nothing is
# patched in the submodule.
#-----------------------------------------------------------------------------
if(VECTRIX_WITH_OPUS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/opusfile/src/opusfile.c")
        message(FATAL_ERROR
            "Opus support is on but the submodules are missing. Run:\n"
            "  git submodule update --init --recursive\n"
            "or configure with -DVECTRIX_WITH_OPUS=OFF to build without .opus support.")
    endif()

    set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
    set(INSTALL_DOCS           OFF CACHE BOOL "" FORCE)
    set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS    OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING     OFF CACHE BOOL "" FORCE)

    add_subdirectory(external/ogg  EXCLUDE_FROM_ALL)
    add_subdirectory(external/opus EXCLUDE_FROM_ALL)

    add_library(opusfile STATIC
        external/opusfile/src/info.c
        external/opusfile/src/internal.c
        external/opusfile/src/opusfile.c
        external/opusfile/src/stream.c
    )
    target_include_directories(opusfile
        PRIVATE external/opusfile/src
        PUBLIC  external/opusfile/include)
    # OP_DISABLE_HTTP drops http.c and its OpenSSL dependency; we read local
    # files only, and a media plugin has no business opening sockets.
    target_compile_definitions(opusfile PRIVATE OP_DISABLE_HTTP)
    target_link_libraries(opusfile PUBLIC ogg opus)
    set_target_properties(opusfile PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(opusfile PRIVATE /wd4267 /wd4244 /wd4090)
    endif()
endif()

#-----------------------------------------------------------------------------
# The shared core.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor and nothing ever references that object by name, so in
# an archive the linker is entitled to drop the whole translation unit -- and
# the result is a bundle that loads, exports plugMain, and reports that it
# contains no plugins.
#
# Note what is NOT in here: SourcePlugin.cpp and EffectPlugin.cpp. Putting
# either registration in the shared library would register both plugins into
# both bundles.
#-----------------------------------------------------------------------------
add_library(vectrix_core OBJECT
    source/Vectrix.h                source/Vectrix.cpp
    source/Controls.h               source/Controls.cpp
    source/Presets.h
    source/Diag.h                   source/Diag.cpp
    source/StoatworksAbout.h        source/StoatworksAboutParams.h

    source/signal/Signal.h
    source/signal/Clock.h           source/signal/Clock.cpp
    source/signal/Engine.h          source/signal/Engine.cpp
    source/signal/Smooth.h
    source/signal/Rng.h
    source/signal/Modulation.h      source/signal/Modulation.cpp

    source/signal/sources/Source.h
    source/signal/sources/Oscillator.h  source/signal/sources/Oscillator.cpp
    source/signal/sources/Shapes.h      source/signal/sources/Shapes.cpp
    source/signal/sources/Wire.h        source/signal/sources/Wire.cpp
    source/signal/sources/Meshes.cpp
    source/signal/sources/AudioFile.h   source/signal/sources/AudioFile.cpp
    source/signal/sources/Trace.h       source/signal/sources/Trace.cpp
    source/signal/sources/PathWalker.h  source/signal/sources/PathWalker.cpp

    source/signal/fx/Fx.h
    source/signal/fx/Filters.h
    source/signal/fx/Chain.h        source/signal/fx/Chain.cpp
    source/signal/fx/Dynamics.cpp
    source/signal/fx/Shaper.cpp
    source/signal/fx/Modulation.cpp
    source/signal/fx/Delay.h        source/signal/fx/Delay.cpp
    source/signal/fx/Reverb.h       source/signal/fx/Reverb.cpp
    source/signal/fx/Output.cpp

    source/render/ScopeBuffer.h     source/render/ScopeBuffer.cpp
    source/render/GLState.h
    source/render/Phosphor.h        source/render/Phosphor.cpp
    source/render/Tube.h            source/render/Tube.cpp
    source/render/BeamGeometry.h    source/render/BeamGeometry.cpp
    source/render/Shaders.h
    source/render/shaders/Prelude.cpp
    source/render/shaders/Trace.cpp
    source/render/shaders/Decay.cpp
    source/render/shaders/Bloom.cpp
    source/render/shaders/Glass.cpp
)

target_include_directories(vectrix_core PUBLIC
    source external/dr_libs external/stb)
target_link_libraries(vectrix_core PUBLIC ffgl::sdk)
target_compile_definitions(vectrix_core PUBLIC VECTRIX_VERSION="${PROJECT_VERSION}")
set_target_properties(vectrix_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(VECTRIX_WITH_OPUS)
    target_link_libraries(vectrix_core PUBLIC opusfile)
    target_compile_definitions(vectrix_core PUBLIC VECTRIX_WITH_OPUS=1)
endif()

# The SDK headers pull <GL/glew.h> on Windows and Linux, and the SDK links GLEW
# privately -- so consumers need the include path and the GLEW_STATIC define the
# static vcpkg triplet propagates. macOS uses the system OpenGL framework.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(vectrix_core PUBLIC GLEW::GLEW)
endif()

#-----------------------------------------------------------------------------
# The two plugins.
#
# FFGL resolves one plugMain per binary, so a source and an effect are two
# bundles -- not one bundle with two entries.
#-----------------------------------------------------------------------------
function(vectrix_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE vectrix_core)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL" "-framework Carbon" "-framework AppKit")
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

vectrix_add_plugin(VectrixSource source/SourcePlugin.cpp
    "Vectrix" "com.stoatworks.ffgl.vectrix")
vectrix_add_plugin(VectrixEffect source/EffectPlugin.cpp
    "Vectrix Trace" "com.stoatworks.ffgl.vectrix.trace")

#-----------------------------------------------------------------------------
# The offline harness. macOS only, one translation unit, and it links the same
# OBJECT library the bundles do -- so it measures the code that ships.
#-----------------------------------------------------------------------------
if(VECTRIX_BUILD_TOOLS AND APPLE)
    add_executable(vxtest tools/vxtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines in
    # the harness rather than a vendored dependency.
    target_link_libraries(vxtest PRIVATE vectrix_core z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

#-----------------------------------------------------------------------------
# Install: straight into Resolume's plugin folder.
#-----------------------------------------------------------------------------
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

install(TARGETS VectrixSource VectrixEffect
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

#-----------------------------------------------------------------------------
# OpenFX. Not in the install() list -- it goes to /Library/OFX/Plugins by hand
# and ships as its own zip.
#-----------------------------------------------------------------------------
if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    # NOTE: `vectrix_core` is LINKED below, not spliced in here as
    # `$<TARGET_OBJECTS:vectrix_core>`.
    #
    # TARGET_OBJECTS pulls in the compiled objects and nothing else -- no include
    # directories, no compile definitions, none of the usage requirements the
    # library declares. So this target compiled happily on macOS, where the SDK
    # headers reach the system OpenGL framework, and failed on Windows with
    # "Cannot open include file: 'GL/glew.h'" from inside FFGL.h, because GLEW
    # arrives through `vectrix_core`'s PUBLIC link to GLEW::GLEW and that link
    # was exactly what TARGET_OBJECTS threw away. Linking the OBJECT library
    # gives both the objects and the usage requirements.
    add_library(VectrixOFX MODULE
        source/ofx/VectrixOFX.cpp
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    target_include_directories(VectrixOFX PRIVATE
        source external/dr_libs external/stb
        "${OFX_SDK}/include" "${OFX_SDK}/Support/include")
    target_link_libraries(VectrixOFX PRIVATE vectrix_core)
    target_compile_definitions(VectrixOFX PRIVATE
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR})
    if(APPLE)
        target_link_libraries(VectrixOFX PRIVATE "-framework OpenGL" "-framework AppKit")
    endif()

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports
    # -- that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(VectrixOFX PRIVATE Threads::Threads)

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Vectrix.ofx.bundle")
    set_target_properties(VectrixOFX PROPERTIES
        PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "Vectrix"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS")

    if(APPLE)
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.vectrix.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(VectrixOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib collides with the FFGL target's, which shares the
            # output base name; a parallel build then races with LNK1104.
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(VectrixOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()
