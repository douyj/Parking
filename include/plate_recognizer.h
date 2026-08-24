#ifndef PLATE_RECOGNIZER_H
#define PLATE_RECOGNIZER_H

#ifdef __cplusplus
extern "C" {
#endif

#define PLATE_MAX_RESULTS 16        // 最大识别结果数量
#define PLATE_TEXT_CAPACITY 32      // 最大车牌号长度
#define PLATE_COLOR_CAPACITY 16     // 最大颜色名称长度

// 车牌颜色枚举
typedef enum plate_color {
  PLATE_COLOR_BLACK = 0,
  PLATE_COLOR_BLUE = 1,
  PLATE_COLOR_GREEN = 2,
  PLATE_COLOR_WHITE = 3,
  PLATE_COLOR_YELLOW = 4
} plate_color_t;

// 车牌识别结果结构体
typedef struct plate_result {
  // 车牌框坐标
  int x1;
  int y1;
  int x2;
  int y2;

  float detect_confidence;     // 检测置信度
  float color_confidence;      // 颜色置信度
  int plate_type;              // 车牌类型，0:单层，1:双层
  plate_color_t color;         // 车牌颜色
  float landmarks[4][2];       // 车牌框的四个角点坐标
  char plate_number[PLATE_TEXT_CAPACITY];   // UTF-8 编码的车牌号
  char color_name[PLATE_COLOR_CAPACITY];     // UTF-8 中文颜色名称
} plate_result_t;

typedef struct plate_recognizer plate_recognizer_t;

/*
  @brief 创建车牌识别器
  @param detect_model_path 检测模型路径
  @param recognition_model_path 识别模型路径
  @param font_path 字体路径
  @return 车牌识别器指针
  @note 模型路径和字体路径必须是有效的文件路径
*/
plate_recognizer_t *plate_recognizer_create(const char *detect_model_path,
                                            const char *recognition_model_path,
                                            const char *font_path);

                                            
/*
  @brief 处理车牌识别器
  @param recognizer 车牌识别器指针
  @param bgr 输入图像指针，BGR格式
  @param width 输入图像宽度
  @param height 输入图像高度
  @param stride 输入图像步长
  @param confidence_threshold 置信阈值
  @param results 输出结果数组指针
  @param max_results 最大结果数量
  @return 识别结果数量
*/
int plate_recognizer_process(plate_recognizer_t *recognizer,
                             const unsigned char *bgr, int width, int height,
                             int stride, float confidence_threshold,
                             plate_result_t *results, int max_results);


/*
  @brief 绘制车牌识别结果
  @param recognizer 车牌识别器指针
  @param bgr 输入图像指针，BGR格式
  @param width 输入图像宽度
  @param height 输入图像高度
  @param stride 输入图像步长
  @param results 识别结果数组指针
  @param result_count 识别结果数量
  @return 0 成功，-1 失败
*/
int plate_recognizer_draw(plate_recognizer_t *recognizer, unsigned char *bgr,
                          int width, int height, int stride,
                          const plate_result_t *results, int result_count);

/*
  @brief 获取车牌识别器的错误信息
  @param recognizer 车牌识别器指针
  @return 错误信息字符串指针，必须由调用者释放
*/
const char *plate_recognizer_last_error(const plate_recognizer_t *recognizer);

/*
  @brief 销毁车牌识别器
  @param recognizer 车牌识别器指针
*/
void plate_recognizer_destroy(plate_recognizer_t *recognizer);

#ifdef __cplusplus
}
#endif
#endif
