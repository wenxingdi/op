#!/usr/bin/env python3
"""用 OP 前台模式截屏生成 OpenCV 素材用例所需的图片素材。

背景：tests/opencv_test.cpp 里的 RealPhoto / GameScene / Hard 三组用例需要
"大尺寸真实截图 + 对应模板"，长期因素材缺失而 SKIP。本脚本以 **前台模式截屏**
（op_c_api_x64.dll -> OpCapture，未绑定自动绑桌面）为素材来源：

  1. 前台截屏得到 base 图（>=1200px 宽）
  2. 从 base 中自动挑选互不重叠的高纹理块作为 patch
  3. 按用例期望的 rect 把 patch 贴到画布上，落盘成 source
  4. **回读落盘文件**再裁模板（保证 JPEG 压缩后模板与 source 像素一致）
  5. 用 FFT 版 CCOEFF_NORMED 自检：最佳位置是否等于期望 rect、次佳峰值是否够低

产物写进 assets/（.gitignore，不入库）；自检报告写进 workbench/。

用法:
    python workbench/make_screen_assets.py                # 用已有截屏
    python workbench/make_screen_assets.py --recapture    # 重新前台截屏
"""

import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
ASSETS = os.path.join(REPO, "assets")
PROBES = os.path.join(HERE, "probes")
DEFAULT_CAPTURE = os.path.join(PROBES, "fg_capture_01.bmp")
CAPTURE_PY = os.path.join(HERE, "op_foreground_capture.py")

CANVAS = (1920, 1200)  # 满足用例对 ">=1200px 宽" 的要求

# ---------------------------------------------------------------- patch 需求
# (标签, 宽, 高)  —— 顺序即挑选优先级，标签相同尺寸也要互不重叠
PATCH_SPECS = [
    ("photo1", 96, 96),
    ("photo2", 96, 96),
    ("photo3", 96, 96),
    ("photo4", 96, 96),
    ("photo5", 96, 96),
    ("photo6", 96, 96),
    ("scale_real", 96, 96),
    ("scale_hard", 96, 96),
    ("brightness", 96, 96),
    ("repeated", 96, 96),
    ("rotation", 120, 120),
    ("coin", 70, 70),
    ("gem", 70, 70),
    ("scene", 128, 128),
    ("edge", 90, 114),
    ("feature", 156, 156),
    ("shape", 240, 275),
    ("stop", 445, 449),
    ("real_shape", 421, 425),
    ("similar_shape", 413, 423),
    ("occ", 120, 120),
]

# ------------------------------------------------------- source 合成声明
# patches: (patch标签, x, y, scale)  scale=1.0 表示原尺寸
SOURCES = {
    # RealPhotoTemplateCases 六张 + 期望 rect
    "opencv_real_source.jpg": [("photo1", 860, 130, 1.0)],
    "opencv_real_city_source.jpg": [("photo2", 700, 260, 1.0)],
    "opencv_real_fruit_source.jpg": [("photo3", 520, 250, 1.0)],
    "opencv_real_sign_source.jpg": [("photo4", 585, 190, 1.0)],
    "opencv_real_space_source.jpg": [("photo5", 560, 300, 1.0)],
    "opencv_real_texture_source.jpg": [("photo6", 640, 320, 1.0)],
    # real_scale：模板 96x96，在 source 中以 1.5x(144x144) 出现在 (80,420)
    "opencv_real_scale_source.png": [("scale_real", 80, 420, 1.5)],
    # real_stop：445x449 大块原位 = 模板尺寸
    "opencv_real_stop_source.jpg": [("stop", 412, 222, 1.0)],
    # real_shape：421x425 大块原位
    "opencv_real_shape_source.png": [("real_shape", 520, 140, 1.0)],
    # GameScene：scene/coin/gem/edge 共用一张
    "opencv_game_scene_source.png": [
        ("coin", 170, 185, 1.0),     # coin 原位（MatchAny 先扫到这里）
        ("gem", 455, 325, 1.0),
        ("scene", 300, 488, 1.0),
        ("edge", 1060, 240, 1.0),
        ("coin", 1320, 620, 1.0),    # 额外 3 处 coin -> MatchAll 至少 4 处
        ("coin", 1500, 620, 1.0),
        ("coin", 1680, 620, 1.0),
    ],
    # game_scale：coin 以 1.5x 出现在 (1040,120)，搜索区 {1000,90,190,160}
    "opencv_game_scale_source.png": [("coin", 1040, 120, 1.5)],
    "opencv_game_feature_source.png": [("feature", 837, 102, 1.0)],
    "opencv_game_shape_source.png": [("shape", 420, 65, 1.0)],
    # Hard：hard_scale 三处不同缩放
    #   (90,450) 0.75x -> 72x72   （auto 小图档）
    #   (940,390) 1.35x -> 130x130（显式 1.35 档）
    #   (1560,760) 1.25x -> 120x120（auto 局部大图档）
    "opencv_hard_scale_source.png": [
        ("scale_hard", 90, 450, 0.75),
        ("scale_hard", 940, 390, 1.35),
        ("scale_hard", 1560, 760, 1.25),
    ],
    "opencv_hard_brightness_source.png": [("brightness", 460, 430, 1.0)],
    "opencv_hard_repeated_source.png": [
        ("repeated", 200, 160, 1.0),
        ("repeated", 520, 160, 1.0),
        ("repeated", 840, 160, 1.0),
        ("repeated", 1160, 160, 1.0),
        ("repeated", 1480, 160, 1.0),
    ],
    # hard_rotation：source 中是原方向，模板旋转 90 度
    "opencv_hard_rotation_source.png": [("rotation", 610, 130, 1.0)],
    # hard_occlusion：物体被遮住一半 -> 完整模板必须匹配不上
    "opencv_hard_occlusion_source.png": [("occ", 300, 300, 1.0)],
    # hard_similar_shapes：暗底亮块，形状匹配用
    "opencv_hard_similar_shapes_source.png": [("similar_shape", 520, 140, 1.0)],
}

