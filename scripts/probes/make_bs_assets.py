# -*- coding: utf-8 -*-
"""从 BlueStacks 截图生成真实素材（source + 多个模板），并自动挑选“全图唯一”的纹理区域。

唯一性判据：模板在整图中 matchTemplate(CCORR_NORMED) 的峰值位置 == 裁剪位置、
峰值 >= 0.995、屏蔽峰值邻域后的次佳 <= 0.90、灰度标准差 >= 12。
"""
import glob
import json
import os
import sys

import cv2
import numpy as np

# cv2.imread/imwrite 在 Windows 下无法处理中文路径，统一走 imdecode/imencode。
def imread_unicode(path, flags=cv2.IMREAD_COLOR):
    data = np.fromfile(path, dtype=np.uint8)
    return cv2.imdecode(data, flags)


def imwrite_unicode(path, image):
    ext = os.path.splitext(path)[1] or ".png"
    ok, buf = cv2.imencode(ext, image)
    if ok:
        buf.tofile(path)
    return ok


WORK = r"D:/AgentWork/WorkBuddy/2026-08-04-11-41-10"
OUT = r"D:/AutoPro/op-master/op/assets"
TILE = 64          # 模板边长
GRID = 8           # 候选位置扫描步长
WANT = 4           # 需要的模板数量


def pick_source():
    cands = sorted(glob.glob(os.path.join(WORK, "bs_final_*.png")))
    if not cands:
        raise SystemExit("no bluestacks screenshot found")
    return cands[0]


def pick_distractor():
    cands = sorted(glob.glob(os.path.join(WORK, "bs_keymouse_test", "dx_0_*.png")))
    return cands[0] if cands else None


def evaluate(img, gray, x, y, w, h):
    """模板放在 (x,y) 时，判断它是否在全图中唯一且匹配自身。"""
    t = img[y:y + h, x:x + w]
    if t.shape[0] != h or t.shape[1] != w:
        return None
    res = cv2.matchTemplate(img, t, cv2.TM_CCORR_NORMED)
    _, peak, _, loc = cv2.minMaxLoc(res)
    # 屏蔽峰值邻域后取次佳，量化“有没有第二个长得像的地方”
    masked = res.copy()
    y0 = max(0, loc[1] - h)
    y1 = min(masked.shape[0], loc[1] + h + 1)
    x0 = max(0, loc[0] - w)
    x1 = min(masked.shape[1], loc[0] + w + 1)
    masked[y0:y1, x0:x1] = -1.0
    second = float(masked.max()) if masked.size else -1.0
    std = float(gray[y:y + h, x:x + w].std())
    ok = (peak >= 0.995) and (second <= 0.90) and (std >= 12.0) and (loc == (x, y))
    return {"x": x, "y": y, "w": w, "h": h, "peak": round(float(peak), 5),
            "second": round(second, 5), "std": round(std, 2), "loc": list(loc), "ok": ok}


def main():
    src = pick_source()
    img = imread_unicode(src, cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit("failed to read %s" % src)
    H, W = img.shape[:2]
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    print("source: %s  %dx%d" % (os.path.basename(src), W, H))

    os.makedirs(OUT, exist_ok=True)
    source_name = "opencv_bluestacks_source.png"
    imwrite_unicode(os.path.join(OUT, source_name), img)

    picked = []
    rejected = 0
    for y in range(0, H - TILE, GRID):
        if len(picked) >= WANT:
            break
        for x in range(0, W - TILE, GRID):
            if len(picked) >= WANT:
                break
            # 与已选区域保持距离，避免模板重叠
            if any(abs(x - p["x"]) < TILE and abs(y - p["y"]) < TILE for p in picked):
                continue
            info = evaluate(img, gray, x, y, TILE, TILE)
            if info is None:
                continue
            if info["ok"]:
                picked.append(info)
            else:
                rejected += 1

    print("candidates rejected: %d, picked: %d" % (rejected, len(picked)))
    if len(picked) < WANT:
        print("!! not enough unique tiles, widen search or change TILE")
        for p in picked:
            print("  partial:", p)

    templates = []
    for i, p in enumerate(picked):
        name = "opencv_bluestacks_t%d_template.png" % i
        tile = img[p["y"]:p["y"] + p["h"], p["x"]:p["x"] + p["w"]]
        imwrite_unicode(os.path.join(OUT, name), tile)
        p["template_file"] = name
        templates.append(p)
        print("  t%d rect=(%d,%d,%d,%d) peak=%.4f second=%.4f std=%.1f -> %s"
              % (i, p["x"], p["y"], p["w"], p["h"], p["peak"], p["second"], p["std"], name))

    # 干扰模板：来自另一张截图，用于验证“不相关模板不得误命中”。
    distractor_file = None
    dsrc = pick_distractor()
    if dsrc:
        dimg = imread_unicode(dsrc, cv2.IMREAD_COLOR)
        if dimg is not None and dimg.shape[0] >= TILE and dimg.shape[1] >= TILE:
            dh, dw = dimg.shape[:2]
            dtile = dimg[(dh - TILE) // 2:(dh - TILE) // 2 + TILE, (dw - TILE) // 2:(dw - TILE) // 2 + TILE]
            distractor_file = "opencv_bluestacks_distractor.png"
            imwrite_unicode(os.path.join(OUT, distractor_file), dtile)
            print("distractor from %s -> %s" % (os.path.basename(dsrc), distractor_file))

    report = {"source": source_name, "source_size": [W, H], "templates": templates,
              "distractor": distractor_file}
    with open(os.path.join(OUT, "bluestacks_assets.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    print("report written")


if __name__ == "__main__":
    main()
