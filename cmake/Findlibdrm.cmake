include(FindPackageHandleStandardArgs)

set(_libdrm_roots)

if(PARKING_LIBDRM_ROOT)
    list(APPEND _libdrm_roots "${PARKING_LIBDRM_ROOT}")
endif()

if(PARKING_LIBDRM_DEV_DEB OR PARKING_LIBDRM_RUNTIME_DEB)
    if(NOT PARKING_LIBDRM_DEV_DEB OR NOT PARKING_LIBDRM_RUNTIME_DEB)
        message(FATAL_ERROR
            "Both PARKING_LIBDRM_DEV_DEB and PARKING_LIBDRM_RUNTIME_DEB are required")
    endif()
    if(NOT EXISTS "${PARKING_LIBDRM_DEV_DEB}" OR
       NOT EXISTS "${PARKING_LIBDRM_RUNTIME_DEB}")
        message(FATAL_ERROR "Configured ARM64 libdrm package does not exist")
    endif()

    find_program(DPKG_DEB_EXECUTABLE dpkg-deb)
    if(NOT DPKG_DEB_EXECUTABLE)
        message(FATAL_ERROR
            "dpkg-deb is required to extract the configured libdrm packages")
    endif()
    set(_libdrm_extract_dir "${CMAKE_BINARY_DIR}/_deps/libdrm-root")
    set(_libdrm_stamp "${_libdrm_extract_dir}/.parking-extracted")

    if(NOT EXISTS "${_libdrm_stamp}")
        file(REMOVE_RECURSE "${_libdrm_extract_dir}")
        file(MAKE_DIRECTORY "${_libdrm_extract_dir}")

        execute_process(
            COMMAND "${DPKG_DEB_EXECUTABLE}" -x
                    "${PARKING_LIBDRM_DEV_DEB}" "${_libdrm_extract_dir}"
            RESULT_VARIABLE _libdrm_dev_result
        )
        execute_process(
            COMMAND "${DPKG_DEB_EXECUTABLE}" -x
                    "${PARKING_LIBDRM_RUNTIME_DEB}" "${_libdrm_extract_dir}"
            RESULT_VARIABLE _libdrm_runtime_result
        )
        if(NOT _libdrm_dev_result EQUAL 0 OR
           NOT _libdrm_runtime_result EQUAL 0)
            message(FATAL_ERROR "Failed to extract ARM64 libdrm packages")
        endif()

        file(WRITE "${_libdrm_stamp}" "libdrm packages extracted\n")
    endif()

    list(APPEND _libdrm_roots "${_libdrm_extract_dir}")
endif()

find_path(libdrm_INCLUDE_DIR
    NAMES xf86drm.h
    HINTS ${_libdrm_roots}
    PATH_SUFFIXES usr/include include
)

find_path(libdrm_DRM_INCLUDE_DIR
    NAMES drm.h drm_fourcc.h
    HINTS ${_libdrm_roots}
    PATH_SUFFIXES usr/include/libdrm include/libdrm libdrm
)

find_library(libdrm_LIBRARY
    NAMES drm
    HINTS ${_libdrm_roots}
    PATH_SUFFIXES usr/lib/aarch64-linux-gnu usr/lib lib
)

find_package_handle_standard_args(libdrm
    REQUIRED_VARS
        libdrm_LIBRARY
        libdrm_INCLUDE_DIR
        libdrm_DRM_INCLUDE_DIR
)

if(libdrm_FOUND AND NOT TARGET libdrm::libdrm)
    add_library(libdrm::libdrm UNKNOWN IMPORTED)
    set_target_properties(libdrm::libdrm PROPERTIES
        IMPORTED_LOCATION "${libdrm_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${libdrm_INCLUDE_DIR};${libdrm_DRM_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(
    libdrm_LIBRARY
    libdrm_INCLUDE_DIR
    libdrm_DRM_INCLUDE_DIR
)