# hard_occlusion：物体被遮住大半 -> 完整模板必须匹配不上。
# 遮挡色取暗底色，等效于该块直接消失。
OCCLUSIONS = {
    "opencv_hard_occlusion_source.png": [(320, 300, 84, 120, 45)],
}

# source 后处理：亮度偏移（hard_brightness 用固定灰底以便 +40 不饱和）
BRIGHTNESS_DELTA = {"opencv_hard_brightness_source.png": 40}
# 固定灰底（避免 CCOEFF_NORMED 分母为 0 的退化区，且给 shape 用例提供强对比背景）
FLAT_BACKGROUND = {
    "opencv_hard_brightness_source.png": 96,
    "opencv_real_shape_source.png": 45,
    "opencv_game_shape_source.png": 45,
    "opencv_hard_similar_shapes_source.png": 45,
    "opencv_hard_occlusion_source.png": 45,
}
# 这些 patch 需要"干净的高亮灰阶形状/高对比块"：
#  - ShapeMatchTemplate 走 Otsu(RETR_EXTERNAL) 取最大轮廓，任意纹理块会被二值化
#    成碎块（实测最大轮廓只剩 19x19）；
#  - 默认 method 是 TM_SQDIFF_NORMED，平坦背景与模板均值接近时归一化分母被抵消，
#    会出现虚假高分。压到高亮灰阶 + 暗底可同时解决这两件事。
SHAPE_LIKE_PATCHES = {"real_shape", "shape", "similar_shape", "occ"}

