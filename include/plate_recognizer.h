#ifndef PLATE_RECOGNIZER_H
#define PLATE_RECOGNIZER_H

#ifdef __cplusplus
extern "C" {
#endif

#define PLATE_MAX_RESULTS 16
#define PLATE_TEXT_CAPACITY 32
#define PLATE_COLOR_CAPACITY 16

typedef enum plate_color {
  PLATE_COLOR_BLACK = 0,
  PLATE_COLOR_BLUE = 1,
  PLATE_COLOR_GREEN = 2,
  PLATE_COLOR_WHITE = 3,
  PLATE_COLOR_YELLOW = 4
} plate_color_t;

typedef struct plate_result {
  int x1;
  int y1;
  int x2;
  int y2;
  float detect_confidence;
  float color_confidence;
  int plate_type; /* 0: single layer, 1: double layer */
  plate_color_t color;
  float landmarks[4][2]; /* top-left, top-right, bottom-right, bottom-left */
  char plate_number[PLATE_TEXT_CAPACITY]; /* UTF-8 */
  char color_name[PLATE_COLOR_CAPACITY];  /* UTF-8 */
} plate_result_t;

typedef struct plate_recognizer plate_recognizer_t;

// 车牌识别器创建
plate_recognizer_t *plate_recognizer_create(const char *detect_model_path,
                                            const char *recognition_model_path,
                                            const char *font_path);
// 车牌识别器处理
int plate_recognizer_process(plate_recognizer_t *recognizer,
                             const unsigned char *bgr, int width, int height,
                             int stride, float confidence_threshold,
                             plate_result_t *results, int max_results);

// 车牌识别器绘制
int plate_recognizer_draw(plate_recognizer_t *recognizer, unsigned char *bgr,
                          int width, int height, int stride,
                          const plate_result_t *results, int result_count);

// 车牌识别器错误信息
const char *plate_recognizer_last_error(const plate_recognizer_t *recognizer);
// 车牌识别器销毁
void plate_recognizer_destroy(plate_recognizer_t *recognizer);

#ifdef __cplusplus
}
#endif
#endif
