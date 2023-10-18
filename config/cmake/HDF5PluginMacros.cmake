#
# Copyright by The HDF Group.
# All rights reserved.
#
# This file is part of HDF5.  The full HDF5 copyright notice, including
# terms governing use, modification, and redistribution, is contained in
# the COPYING file, which can be found at the root of the source code
# distribution tree, or in https://www.hdfgroup.org/licenses.
# If you do not have access to either file, you may request a copy from
# help@hdfgroup.org.
#
#-------------------------------------------------------------------------------
# Plugins must be built SHARED
#-------------------------------------------------------------------------------
macro (EXTERNAL_PLUGIN_LIBRARY compress_type)
  if (${compress_type} MATCHES "GIT")
    FetchContent_Declare (PLUGIN
        GIT_REPOSITORY ${PLUGIN_URL}
        GIT_TAG ${PLUGIN_BRANCH}
    )
  elseif (${compress_type} MATCHES "TGZ")
    FetchContent_Declare (PLUGIN
        URL ${PLUGIN_URL}
        URL_HASH ""
    )
  endif ()
  FetchContent_GetProperties(PLUGIN)
  message (VERBOSE "HDF5_INCLUDE_DIR=${HDF5_INCLUDE_DIR}")
  if(NOT PLUGIN_POPULATED)
    FetchContent_Populate(PLUGIN)
    include (${HDF_RESOURCES_DIR}/HDF5PluginCache.cmake)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    add_subdirectory(${plugin_SOURCE_DIR} ${plugin_BINARY_DIR})
    if (ENABLE_BLOSC)
      add_dependencies (h5blosc ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_blosc ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_blosc PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_BSHUF)
      add_dependencies (h5bshuf ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_bshuf ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_bshuf PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_BZIP2)
      add_dependencies (h5bz2 ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_bzip2 ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_bzip2 PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_JPEG)
      add_dependencies (h5jpeg ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_jpeg ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_jpeg PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_LZ4)
      add_dependencies (h5lz4 ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_lz4 ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_lz4 PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_LZF)
      add_dependencies (h5lzf ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_lzf ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_lzf PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_MAFISC)
      add_dependencies (h5mafisc ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_mafisc ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_mafisc PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_SZ)
      add_dependencies (h5sz ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_sz ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_sz PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_ZFP)
      add_dependencies (h5zfp ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_zfp ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_zfp PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
    if (ENABLE_ZSTD)
      add_dependencies (h5zstd ${HDF5_LIBSH_TARGET})
      add_dependencies (h5ex_d_zstd ${HDF5_LIBSH_TARGET})
      target_include_directories (h5ex_d_zstd PRIVATE "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}")
    endif ()
  endif ()
  message (VERBOSE "HDF5_INCLUDE_DIR=${HDF5_INCLUDE_DIR}")
  set (PLUGIN_BINARY_DIR "${plugin_BINARY_DIR}")
  set (PLUGIN_SOURCE_DIR "${plugin_SOURCE_DIR}")
  set (PLUGIN_LIBRARY "PLUGIN")
  set (PLUGIN_FOUND 1)
endmacro ()

#-------------------------------------------------------------------------------
macro (FILTER_OPTION plname)
  string(TOLOWER ${plname} PLUGIN_NAME)
  option (ENABLE_${plname} "Enable Library Building for ${plname} plugin" ON)
  if (ENABLE_${plname})
    option (HDF_${plname}_USE_EXTERNAL "Use External Library Building for ${PLUGIN_NAME} plugin" OFF)
    mark_as_advanced (HDF_${plname}_USE_EXTERNAL)
    if (H5PL_ALLOW_EXTERNAL_SUPPORT MATCHES "GIT" OR H5PL_ALLOW_EXTERNAL_SUPPORT MATCHES "TGZ")
      set (HDF_${plname}_USE_EXTERNAL ON CACHE BOOL "Use External Library Building for ${PLUGIN_NAME} plugin" FORCE)
      if (H5PL_ALLOW_EXTERNAL_SUPPORT MATCHES "GIT")
        set (HDF_${plname}_URL ${HDF_${plname}_GIT_URL})
        set (HDF_${plname}_BRANCH ${HDF_${plname}_GIT_BRANCH})
      elseif (H5PL_ALLOW_EXTERNAL_SUPPORT MATCHES "TGZ")
        if (NOT H5PL_COMP_TGZPATH)
          set (H5PL_COMP_TGZPATH ${H5PL_SOURCE_DIR}/libs)
        endif ()
        set (HDF_${plname}_URL ${H5PL_COMP_TGZPATH}/${HDF_${plname}_TGZ_NAME})
      endif ()
    endif ()
    add_subdirectory (${plname})
    set_global_variable (H5PL_LIBRARIES_TO_EXPORT "${H5PL_LIBRARIES_TO_EXPORT};${H5${plname}_LIBRARIES_TO_EXPORT}")
  endif ()
endmacro ()

#-------------------------------------------------------------------------------
macro (PACKAGE_PLUGIN_LIBRARY compress_type)
  if (${compress_type} MATCHES "GIT" OR ${compress_type} MATCHES "TGZ")
    message (STATUS "Filter PLUGIN is to be packaged")
  endif ()
endmacro ()