# ------------------------------------------------------- 模板声明
# (模板文件, kind, source文件, rect)
#   crop             从落盘后的 source 裁 rect
#   crop_clean       从"未做后处理"的合成图裁 rect（亮度用例：模板保持原亮度）
#   patch            直接用 patch 原尺寸（缩放用例：模板尺寸 != source 中呈现尺寸）
#   patch_rot90      patch 旋转 90 度
TEMPLATES = [
    ("opencv_real_template.png", "crop", "opencv_real_source.jpg", (860, 130, 96, 96)),
    ("opencv_real_city_template.png", "crop", "opencv_real_city_source.jpg", (700, 260, 96, 96)),
    ("opencv_real_fruit_template.png", "crop", "opencv_real_fruit_source.jpg", (520, 250, 96, 96)),
    ("opencv_real_sign_template.png", "crop", "opencv_real_sign_source.jpg", (585, 190, 96, 96)),
    ("opencv_real_space_template.png", "crop", "opencv_real_space_source.jpg", (560, 300, 96, 96)),
    ("opencv_real_texture_template.png", "crop", "opencv_real_texture_source.jpg", (640, 320, 96, 96)),
    ("opencv_real_scale_template.png", "patch", "opencv_real_scale_source.png", (80, 420, 96, 96)),
    ("opencv_real_stop_template.png", "crop", "opencv_real_stop_source.jpg", (412, 222, 445, 449)),
    ("opencv_real_shape_template.png", "crop_alpha", "opencv_real_shape_source.png", (520, 140, 421, 425)),
    ("opencv_game_scene_template.png", "crop", "opencv_game_scene_source.png", (300, 488, 128, 128)),
    ("opencv_game_coin_template.png", "patch", "opencv_game_scene_source.png", (170, 185, 70, 70)),
    ("opencv_game_gem_template.png", "crop", "opencv_game_scene_source.png", (455, 325, 70, 70)),
    ("opencv_game_edge_template.png", "crop", "opencv_game_scene_source.png", (1060, 240, 90, 114)),
    ("opencv_game_feature_template.png", "crop", "opencv_game_feature_source.png", (837, 102, 156, 156)),
    ("opencv_game_shape_template.png", "crop_alpha", "opencv_game_shape_source.png", (420, 65, 240, 275)),
    ("opencv_hard_scale_template.png", "patch", "opencv_hard_scale_source.png", (90, 450, 96, 96)),
    ("opencv_hard_brightness_template.png", "crop_clean", "opencv_hard_brightness_source.png", (460, 430, 96, 96)),
    ("opencv_hard_repeated_template.png", "crop", "opencv_hard_repeated_source.png", (200, 160, 96, 96)),
    ("opencv_hard_rotation_template.png", "patch_rot90", "opencv_hard_rotation_source.png", (610, 130, 120, 120)),
    ("opencv_hard_occlusion_template.png", "patch", "opencv_hard_occlusion_source.png", (300, 300, 120, 120)),
    ("opencv_hard_similar_shapes_template.png", "crop_alpha", "opencv_hard_similar_shapes_source.png",
     (520, 140, 413, 423)),
]

# 自检用：每个 source 内需要校验的 (模板文件, 期望 rect, 是否要求命中)
SELF_CHECK = [
    ("opencv_real_template.png", "opencv_real_source.jpg", (860, 130, 96, 96)),
    ("opencv_real_city_template.png", "opencv_real_city_source.jpg", (700, 260, 96, 96)),
    ("opencv_real_fruit_template.png", "opencv_real_fruit_source.jpg", (520, 250, 96, 96)),
    ("opencv_real_sign_template.png", "opencv_real_sign_source.jpg", (585, 190, 96, 96)),
    ("opencv_real_space_template.png", "opencv_real_space_source.jpg", (560, 300, 96, 96)),
    ("opencv_real_texture_template.png", "opencv_real_texture_source.jpg", (640, 320, 96, 96)),
    ("opencv_game_scene_template.png", "opencv_game_scene_source.png", (300, 488, 128, 128)),
    ("opencv_game_gem_template.png", "opencv_game_scene_source.png", (455, 325, 70, 70)),
    ("opencv_game_edge_template.png", "opencv_game_scene_source.png", (1060, 240, 90, 114)),
    ("opencv_game_feature_template.png", "opencv_game_feature_source.png", (837, 102, 156, 156)),
    ("opencv_game_shape_template.png", "opencv_game_shape_source.png", (420, 65, 240, 275)),
    ("opencv_real_stop_template.png", "opencv_real_stop_source.jpg", (412, 222, 445, 449)),
    ("opencv_real_shape_template.png", "opencv_real_shape_source.png", (520, 140, 421, 425)),
    ("opencv_hard_brightness_template.png", "opencv_hard_brightness_source.png", (460, 430, 96, 96)),
    ("opencv_hard_repeated_template.png", "opencv_hard_repeated_source.png", (200, 160, 96, 96)),
    ("opencv_hard_similar_shapes_template.png", "opencv_hard_similar_shapes_source.png", (520, 140, 413, 423)),
]


# 期望在 source 中被 0.98 阈值命中的独立位置数（默认 1，重复图案用例 >1）
EXPECT_COUNTS = {
    "opencv_hard_repeated_template.png": 5,
}


