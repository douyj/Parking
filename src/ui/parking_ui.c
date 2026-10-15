#include "parking_ui.h"

#include <lvgl.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 360
#define UI_PLATE_CAPACITY 32U
#define UI_COLOR_CAPACITY 32U
#define UI_PAYMENT_URL_CAPACITY 512U

struct parking_ui {
    lv_display_t *display;
    lv_obj_t *screen;
    lv_obj_t *camera_canvas;
    lv_obj_t *payment_panel;
    lv_obj_t *payment_qrcode;
    lv_obj_t *payment_title;
    lv_obj_t *payment_hint;
    lv_obj_t *mode_label;
    lv_obj_t *space_label;
    lv_obj_t *plate_label;
    lv_obj_t *fee_label;
    lv_obj_t *business_label;
    lv_obj_t *gate_label;
    lv_obj_t *message_label;
    lv_font_t *font;
    void *font_data;
    uint32_t *camera_buffer;
    int lvgl_initialized;
    char plate_number[UI_PLATE_CAPACITY];
    char plate_color[UI_COLOR_CAPACITY];
    char payment_url[UI_PAYMENT_URL_CAPACITY];
};

static void print_lvgl_log(lv_log_level_t level, const char *message)
{
    if (level >= LV_LOG_LEVEL_WARN && message != NULL)
        fputs(message, stderr);
}

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

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
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
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

