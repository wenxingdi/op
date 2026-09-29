@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
set "PATH=D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S "D:\AutoPro\op-master\op\build\_deps\opencv\opencv-5.0.0" -B "D:\AutoPro\op-master\op\build\_deps\opencv\build\nmake-x86" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_LIST=core,imgcodecs,flann,imgproc,features,objdetect,stereo,calib -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_apps=OFF -DCPU_BASELINE=SSE3 -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_CUDA=OFF -DWITH_PROTOBUF=OFF -DWITH_FFMPEG=OFF -DWITH_MSMF=OFF -DWITH_DSHOW=OFF -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_WIN32UI=OFF -DCMAKE_INSTALL_PREFIX=D:\AutoPro\op-master\op\build\_deps\opencv\install\nmake-x86
exit /b %ERRORLEVEL%
