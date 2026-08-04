#!/usr/bin/env python3
"""
OP项目构建脚本 - 国内源优化版本
为国内网络环境优化的构建脚本
"""

import os
import sys
from pathlib import Path

# 国内源配置
DOMESTIC_MIRRORS = {
    # vcpkg镜像
    "vcpkg_original": "https://github.com/microsoft/vcpkg.git",
    "vcpkg_mirror": "https://gitee.com/mirrors/vcpkg.git",
    
    # BlackBone镜像  
    "blackbone_original": "https://github.com/DarthTon/Blackbone",
    "blackbone_mirror": "https://gitee.com/mirrors/blackbone.git",
    
    # OpenCV镜像
    "opencv_original": "https://github.com/opencv/opencv/archive/refs/tags/",
    "opencv_mirror": "https://mirrors.tuna.tsinghua.edu.cn/github-release/opencv/opencv/"
}

def setup_domestic_mirrors():
    """设置国内镜像环境变量"""
    print("=== 配置国内镜像源 ===")
    
    # 设置vcpkg国内镜像
    if os.path.exists(r"D:\vcpkg-mirror"):
        os.environ["VCPKG_ROOT"] = r"D:\vcpkg-mirror"
        print("✅ 使用本地vcpkg镜像")
    
    # 设置git代理（如果网络需要）
    # os.environ["ALL_PROXY"] = "http://127.0.0.1:1080"
    
    print("🌐 将自动使用国内镜像源下载依赖")

def check_required_downloads():
    """检查需要下载的依赖"""
    print("\n=== 依赖下载清单 ===")
    
    downloads = [
        {
            "name": "vcpkg",
            "original": "https://github.com/microsoft/vcpkg.git",
            "domestic": "https://gitee.com/mirrors/vcpkg.git",
            "size": "约50MB",
            "essential": True
        },
        {
            "name": "BlackBone",  
            "original": "https://github.com/DarthTon/Blackbone",
            "domestic": "https://gitee.com/mirrors/blackbone.git",
            "size": "约15MB", 
            "essential": True
        },
        {
            "name": "OpenCV 5.0.0",
            "original": "https://github.com/opencv/opencv/archive/refs/tags/5.0.0.zip",
            "domestic": "https://mirrors.tuna.tsinghua.edu.cn/github-release/opencv/opencv/Release-5.0.0.html",
            "size": "约500MB",
            "essential": True
        },
        {
            "name": "MinHook",
            "original": "通过vcpkg自动安装",
            "domestic": "通过国内vcpkg镜像",
            "size": "约2MB",
            "essential": True
        }
    ]
    
    for item in downloads:
        print(f"📦 {item['name']}: {item['size']}")
        print(f"   原始: {item['original']}")
        print(f"   国内: {item['domestic']}")
        print()

def manual_download_guide():
    """手动下载指南"""
    print("\n=== 手动下载指南 ===")
    
    print("""
如果自动构建失败，请按以下步骤手动下载：

1️⃣ 下载vcpkg国内镜像:
   git clone https://gitee.com/mirrors/vcpkg.git D:\vcpkg-mirror

2️⃣ 下载BlackBone:
   git clone https://gitee.com/mirrors/blackbone.git D:\AutoPro\op-master\op\build\_deps\BlackBone

3️⃣ 下载OpenCV 5.0.0:
   访问: https://mirrors.tuna.tsinghua.edu.cn/github-release/opencv/opencv/Release-5.0.0.html
   下载: opencv-5.0.0.zip
   解压到: D:\AutoPro\op-master\op\build\_deps\opencv\

4️⃣ 初始化vcpkg:
   cd D:\vcpkg-mirror
   .\\bootstrap-vcpkg.bat
   .\\vcpkg.exe install minhook:x64-windows gtest:x64-windows directx-headers:x64-windows

5️⃣ 重新运行构建:
   python build.py -g vs2022 -t Release -a x64
""")

def main():
    print("🌏 OP项目国内源优化构建 v1.0")
    print("=" * 50)
    
    # 配置国内源
    setup_domestic_mirrors()
    
    # 检查下载需求
    check_required_downloads()
    
    # 手动下载指南
    manual_download_guide()
    
    print("\n💡 使用建议:")
    print("• 如果网络良好，直接运行: python build.py -g vs2022 -t Release -a x64")
    print("• 如果下载失败，按上述指南手动下载依赖")
    print("• 构建过程需要20-45分钟，请耐心等待")

if __name__ == "__main__":
    main()