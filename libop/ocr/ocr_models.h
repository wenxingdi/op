#pragma once
// OCR 模型资源 ID（编译进 DLL 资源段，运行时从内存加载，无需外部文件）
#define IDR_OCR_DET  101
#define IDR_OCR_REC  102
#define IDR_OCR_CLS  103   // PP-LCNet x0.25 textline_ori 方向分类（0/180 度，v4/v6 共用）
#define IDR_OCR_KEYS 104
// V4 快档（B8 多档热切换）：坐标小图等速度敏感场景
#define IDR_OCR_V4_DET  201
#define IDR_OCR_V4_REC  202
#define IDR_OCR_V4_KEYS 203
