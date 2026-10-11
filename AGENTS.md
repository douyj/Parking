# 项目开发约定

本项目以易读、紧凑、接近实际 C 工程的风格为准。修改代码时优先延续文件已有风格，不要把无关代码一起重排。

## C 代码排版

- 使用 4 个空格缩进，不使用 Tab。
- 大括号采用 K&R 风格，函数定义的大括号单独一行。
- 单行尽量不超过 100 个字符；只有确实超长时才换行。
- 短函数调用必须放在一行，不要把每个参数机械地拆成一行。
- 连续的同类调用保持对齐和紧凑，方便纵向比较。
- 长参数列表换行时使用悬挂缩进，不要制造大量只有一个参数的行。
- 不添加大段 `====`、`----` 等装饰性分隔注释。
- 注释说明“为什么这样做”或硬件/API 限制，不重复代码已经表达的内容。
- 不为简单语句添加教程式逐行注释。
- 变量尽量在第一次使用附近声明，但资源句柄统一初始化为 `NULL` 或无效值。

推荐：

```c
ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);

mpp_frame_set_width(frame, WIDTH);
mpp_frame_set_height(frame, HEIGHT);
mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
mpp_frame_set_buffer(frame, mpp_buffer);
```

不推荐：

```c
ret = mpp_init(
    ctx,
    MPP_CTX_ENC,
    MPP_VIDEO_CodingAVC
);

mpp_frame_set_width(
    frame,
    WIDTH
);
```

确实超长时再换行：

```c
ret = mpp_buffer_group_get_internal(&mpp_group,
                                    MPP_BUFFER_TYPE_DRM);
```