# ------------------------------------------------------------------ 图像工具
def read_image(path):
    return Image.open(path).convert("RGB")


def write_image(path, array, fmt=None):
    image = Image.fromarray(array)
    ext = os.path.splitext(path)[1].lower()
    if ext in (".jpg", ".jpeg"):
        image.save(path, quality=98, subsampling=0)
    else:
        image.save(path)


def box_sum(image, height, width):
    integral = np.cumsum(np.cumsum(image, axis=0), axis=1)
    integral = np.pad(integral, ((1, 0), (1, 0)))
    return (
        integral[height:, width:]
        - integral[:-height, width:]
        - integral[height:, :-width]
        + integral[:-height, :-width]
    )


def fft_correlate(source, kernel):
    sh, sw = source.shape
    kh, kw = kernel.shape
    fh = 1
    while fh < sh + kh - 1:
        fh *= 2
    fw = 1
    while fw < sw + kw - 1:
        fw *= 2
    fk = np.fft.rfft2(kernel[::-1, ::-1].astype(np.float64), s=(fh, fw))
    fs = np.fft.rfft2(source.astype(np.float64), s=(fh, fw))
    full = np.fft.irfft2(fs * fk, s=(fh, fw))
    out_h = sh - kh + 1
    out_w = sw - kw + 1
    return full[kh - 1:kh - 1 + out_h, kw - 1:kw - 1 + out_w]


def ccoeff_normed_map(source_gray, template_gray):
    """TM_CCOEFF_NORMED 响应图（valid 区域）。"""
    sh, sw = source_gray.shape
    th, tw = template_gray.shape
    if th > sh or tw > sw:
        return np.zeros((0, 0))
    tpl = template_gray.astype(np.float64) - float(template_gray.mean())
    tpl_norm = float(np.sqrt((tpl * tpl).sum()))
    if tpl_norm <= 1e-9:
        return np.zeros((sh - th + 1, sw - tw + 1))

    count = float(th * tw)
    total = box_sum(source_gray.astype(np.float64), th, tw)
    total_sq = box_sum(source_gray.astype(np.float64) ** 2, th, tw)
    mean = total / count
    variance = np.maximum(total_sq / count - mean * mean, 0.0)
    denom = np.sqrt(variance * count) * tpl_norm
    numer = fft_correlate(source_gray, tpl)
    # 分母为 0（纯色窗口 / 常量模板）时按 OpenCV 语义返回 0，不能当成满分
    response = np.zeros_like(numer)
    valid = denom > 1e-9
    response[valid] = numer[valid] / denom[valid]
    return np.clip(response, -1.0, 1.0)


def gray_of(array):
    return np.asarray(Image.fromarray(array).convert("L"), dtype=np.uint8)


