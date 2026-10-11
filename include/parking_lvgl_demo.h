#ifndef PARKING_LVGL_DEMO_H
#define PARKING_LVGL_DEMO_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parking_lvgl_demo parking_lvgl_demo_t;

typedef struct {
    const char *font_path;
} parking_lvgl_demo_config_t;

parking_lvgl_demo_t *parking_lvgl_demo_create(
    const parking_lvgl_demo_config_t *config);

void parking_lvgl_demo_destroy(parking_lvgl_demo_t *demo);

#ifdef __cplusplus
}
#endif

#endif
