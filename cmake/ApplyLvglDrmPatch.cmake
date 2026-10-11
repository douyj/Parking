if(NOT DEFINED LVGL_SOURCE_DIR OR NOT DEFINED LVGL_DRM_PATCH)
    message(FATAL_ERROR "LVGL_SOURCE_DIR and LVGL_DRM_PATCH are required")
endif()

set(_drm_source
    "${LVGL_SOURCE_DIR}/src/drivers/display/drm/lv_linux_drm.c")
if(NOT EXISTS "${_drm_source}" OR NOT EXISTS "${LVGL_DRM_PATCH}")
    message(FATAL_ERROR "LVGL DRM source or patch file not found")
endif()

file(READ "${_drm_source}" _drm_contents)
string(FIND "${_drm_contents}"
    "static void drm_add_plane_max_zpos(drm_dev_t * drm_dev)\n{"
    _patch_marker)
if(NOT _patch_marker EQUAL -1)
    return()
endif()

find_program(_patch_program patch)
if(NOT _patch_program)
    message(FATAL_ERROR "The patch program is required to patch LVGL DRM")
endif()

execute_process(
    COMMAND "${_patch_program}" -p1 --forward --input "${LVGL_DRM_PATCH}"
    WORKING_DIRECTORY "${LVGL_SOURCE_DIR}"
    RESULT_VARIABLE _patch_result
)
if(NOT _patch_result EQUAL 0)
    message(FATAL_ERROR "Unable to apply ${LVGL_DRM_PATCH}")
endif()