static int create_screen(parking_ui_t *ui)
{
    lv_obj_t *camera_card;
    lv_obj_t *status_card;
    lv_obj_t *bottom_card;
    lv_obj_t *label;

    ui->screen = lv_obj_create(NULL);
    if (ui->screen == NULL)
        return -1;

    lv_obj_set_style_bg_color(ui->screen, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(ui->screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(ui->screen, false);

    label = create_label(ui->screen, ui->font,
                         "智慧停车管理系统", 0xF8FAFC);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 18);

    ui->mode_label = create_label(ui->screen, ui->font,
                                  "入口车道", 0x38BDF8);
    lv_obj_align(ui->mode_label, LV_ALIGN_TOP_RIGHT, -24, 18);

    camera_card = create_card(ui->screen, 24, 68, 660, 390);
    lv_obj_set_style_pad_all(camera_card, 9, 0);

    ui->camera_canvas = lv_canvas_create(camera_card);
    if (ui->camera_canvas == NULL)
        return -1;
    lv_canvas_set_buffer(ui->camera_canvas, ui->camera_buffer,
                         CAMERA_WIDTH, CAMERA_HEIGHT,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_center(ui->camera_canvas);

    ui->payment_panel = lv_obj_create(camera_card);
    if (ui->payment_panel == NULL)
        return -1;
    lv_obj_set_size(ui->payment_panel, CAMERA_WIDTH, CAMERA_HEIGHT);
    lv_obj_center(ui->payment_panel);
    lv_obj_set_style_bg_color(ui->payment_panel,
                              lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(ui->payment_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui->payment_panel, 0, 0);
    lv_obj_set_style_radius(ui->payment_panel, 8, 0);
    lv_obj_set_scrollable(ui->payment_panel, false);

    ui->payment_qrcode = lv_qrcode_create(ui->payment_panel);
    if (ui->payment_qrcode == NULL)
        return -1;
    lv_qrcode_set_size(ui->payment_qrcode, 250);
    lv_qrcode_set_dark_color(ui->payment_qrcode, lv_color_hex(0x102A43));
    lv_qrcode_set_light_color(ui->payment_qrcode, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(ui->payment_qrcode, true);
    lv_obj_align(ui->payment_qrcode, LV_ALIGN_LEFT_MID, 28, 0);

    ui->payment_title = create_label(ui->payment_panel, ui->font,
                                     "正在生成支付二维码", 0xF8FAFC);
    lv_obj_set_width(ui->payment_title, 300);
    lv_label_set_long_mode(ui->payment_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(ui->payment_title, LV_ALIGN_TOP_RIGHT, -18, 78);

    ui->payment_hint = create_label(ui->payment_panel, ui->font,
                                    "请使用手机扫码完成模拟支付",
                                    0x94A3B8);
    lv_obj_set_width(ui->payment_hint, 300);
    lv_label_set_long_mode(ui->payment_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(ui->payment_hint, LV_ALIGN_TOP_RIGHT, -18, 138);
    lv_obj_set_hidden(ui->payment_qrcode, true);
    lv_obj_set_hidden(ui->payment_panel, true);

    status_card = create_card(ui->screen, 700, 68, 300, 390);
    label = create_label(status_card, ui->font, "停车场状态", 0x94A3B8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    ui->space_label = create_label(status_card, ui->font,
                                   "剩余 0 / 0", 0x34D399);
    lv_obj_align(ui->space_label, LV_ALIGN_TOP_LEFT, 0, 42);

    label = create_label(status_card, ui->font, "当前车辆", 0x94A3B8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 98);

    ui->plate_label = create_label(status_card, ui->font,
                                   "等待识别", 0xF8FAFC);
    lv_obj_align(ui->plate_label, LV_ALIGN_TOP_LEFT, 0, 138);

    label = create_label(status_card, ui->font, "本次费用", 0x94A3B8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 202);

    ui->fee_label = create_label(status_card, ui->font,
                                 "0.00 元", 0xF59E0B);
    lv_obj_align(ui->fee_label, LV_ALIGN_TOP_LEFT, 0, 242);

    ui->message_label = create_label(status_card, ui->font,
                                     "系统就绪", 0xCBD5E1);
    lv_obj_set_width(ui->message_label, 260);
    lv_label_set_long_mode(ui->message_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(ui->message_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    bottom_card = create_card(ui->screen, 24, 474, 976, 102);
    ui->business_label = create_label(bottom_card, ui->font,
                                      "业务状态：空闲", 0xA7F3D0);
    lv_obj_align(ui->business_label, LV_ALIGN_TOP_LEFT, 0, 0);

    ui->gate_label = create_label(bottom_card, ui->font,
                                  "道闸状态：已关闭", 0xBAE6FD);
    lv_obj_align(ui->gate_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    label = create_label(bottom_card, ui->font,
                         "实时识别运行中 · 按 Ctrl+C 安全退出", 0x64748B);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_screen_load(ui->screen);
    return 0;
}

static const char *business_state_text(parking_state_t state)
{
    switch (state) {
        case PARKING_STATE_INITIALIZING:
            return "初始化";
        case PARKING_STATE_IDLE:
            return "空闲";
        case PARKING_STATE_CHECKING:
            return "业务检查";
        case PARKING_STATE_WAIT_PAYMENT:
            return "等待支付";
        case PARKING_STATE_OPENING:
            return "正在开闸";
        case PARKING_STATE_WAIT_PASSAGE:
            return "等待通行";
        case PARKING_STATE_CLOSING:
            return "正在关闸";
        case PARKING_STATE_COMPLETING:
            return "正在提交";
        case PARKING_STATE_REJECT_COOLDOWN:
            return "业务拒绝";
        case PARKING_STATE_ERROR:
        default:
            return "异常";
    }
}

static const char *gate_state_text(gate_state_t state)
{
    switch (state) {
        case GATE_STATE_CLOSED:
            return "已关闭";
        case GATE_STATE_OPENING:
            return "正在打开";
        case GATE_STATE_OPEN:
            return "已打开";
        case GATE_STATE_CLOSING:
            return "正在关闭";
        case GATE_STATE_ERROR:
        default:
            return "异常";
    }
}

static const char *reason_text(parking_reason_t reason)
{
    switch (reason) {
        case PARKING_REASON_NONE:
            return "系统就绪";
        case PARKING_REASON_INVALID_PLATE:
            return "车牌格式无效";
        case PARKING_REASON_AMBIGUOUS_PLATE:
            return "识别结果存在歧义";
        case PARKING_REASON_LANE_BUSY:
            return "车道正在处理其他车辆";
        case PARKING_REASON_LANE_UNAVAILABLE:
            return "当前车道不可用";
        case PARKING_REASON_ALREADY_INSIDE:
            return "车辆已在场内";
        case PARKING_REASON_LOT_FULL:
            return "停车场车位已满";
        case PARKING_REASON_NO_ACTIVE_SESSION:
            return "没有有效停车记录";
        case PARKING_REASON_PAYMENT_FAILED:
            return "支付失败";
        case PARKING_REASON_GATE_FAILURE:
            return "道闸故障";
        case PARKING_REASON_COMMIT_FAILURE:
            return "停车记录提交失败";
        case PARKING_REASON_CHECK_TIMEOUT:
        case PARKING_REASON_PAYMENT_TIMEOUT:
        case PARKING_REASON_GATE_TIMEOUT:
        case PARKING_REASON_COMMIT_TIMEOUT:
            return "业务处理超时";
        case PARKING_REASON_CHECK_REJECTED:
            return "业务检查未通过";
        case PARKING_REASON_UNEXPECTED_EVENT:
        case PARKING_REASON_INTERNAL_ERROR:
        default:
            return "系统处理异常";
    }
}

static const char *message_text(const parking_ui_status_t *status)
{
    if (status->reason != PARKING_REASON_NONE)
        return reason_text(status->reason);

    switch (status->business_state) {
        case PARKING_STATE_IDLE:
            return "等待车辆进入识别区域";
        case PARKING_STATE_CHECKING:
            return "正在检查停车记录";
        case PARKING_STATE_WAIT_PAYMENT:
            return "等待完成支付";
        case PARKING_STATE_OPENING:
            return "道闸正在打开";
        case PARKING_STATE_WAIT_PASSAGE:
            return "请车辆安全通行";
        case PARKING_STATE_CLOSING:
            return "道闸正在关闭";
        case PARKING_STATE_COMPLETING:
            return "正在保存停车记录";
        case PARKING_STATE_REJECT_COOLDOWN:
            return "本次业务已拒绝";
        case PARKING_STATE_INITIALIZING:
            return "系统正在初始化";
        case PARKING_STATE_ERROR:
        default:
            return "系统处理异常";
    }
}

parking_ui_t *parking_ui_create(const parking_ui_config_t *config)
{
    parking_ui_t *ui;
    size_t font_size = 0;

    if (config == NULL || config->device == NULL ||
        config->font_path == NULL)
        return NULL;

    ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
        return NULL;

    ui->camera_buffer = calloc((size_t)CAMERA_WIDTH * CAMERA_HEIGHT,
                               sizeof(*ui->camera_buffer));
    if (ui->camera_buffer == NULL)
        goto fail;

    ui->font_data = read_file(config->font_path, &font_size);
    if (ui->font_data == NULL)
        goto fail;

    lv_init();
    ui->lvgl_initialized = 1;
    lv_log_register_print_cb(print_lvgl_log);

    ui->display = lv_linux_drm_create();
    if (ui->display == NULL)
        goto fail;
    if (lv_linux_drm_set_file(ui->display, config->device,
                              config->connector_id) != LV_RESULT_OK)
        goto fail;

    ui->font = lv_tiny_ttf_create_data(ui->font_data, font_size, 24);
    if (ui->font == NULL || create_screen(ui) != 0)
        goto fail;

    lv_refr_now(ui->display);
    return ui;

fail:
    parking_ui_destroy(ui);
    return NULL;
}

int parking_ui_present_bgr(parking_ui_t *ui, const unsigned char *data,
                           unsigned int width, unsigned int height,
                           unsigned int stride)
{
    unsigned int output_width;
    unsigned int output_height;
    unsigned int left;
    unsigned int top;
    unsigned int y;

    if (ui == NULL || ui->camera_canvas == NULL || data == NULL ||
        width == 0 || height == 0 || width > UINT_MAX / 3U ||
        stride < width * 3U)
        return -1;

    if ((uint64_t)CAMERA_WIDTH * height <=
        (uint64_t)CAMERA_HEIGHT * width) {
        output_width = CAMERA_WIDTH;
        output_height = (unsigned int)((uint64_t)height * CAMERA_WIDTH /
                                       width);
    } else {
        output_height = CAMERA_HEIGHT;
        output_width = (unsigned int)((uint64_t)width * CAMERA_HEIGHT /
                                      height);
    }
    if (output_width == 0 || output_height == 0)
        return -1;

    left = (CAMERA_WIDTH - output_width) / 2U;
    top = (CAMERA_HEIGHT - output_height) / 2U;
    memset(ui->camera_buffer, 0,
           (size_t)CAMERA_WIDTH * CAMERA_HEIGHT *
           sizeof(*ui->camera_buffer));

    for (y = 0; y < output_height; ++y) {
        unsigned int source_y =
            (unsigned int)((uint64_t)y * height / output_height);
        uint32_t *output = ui->camera_buffer +
            (size_t)(top + y) * CAMERA_WIDTH + left;
        unsigned int x;

        for (x = 0; x < output_width; ++x) {
            unsigned int source_x =
                (unsigned int)((uint64_t)x * width / output_width);
            const unsigned char *source = data +
                (size_t)source_y * stride + (size_t)source_x * 3U;

            output[x] = 0xFF000000U |
                        (uint32_t)source[0] |
                        (uint32_t)source[1] << 8U |
                        (uint32_t)source[2] << 16U;
        }
    }

    lv_obj_invalidate(ui->camera_canvas);
    return 0;
}

int parking_ui_set_status(parking_ui_t *ui,
                          const parking_ui_status_t *status)
{
    char buffer[160];
    const char *plate;
    const char *color;

    if (ui == NULL || status == NULL)
        return -1;

    plate = status->plate_number != NULL && status->plate_number[0] != '\0'
        ? status->plate_number : "等待识别";
    color = status->plate_color != NULL && status->plate_color[0] != '\0'
        ? status->plate_color : "颜色未知";
    snprintf(ui->plate_number, sizeof(ui->plate_number), "%s", plate);
    snprintf(ui->plate_color, sizeof(ui->plate_color), "%s", color);

    set_label_text(ui->mode_label,
                   status->lane_mode == PARKING_LANE_EXIT
                       ? "出口车道" : "入口车道");

    snprintf(buffer, sizeof(buffer), "剩余 %u / %u\n已使用 %u",
             status->available_spaces, status->total_spaces,
             status->occupied_spaces);
    set_label_text(ui->space_label, buffer);

    if (strcmp(plate, "等待识别") == 0) {
        set_label_text(ui->plate_label, plate);
    } else {
        snprintf(buffer, sizeof(buffer), "%s\n%s  %.0f%%",
                 ui->plate_number, ui->plate_color,
                 status->confidence * 100.0F);
        set_label_text(ui->plate_label, buffer);
    }

    snprintf(buffer, sizeof(buffer), "%" PRIu64 ".%02" PRIu64 " 元",
             status->amount_cents / 100U,
             status->amount_cents % 100U);
    set_label_text(ui->fee_label, buffer);

    snprintf(buffer, sizeof(buffer), "业务状态：%s",
             business_state_text(status->business_state));
    set_label_text(ui->business_label, buffer);

    snprintf(buffer, sizeof(buffer), "道闸状态：%s%s",
             gate_state_text(status->gate_state),
             status->gate_is_real ? "" : "（模拟）");
    set_label_text(ui->gate_label, buffer);
    set_label_text(ui->message_label, message_text(status));

    if (status->business_state == PARKING_STATE_WAIT_PAYMENT) {
        const char *payment_url = status->payment_url != NULL
            ? status->payment_url : "";

        lv_obj_set_hidden(ui->payment_panel, false);
        if (payment_url[0] == '\0') {
            set_label_text(ui->payment_title, "正在生成支付二维码");
            set_label_text(ui->payment_hint, "请稍候…");
            lv_obj_set_hidden(ui->payment_qrcode, true);
        } else {
            if (strcmp(payment_url, ui->payment_url) != 0) {
                if (strlen(payment_url) >= sizeof(ui->payment_url) ||
                    lv_qrcode_update(ui->payment_qrcode, payment_url,
                                     (uint32_t)strlen(payment_url)) !=
                        LV_RESULT_OK)
                    return -1;
                memcpy(ui->payment_url, payment_url,
                       strlen(payment_url) + 1U);
            }
            set_label_text(ui->payment_title, "扫码支付停车费");
            set_label_text(ui->payment_hint,
                           "手机确认后，道闸将自动继续流程");
            lv_obj_set_hidden(ui->payment_qrcode, false);
        }
    } else {
        ui->payment_url[0] = '\0';
        lv_obj_set_hidden(ui->payment_panel, true);
    }
    return 0;
}

void parking_ui_process(parking_ui_t *ui)
{
    if (ui != NULL)
        (void)lv_timer_handler();
}

void parking_ui_destroy(parking_ui_t *ui)
{
    if (ui == NULL)
        return;

    if (ui->screen != NULL) {
        lv_obj_t *replacement = lv_obj_create(NULL);

        if (replacement != NULL)
            lv_screen_load(replacement);
        lv_obj_delete(ui->screen);
        ui->screen = NULL;
    }
    if (ui->font != NULL)
        lv_tiny_ttf_destroy(ui->font);
    if (ui->display != NULL)
        lv_display_delete(ui->display);
    if (ui->lvgl_initialized)
        lv_deinit();

    free(ui->font_data);
    free(ui->camera_buffer);
    free(ui);
}
