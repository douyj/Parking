#include "parking_lvgl_demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parking_lvgl_demo {
    lv_obj_t *screen;
    lv_font_t *font;
    void *font_data;
};

static void *read_file(const char *path, size_t *size)
{
    unsigned char *data = NULL;
    FILE *file = NULL;
    long length;

    if (path == NULL || path[0] == '\0' || size == NULL)
        return NULL;

    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0)
        goto fail;

    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
        goto fail;

    data = malloc((size_t)length);
    if (data == NULL)
        goto fail;
    if (fread(data, 1, (size_t)length, file) != (size_t)length)
        goto fail;

    fclose(file);
    *size = (size_t)length;
    return data;

fail:
    free(data);
    fclose(file);
    return NULL;
}

static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font,
                              const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *create_card(lv_obj_t *parent, int32_t x, int32_t y,
                             int32_t width, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x172033), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2C3A52), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

static void create_space_card(lv_obj_t *parent, const lv_font_t *font,
                              int32_t x, int32_t width,
                              const char *title, const char *value,
                              uint32_t value_color)
{
    lv_obj_t *card;
    lv_obj_t *label;

    card = create_card(parent, x, 104, width, 126);
    label = create_label(card, font, title, 0x94A3B8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = create_label(card, font, value, value_color);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static int create_screen(parking_lvgl_demo_t *demo)
{
    lv_display_t *display = lv_display_get_default();
    lv_obj_t *screen;
    lv_obj_t *label;
    lv_obj_t *card;
    int32_t width;
    int32_t content_width;
    int32_t summary_width;

    if (display == NULL)
        return -1;

    width = lv_display_get_horizontal_resolution(display);
    if (width < 640)
        return -1;

    screen = lv_obj_create(NULL);
    if (screen == NULL)
        return -1;

    demo->screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    label = create_label(screen, demo->font, "智慧停车管理系统", 0xF8FAFC);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 36, 25);

    label = create_label(screen, demo->font, "LVGL 9.6 · DRM/KMS", 0x38BDF8);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -36, 29);

    content_width = width - 72;
    summary_width = (content_width - 36) / 3;
    create_space_card(screen, demo->font, 36, summary_width,
                      "总车位", "100", 0xF8FAFC);
    create_space_card(screen, demo->font, 36 + summary_width + 18,
                      summary_width, "已使用", "73", 0xF59E0B);
    create_space_card(screen, demo->font,
                      36 + (summary_width + 18) * 2,
                      content_width - summary_width * 2 - 36,
                      "剩余车位", "27", 0x34D399);

    card = create_card(screen, 36, 258, content_width, 252);
    label = create_label(card, demo->font, "当前车道", 0x94A3B8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    label = create_label(card, demo->font, "入口 · 等待车辆", 0xF8FAFC);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 42);

    label = create_label(card, demo->font,
                         "道闸状态：已关闭    业务状态：空闲", 0xA7F3D0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 92);

    label = create_label(card, demo->font,
                         "最近车辆：粤A12345    入场时间：15:32", 0xCBD5E1);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 142);

    label = create_label(screen, demo->font,
                         "显示后端测试 · 按 Ctrl+C 安全退出", 0x64748B);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_screen_load(screen);
    return 0;
}

parking_lvgl_demo_t *parking_lvgl_demo_create(
    const parking_lvgl_demo_config_t *config)
{
    parking_lvgl_demo_t *demo;
    size_t font_size = 0;

    if (config == NULL || config->font_path == NULL)
        return NULL;

    demo = calloc(1, sizeof(*demo));
    if (demo == NULL)
        return NULL;

    demo->font_data = read_file(config->font_path, &font_size);
    if (demo->font_data == NULL)
        goto fail;

    demo->font = lv_tiny_ttf_create_data(demo->font_data, font_size, 27);
    if (demo->font == NULL)
        goto fail;

    if (create_screen(demo) != 0)
        goto fail;

    return demo;

fail:
    parking_lvgl_demo_destroy(demo);
    return NULL;
}

void parking_lvgl_demo_destroy(parking_lvgl_demo_t *demo)
{
    if (demo == NULL)
        return;

    if (demo->screen != NULL) {
        lv_obj_t *replacement = lv_obj_create(NULL);

        if (replacement != NULL)
            lv_screen_load(replacement);
        lv_obj_delete(demo->screen);
    }

    lv_tiny_ttf_destroy(demo->font);
    free(demo->font_data);
    free(demo);
}
