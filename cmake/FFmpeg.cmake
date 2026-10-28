include(ExternalProject)
include(ProcessorCount)
find_package(Threads REQUIRED)
find_program(FFLITE_MAKE_EXECUTABLE NAMES gmake make REQUIRED)

set(FFLITE_FFMPEG_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH
    "Local FFmpeg source directory (tested with 7.1.1)")
set(FFLITE_FFMPEG_CONFIGURE_ARGS "" CACHE STRING
    "Additional FFmpeg configure arguments, separated by semicolons")
if(NOT EXISTS "${FFLITE_FFMPEG_SOURCE_DIR}/configure")
  message(FATAL_ERROR
    "FFmpeg source missing: ${FFLITE_FFMPEG_SOURCE_DIR}. "
    "Extract FFmpeg into third_party/ffmpeg or set FFLITE_FFMPEG_SOURCE_DIR. See README.md.")
endif()
if(NOT UNIX)
  message(FATAL_ERROR "The initial local FFmpeg build supports POSIX systems; Windows is not configured yet.")
endif()

set(ffmpeg_prefix "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-install")
file(MAKE_DIRECTORY "${ffmpeg_prefix}/include")
ProcessorCount(ffmpeg_jobs)
if(NOT ffmpeg_jobs)
  set(ffmpeg_jobs 2)
endif()
set(ffmpeg_archives)
foreach(component avfilter avformat avcodec swscale swresample avutil)
  list(APPEND ffmpeg_archives "${ffmpeg_prefix}/lib/lib${component}.a")
endforeach()

ExternalProject_Add(fflite_ffmpeg_build
  SOURCE_DIR "${FFLITE_FFMPEG_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-build"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND "${FFLITE_FFMPEG_SOURCE_DIR}/configure"
    "--prefix=${ffmpeg_prefix}" --libdir=${ffmpeg_prefix}/lib
    "--cc=${CMAKE_C_COMPILER}" "--cxx=${CMAKE_CXX_COMPILER}"
    --disable-autodetect --disable-programs --disable-doc --disable-debug
    --disable-x86asm --disable-shared --enable-static --enable-pic
    --disable-avdevice --disable-postproc
    ${FFLITE_FFMPEG_CONFIGURE_ARGS}
    --enable-avfilter --enable-swresample
    --enable-filter=buffer,buffersink,abuffer,abuffersink,scale,crop,format,aresample,aformat,volume,trim,atrim,setpts,asetpts
  BUILD_COMMAND "${FFLITE_MAKE_EXECUTABLE}" "-j${ffmpeg_jobs}"
  INSTALL_COMMAND "${FFLITE_MAKE_EXECUTABLE}" install
  BUILD_BYPRODUCTS ${ffmpeg_archives}
)

add_library(fflite_ffmpeg INTERFACE)
add_dependencies(fflite_ffmpeg fflite_ffmpeg_build)
target_include_directories(fflite_ffmpeg SYSTEM INTERFACE "${ffmpeg_prefix}/include")
target_link_libraries(fflite_ffmpeg INTERFACE ${ffmpeg_archives} Threads::Threads ${CMAKE_DL_LIBS} m)
if(APPLE)
  target_link_libraries(fflite_ffmpeg INTERFACE "-framework CoreFoundation" "-framework Security"
    "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo" "-framework AudioToolbox")
endif()
