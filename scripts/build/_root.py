# -*- coding: utf-8 -*-
"""仓库根定位（构建/探针脚本共用）。

原先这些脚本位于 <repo>/build/ 下，用 dirname(dirname(__file__)) 取仓库根；归档到
<repo>/scripts/build/ 后该假设失效（会指到 scripts/）。改为**向上找 .git**，
于是脚本放在仓库任意层级都成立，后续再挪目录也不会重蹈覆辙。
"""
import os

__all__ = ["repo_root"]


def repo_root(start: str | None = None) -> str:
    """自 start（默认本文件所在目录）向上查找含 .git 的目录。"""
    p = os.path.abspath(start or os.path.dirname(__file__))
    while True:
        if os.path.isdir(os.path.join(p, ".git")):
            return p
        parent = os.path.dirname(p)
        if parent == p:
            raise RuntimeError(f"未找到仓库根：自 {start or __file__} 向上没有 .git")
        p = parent