def pick_patches(base_gray, base_rgb):
    """按 PATCH_SPECS 挑互不重叠的高纹理块。"""
    picked = []
    used = []
    height, width = base_gray.shape

    lap_kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float64)

    for label, pw, ph in PATCH_SPECS:
        best = None
        step = max(8, min(pw, ph) // 3)
        for y in range(0, height - ph + 1, step):
            for x in range(0, width - pw + 1, step):
                if any(
                    not (x + pw + 8 <= ux or ux + uw + 8 <= x or y + ph + 8 <= uy or uy + uh + 8 <= y)
                    for ux, uy, uw, uh in used
                ):
                    continue
                window = base_gray[y:y + ph, x:x + pw].astype(np.float64)
                std = float(window.std())
                # 形状类 patch 的内容不重要（形状由 alpha 边给出），只要不是常量即可
                min_std = 2.0 if label in SHAPE_LIKE_PATCHES else 18.0
                if std < min_std:
                    continue
                # 拉普拉斯方差（特征/边缘丰富度），下采样加速
                small = window[::2, ::2]
                lap = (
                    small[:-2, 1:-1]
                    + small[2:, 1:-1]
                    + small[1:-1, :-2]
                    + small[1:-1, 2:]
                    - 4.0 * small[1:-1, 1:-1]
                )
                lap_var = float(lap.var())
                score = std + 0.02 * lap_var
                if best is None or score > best[0]:
                    best = (score, x, y, std, lap_var)
        if best is None:
            raise RuntimeError("找不到合适的 patch: %s (%dx%d)" % (label, pw, ph))
        _, x, y, std, lap_var = best
        picked.append(
            {
                "label": label,
                "x": x,
                "y": y,
                "w": pw,
                "h": ph,
                "std": round(std, 2),
                "lap_var": round(lap_var, 1),
            }
        )
        used.append((x, y, pw, ph))
    return picked


def low_texture_tile(base_rgb, block=320, lo=1.5, hi=10.0):
    """从截图中挑一块"低纹理但非纯色"的区域做画布底。

    纯色底会让 CCOEFF_NORMED 分母为 0，落在 OpenCV 的边界分支上；带一点点
    纹理的底更接近真实截图，也不会制造虚假满分。
    """
    gray = gray_of(base_rgb)
    height, width = gray.shape
    best = None
    step = 24
    for y in range(0, height - block + 1, step):
        for x in range(0, width - block + 1, step):
            std = float(gray[y:y + block, x:x + block].std())
            if not (lo <= std <= hi):
                continue
            score = abs(std - 4.0)
            if best is None or score < best[0]:
                best = (score, x, y, std)
    if best is None:
        return np.full((block, block, 3), 200, dtype=np.uint8)
    _, bx, by, std = best
    return base_rgb[by:by + block, bx:bx + block].copy()


def tile_to(tile, target_w, target_h):
    th, tw = tile.shape[:2]
    reps_y = (target_h + th - 1) // th
    reps_x = (target_w + tw - 1) // tw
    tiled = np.tile(tile, (reps_y, reps_x, 1))[:target_h, :target_w]
    return np.ascontiguousarray(tiled)


def brighten_gray(patch, lo=185.0, hi=255.0):
    """把 patch 压到高亮灰阶。

    ShapeMatchTemplate 走 Otsu + RETR_EXTERNAL 取最大轮廓，任意纹理块会被二值化
    成碎块。压到高亮灰阶后配暗底，Otsu 阈值落在两者之间，整块成为一个实心轮廓。
    """
    gray = gray_of(patch).astype(np.float64)
    low, high = float(gray.min()), float(gray.max())
    if high - low < 1e-6:
        mapped = np.full_like(gray, hi)
    else:
        mapped = lo + (gray - low) / (high - low) * (hi - lo)
    out = np.dstack([mapped, mapped, mapped])
    return np.ascontiguousarray(np.clip(out, 0, 255).astype(np.uint8))


def add_alpha_border(tile, border=4):
    """给模板加一圈透明边。

    ShapeMatchTemplate -> toShapeMask -> alphaToMask 要求存在 alpha==0 的像素才会
    走 alpha 路径；一圈透明边能让形状轮廓精确等于内部实心区，避开 Otsu 把纹理
    块切成碎块的问题。
    """
    height, width = tile.shape[:2]
    out = np.zeros((height + 2 * border, width + 2 * border, 4), dtype=np.uint8)
    out[border:border + height, border:border + width, :3] = tile
    out[border:border + height, border:border + width, 3] = 255
    return out


def paste(array, patch, x, y, scale):
    target_w = max(1, int(round(patch.shape[1] * scale)))
    target_h = max(1, int(round(patch.shape[0] * scale)))
    resized = np.asarray(
        Image.fromarray(patch).resize((target_w, target_h), Image.LANCZOS)
    )
    canvas_h, canvas_w = array.shape[:2]
    x = max(0, min(x, canvas_w - target_w))
    y = max(0, min(y, canvas_h - target_h))
    array[y:y + target_h, x:x + target_w] = resized
    return array


# ------------------------------------------------------------------ 主流程
def capture(out_path):
    cmd = [sys.executable, CAPTURE_PY, out_path]
    print("[capture]", " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recapture", action="store_true", help="重新前台截屏")
    parser.add_argument("--capture", default=DEFAULT_CAPTURE, help="截屏 BMP 路径")
    parser.add_argument("--keep-background", action="store_true",
                        help="画布背景用截图真实内容（默认用纯色低纹理底）")
    parser.add_argument("--reverse", choices=["none", "scale", "occlusion"], default="none",
                        help="反向验证：故意做错，用于确认对应用例会 FAIL")
    args = parser.parse_args()

    if args.reverse == "scale":
        # 1.30x 不在自动档位表里 -> scale_auto_large 必须失败
        print("[reverse] hard_scale 第三处改用 1.30x（非档位）")
        SOURCES["opencv_hard_scale_source.png"] = [
            ("scale_hard", 90, 450, 0.75),
            ("scale_hard", 940, 390, 1.35),
            ("scale_hard", 1560, 760, 1.30),
        ]
    elif args.reverse == "occlusion":
        # 不遮挡 -> 完整模板必然命中 -> EXPECT_FALSE 必须失败
        print("[reverse] 取消 occlusion 遮挡")
        OCCLUSIONS.clear()

    if args.recapture or not os.path.exists(args.capture):
        capture(args.capture)

    base_rgb = np.asarray(read_image(args.capture))
    base_gray = gray_of(base_rgb)
    print("[base] %dx%d" % (base_rgb.shape[1], base_rgb.shape[0]))

    patches = pick_patches(base_gray, base_rgb)
    print("[patch] 选出 %d 块" % len(patches))
    patch_arrays = {}
    for item in patches:
        patch_arrays[item["label"]] = base_rgb[
            item["y"]:item["y"] + item["h"], item["x"]:item["x"] + item["w"]
        ].copy()
    for label in SHAPE_LIKE_PATCHES:
        if label in patch_arrays:
            patch_arrays[label] = brighten_gray(patch_arrays[label])
            print("[patch] %s -> 高亮灰阶（形状用例）" % label)

    rng = np.random.default_rng(20260928)

    os.makedirs(ASSETS, exist_ok=True)

    if args.keep_background:
        background = tile_to(base_rgb, CANVAS[0], CANVAS[1])
        print("[base] background = 截图真实内容平铺")
    else:
        tile = low_texture_tile(base_rgb)
        print("[base] low-texture tile %dx%d std=%.2f"
              % (tile.shape[1], tile.shape[0], float(gray_of(tile).std())))
        background = tile_to(tile, CANVAS[0], CANVAS[1])

    # 轻微噪声：避免大面积常量窗口把 CCOEFF_NORMED 推到分母为 0 的退化分支
    noise = rng.integers(-3, 4, size=background.shape).astype(np.int16)
    background = np.clip(background.astype(np.int16) + noise, 0, 255).astype(np.uint8)

    clean_images = {}
    for name, items in SOURCES.items():
        canvas = background.copy()
        if name in FLAT_BACKGROUND:
            flat = np.full(canvas.shape, FLAT_BACKGROUND[name], dtype=np.int16)
            flat += rng.integers(-3, 4, size=canvas.shape)
            canvas = np.clip(flat, 0, 255).astype(np.uint8)
        for label, x, y, scale in items:
            canvas = paste(canvas, patch_arrays[label], x, y, scale)
        for ox, oy, ow, oh, fill in OCCLUSIONS.get(name, []):
            canvas[oy:oy + oh, ox:ox + ow] = fill
        clean_images[name] = canvas.copy()
        if name in BRIGHTNESS_DELTA:
            delta = BRIGHTNESS_DELTA[name]
            canvas = np.clip(canvas.astype(np.int16) + delta, 0, 255).astype(np.uint8)
        path = os.path.join(ASSETS, name)
        write_image(path, canvas)
        print("[source] %-42s %dx%d" % (name, canvas.shape[1], canvas.shape[0]))

    # 模板：先回读 source 再裁，保证与落盘像素一致
    loaded = {}
    for name, kind, source_name, rect in TEMPLATES:
        x, y, w, h = rect
        if kind == "crop":
            if source_name not in loaded:
                loaded[source_name] = np.asarray(read_image(os.path.join(ASSETS, source_name)))
            tile = loaded[source_name][y:y + h, x:x + w]
        elif kind == "crop_clean":
            tile = clean_images[source_name][y:y + h, x:x + w]
        elif kind == "crop_alpha":
            if source_name not in loaded:
                loaded[source_name] = np.asarray(read_image(os.path.join(ASSETS, source_name)))
            tile = add_alpha_border(loaded[source_name][y:y + h, x:x + w])
        elif kind == "patch":
            flat = [p for p in SOURCES[source_name] if p[1] == x and p[2] == y]
            label = flat[0][0] if flat else None
            if label is None:
                raise RuntimeError("找不到 %s 在 (%d,%d) 的 patch 声明" % (source_name, x, y))
            tile = patch_arrays[label]
        elif kind == "patch_rot90":
            flat = [p for p in SOURCES[source_name] if p[1] == x and p[2] == y]
            label = flat[0][0] if flat else None
            tile = np.rot90(patch_arrays[label], 1)  # 逆时针 90 度
        else:
            raise RuntimeError("未知模板类型 " + kind)
        write_image(os.path.join(ASSETS, name), np.ascontiguousarray(tile))
        print("[template] %-42s %dx%d" % (name, tile.shape[1], tile.shape[0]))

    # ---------------------------------------------------------- 唯一性自检
    report = []
    for template_name, source_name, rect in SELF_CHECK:
        source_gray = gray_of(np.asarray(read_image(os.path.join(ASSETS, source_name))))
        template_image = Image.open(os.path.join(ASSETS, template_name))
        if template_image.mode in ("RGBA", "LA"):
            alpha = np.asarray(template_image.split()[-1])
            rows, cols = np.nonzero(alpha > 0)
            if len(cols):
                template_image = template_image.crop(
                    (int(cols.min()), int(rows.min()), int(cols.max()) + 1, int(rows.max()) + 1)
                )
        template_gray = gray_of(np.asarray(template_image.convert("RGB")))
        response = ccoeff_normed_map(source_gray, template_gray)
        if response.size == 0:
            report.append({"template": template_name, "source": source_name, "ok": False,
                           "reason": "模板大于源图"})
            continue

        # 非极大抑制后收集所有 >=0.98 的独立峰值
        th, tw = template_gray.shape
        radius_y, radius_x = max(1, th // 2), max(1, tw // 2)
        score_map = response.copy()
        peaks = []
        for _ in range(64):
            if score_map.size == 0 or float(score_map.max()) < 0.98:
                break
            index = np.unravel_index(int(np.argmax(score_map)), score_map.shape)
            peaks.append((int(index[1]), int(index[0]), float(score_map[index])))
            y0 = max(0, index[0] - radius_y)
            y1 = min(score_map.shape[0], index[0] + radius_y + 1)
            x0 = max(0, index[1] - radius_x)
            x1 = min(score_map.shape[1], index[1] + radius_x + 1)
            score_map[y0:y1, x0:x1] = -1.0

        expected_count = EXPECT_COUNTS.get(template_name, 1)
        best = (peaks[0][0], peaks[0][1]) if peaks else (-1, -1)
        best_score = peaks[0][2] if peaks else 0.0
        runner_up = peaks[expected_count][2] if len(peaks) > expected_count else -1.0
        ok = (
            best == (rect[0], rect[1])
            and best_score >= 0.98
            and len(peaks) >= expected_count
            and runner_up < 0.90
        )
        report.append(
            {
                "template": template_name,
                "source": source_name,
                "expected": {"x": rect[0], "y": rect[1], "w": rect[2], "h": rect[3]},
                "best": {"x": best[0], "y": best[1], "score": round(best_score, 5)},
                "peaks": [{"x": p[0], "y": p[1], "score": round(p[2], 5)} for p in peaks[:8]],
                "expected_count": expected_count,
                "runner_up": round(runner_up, 5),
                "ok": bool(ok),
            }
        )
        print("[check] %-42s best=(%4d,%4d) score=%.4f peaks=%d runner=%.4f %s"
              % (template_name, best[0], best[1], best_score, len(peaks), runner_up,
                 "OK" if ok else "FAIL"))

    manifest = {
        "capture": args.capture,
        "canvas": {"w": CANVAS[0], "h": CANVAS[1]},
        "patches": patches,
        "sources": {k: [{"label": i[0], "x": i[1], "y": i[2], "scale": i[3]} for i in v]
                    for k, v in SOURCES.items()},
        "self_check": report,
    }
    manifest_path = os.path.join(HERE, "screen_assets.json")
    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)

    failed = [item for item in report if not item["ok"]]
    print("\n[summary] 自检 %d 项，通过 %d，失败 %d" % (len(report), len(report) - len(failed), len(failed)))
    for item in failed:
        print("  FAIL %s: best=%s expected=%s runner=%.4f"
              % (item["template"], item.get("best"), item.get("expected"), item.get("runner_up", -1)))
    print("[manifest]", manifest_path)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
