# Turn everything ON that's expected for an official release builds.
#
# Example usage:
#   cmake -C../blender/build_files/cmake/config/blender_full.cmake  ../blender
#

set(WITH_ALEMBIC             ON  CACHE BOOL "" FORCE)
set(WITH_AUDASPACE           ON  CACHE BOOL "" FORCE)
set(WITH_BUILDINFO           ON  CACHE BOOL "" FORCE)
set(WITH_BULLET              ON  CACHE BOOL "" FORCE)
set(WITH_CODEC_AVI           ON  CACHE BOOL "" FORCE)
set(WITH_CODEC_FFMPEG        ON  CACHE BOOL "" FORCE)
set(WITH_CODEC_SNDFILE       ON  CACHE BOOL "" FORCE)
set(WITH_COMPOSITOR          ON  CACHE BOOL "" FORCE)
set(WITH_CYCLES              ON  CACHE BOOL "" FORCE)
set(WITH_CYCLES_EMBREE       ON  CACHE BOOL "" FORCE)
set(WITH_CYCLES_OSL          ON  CACHE BOOL "" FORCE)
set(WITH_DRACO               ON  CACHE BOOL "" FORCE)
set(WITH_FFTW3               ON  CACHE BOOL "" FORCE)
set(WITH_FREESTYLE           ON  CACHE BOOL "" FORCE)
set(WITH_GMP                 ON  CACHE BOOL "" FORCE)
set(WITH_HARU                ON  CACHE BOOL "" FORCE)
set(WITH_IK_ITASC            ON  CACHE BOOL "" FORCE)
set(WITH_IK_SOLVER           ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_CINEON        ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_DDS           ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_HDR           ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_OPENEXR       ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_OPENJPEG      ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_TIFF          ON  CACHE BOOL "" FORCE)
set(WITH_INPUT_NDOF          ON  CACHE BOOL "" FORCE)
set(WITH_INTERNATIONAL       ON  CACHE BOOL "" FORCE)
set(WITH_LIBMV               ON  CACHE BOOL "" FORCE)
set(WITH_LIBMV_SCHUR_SPECIALIZATIONS ON CACHE BOOL "" FORCE)
set(WITH_LZMA                ON  CACHE BOOL "" FORCE)
set(WITH_LZO                 ON  CACHE BOOL "" FORCE)
set(WITH_MOD_FLUID           ON  CACHE BOOL "" FORCE)
set(WITH_MOD_OCEANSIM        ON  CACHE BOOL "" FORCE)
set(WITH_MOD_REMESH          ON  CACHE BOOL "" FORCE)
set(WITH_NANOVDB             ON  CACHE BOOL "" FORCE)
set(WITH_OPENAL              ON  CACHE BOOL "" FORCE)
set(WITH_OPENCOLLADA         ON  CACHE BOOL "" FORCE)
set(WITH_OPENCOLORIO         ON  CACHE BOOL "" FORCE)
set(WITH_OPENIMAGEDENOISE    ON  CACHE BOOL "" FORCE)
set(WITH_OPENMP              ON  CACHE BOOL "" FORCE)
set(WITH_OPENSUBDIV          ON  CACHE BOOL "" FORCE)
set(WITH_OPENVDB             ON  CACHE BOOL "" FORCE)
set(WITH_OPENVDB_BLOSC       ON  CACHE BOOL "" FORCE)
set(WITH_POTRACE             ON  CACHE BOOL "" FORCE)
set(WITH_PUGIXML             ON  CACHE BOOL "" FORCE)
set(WITH_PYTHON_INSTALL      ON  CACHE BOOL "" FORCE)
set(WITH_QUADRIFLOW          ON  CACHE BOOL "" FORCE)
set(WITH_SDL                 ON  CACHE BOOL "" FORCE)
set(WITH_TBB                 ON  CACHE BOOL "" FORCE)
set(WITH_USD                 ON  CACHE BOOL "" FORCE)

set(WITH_MEM_JEMALLOC        ON  CACHE BOOL "" FORCE)


# platform dependent options
if(APPLE)
  set(WITH_COREAUDIO           ON  CACHE BOOL "" FORCE)
endif()
if(NOT WIN32)
  set(WITH_JACK                ON  CACHE BOOL "" FORCE)
endif()
if(WIN32)
  set(WITH_WASAPI              ON  CACHE BOOL "" FORCE)
endif()
if(UNIX AND NOT APPLE)
  set(WITH_DOC_MANPAGE         ON  CACHE BOOL "" FORCE)
  set(WITH_GHOST_XDND          ON  CACHE BOOL "" FORCE)
  set(WITH_PULSEAUDIO          ON  CACHE BOOL "" FORCE)
  set(WITH_X11_XINPUT          ON  CACHE BOOL "" FORCE)
  set(WITH_X11_XF86VMODE       ON  CACHE BOOL "" FORCE)
endif()
if(NOT APPLE)
  set(WITH_XR_OPENXR           ON  CACHE BOOL "" FORCE)
endif()
