# CMake cache file for examples

#########################
# EXTERNAL cache entries
#########################

# set example options to match build options
set (H5EX_BUILD_TESTING ${BUILD_TESTING} CACHE BOOL "Enable examples testing" FORCE)
set (H5EX_BUILD_EXAMPLES ${HDF5_BUILD_EXAMPLES} CACHE BOOL "Build Examples" FORCE)
set (HDF_BUILD_FORTRAN ${HDF5_BUILD_FORTRAN} CACHE BOOL "Build examples FORTRAN support" FORCE)
set (HDF_BUILD_JAVA ${HDF5_BUILD_JAVA} CACHE BOOL "Build examples JAVA support" FORCE)
set (HDF_BUILD_FILTERS ${HDF5_ENABLE_PLUGIN_SUPPORT} CACHE BOOL "Build examples PLUGIN filter support" FORCE)
set (HDF_BUILD_CPP_LIB ${HDF5_BUILD_CPP_LIB} CACHE BOOL "Build HDF5 C++ Library" FORCE)
set (HDF_BUILD_HL_LIB ${HDF5_BUILD_HL_LIB} CACHE BOOL "Build HIGH Level examples" FORCE)
set (HDF_ENABLE_THREADSAFE ${HDF5_ENABLE_THREADSAFE} CACHE BOOL "Enable examples thread-safety" FORCE)
set (HDF_ENABLE_PARALLEL ${HDF5_ENABLE_PARALLEL} CACHE BOOL "Enable examples parallel build (requires MPI)" FORCE)
set (H5EX_USE_GNU_DIRS ${HDF5_USE_GNU_DIRS} CACHE BOOL "TRUE to use GNU Coding Standard install directory variables, FALSE to use historical settings" FORCE)

#preset HDF5 cache vars to this projects libraries instead of searching
set (H5EX_HDF5_HEADER "H5pubconf.h" CACHE STRING "Name of HDF5 header" FORCE)
#set (H5EX_HDF5_INCLUDE_DIRS $<TARGET_PROPERTY:${HDF5_LIBSH_TARGET},INCLUDE_DIRECTORIES> CACHE PATH "HDF5 include dirs" FORCE)
set (H5EX_HDF5_INCLUDE_DIRS "${HDF5_SRC_INCLUDE_DIRS};${HDF5_SRC_BINARY_DIR}" CACHE PATH "HDF5 include dirs" FORCE)
set (H5EX_HDF5_DIR ${CMAKE_CURRENT_BINARY_DIR} CACHE STRING "HDF5 build folder" FORCE)
set (EXAMPLES_EXTERNALLY_CONFIGURED ON CACHE BOOL "Examples build is used in another project" FORCE)

if (NOT BUILD_SHARED_LIBS AND BUILD_STATIC_LIBS)
  set (USE_SHARED_LIBS OFF CACHE BOOL "Use Shared Libraries for Examples" FORCE)
  set (H5EX_HDF5_LINK_LIBS ${HDF5_LIB_TARGET} CACHE STRING "HDF5 target" FORCE)
  if (HDF5_BUILD_FORTRAN)
    set (H5EX_HDF5_LINK_LIBS ${H5EX_HDF5_LINK_LIBS} ${HDF5_F90_LIB_TARGET})
    set (HDF_MOD_EXT "/static" CACHE STRING "Use Static Modules for Examples" FORCE)
  endif ()
else ()
  set (USE_SHARED_LIBS ON CACHE BOOL "Use Shared Libraries for Examples" FORCE)
  set (H5EX_HDF5_LINK_LIBS ${HDF5_LIBSH_TARGET} CACHE STRING "HDF5 target" FORCE)
  if (HDF5_BUILD_FORTRAN)
    set (H5EX_HDF5_LINK_LIBS ${H5EX_HDF5_LINK_LIBS} ${HDF5_F90_LIBSH_TARGET})
    set (HDF_MOD_EXT "/shared" CACHE STRING "Use Shared Modules for Examples" FORCE)
  endif ()
  if (HDF5_BUILD_JAVA)
    set (HDF5_JAVA_INCLUDE_DIRS ${HDF5_JAVA_JARS} ${HDF5_JAVA_LOGGING_JAR})
    set (H5EX_JAVA_LIBRARY ${HDF5_JAVA_JNI_LIB_TARGET})
    set (H5EX_JAVA_LIBRARIES ${HDF5_JAVA_HDF5_LIB_TARGET} ${HDF5_JAVA_JNI_LIB_TARGET})
    set (HDF5_LIBRARY_PATH ${CMAKE_TEST_OUTPUT_DIRECTORY})
    message (STATUS "HDF5 Example java lib: ${H5EX_JAVA_LIBRARY} jars: ${HDF5_JAVA_INCLUDE_DIRS}")
  endif ()
  if (HDF5_ENABLE_PLUGIN_SUPPORT)
    set (H5EX_HDF5_PLUGIN_PATH "${CMAKE_BINARY_DIR}/plugins")
  endif ()
endif ()
message (STATUS "HDF5 Example link libs: ${H5EX_HDF5_LINK_LIBS} Includes: ${H5EX_HDF5_INCLUDE_DIRS}")

set (HDF5_TOOLS_DIR ${CMAKE_TEST_OUTPUT_DIRECTORY} CACHE STRING "HDF5 Directory for all Executables" FORCE)
set (H5EX_HDF5_DUMP_EXECUTABLE $<TARGET_FILE:h5dump${tgt_file_ext}> CACHE STRING "HDF5 h5dump target" FORCE)
set (H5EX_HDF5_REPACK_EXECUTABLE $<TARGET_FILE:h5repack${tgt_file_ext}> CACHE STRING "HDF5 h5repack target" FORCE)
