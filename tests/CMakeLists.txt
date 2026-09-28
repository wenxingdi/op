cmake_minimum_required(VERSION 3.10)

# 测试目标必须与 libop 用同一档 C++ 标准。
# 起因：hook/SharedFrame.h 本轮由 HookCapture.cpp 抽成 header-only、并被测试直接 include，
# 而它用了 std::span。libop 侧编译时带 /std:c++20（随依赖链传播），本目录的测试目标没有，
# 于是出现 "C2039: span 不是 std 的成员" —— 库编得过去、测试编不过。
# 测试与库不同标准是长期隐患：库能用的设施测试用不了，这类头文件就永远无法进回归。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OP_GTEST_STATIC_TRIPLET "x64-windows-static")
else()
  set(OP_GTEST_STATIC_TRIPLET "x86-windows-static")
endif()

set(OP_GTEST_STATIC_OK FALSE)
set(OP_GTEST_INCLUDE_DIR "")
set(OP_GTEST_LINK_LIBS "")
set(OP_VCPKG_INSTALLED_DIR "")

if(DEFINED VCPKG_INSTALLED_DIR)
  set(OP_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")
elseif(DEFINED _VCPKG_INSTALLED_DIR)
  set(OP_VCPKG_INSTALLED_DIR "${_VCPKG_INSTALLED_DIR}")
elseif(DEFINED ENV{VCPKG_ROOT})
  set(OP_VCPKG_INSTALLED_DIR "$ENV{VCPKG_ROOT}/installed")
elseif(DEFINED CMAKE_TOOLCHAIN_FILE)
  get_filename_component(_op_toolchain_dir "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
  get_filename_component(_op_vcpkg_scripts_dir "${_op_toolchain_dir}" DIRECTORY)
  get_filename_component(_op_vcpkg_root "${_op_vcpkg_scripts_dir}" DIRECTORY)
  set(OP_VCPKG_INSTALLED_DIR "${_op_vcpkg_root}/installed")
endif()

if(OP_VCPKG_INSTALLED_DIR)
  file(TO_CMAKE_PATH "${OP_VCPKG_INSTALLED_DIR}" OP_VCPKG_INSTALLED_DIR)
  set(OP_GTEST_STATIC_ROOT "${OP_VCPKG_INSTALLED_DIR}/${OP_GTEST_STATIC_TRIPLET}")
  set(OP_GTEST_STATIC_INCLUDE "${OP_GTEST_STATIC_ROOT}/include")
  set(OP_GTEST_STATIC_LIB_RELEASE "${OP_GTEST_STATIC_ROOT}/lib/gtest.lib")

  if(EXISTS "${OP_GTEST_STATIC_INCLUDE}/gtest/gtest.h"
     AND EXISTS "${OP_GTEST_STATIC_LIB_RELEASE}")
    set(OP_GTEST_INCLUDE_DIR "${OP_GTEST_STATIC_INCLUDE}")
    set(OP_GTEST_LINK_LIBS "${OP_GTEST_STATIC_LIB_RELEASE}")
    set(OP_GTEST_STATIC_OK TRUE)
    message(STATUS "Using static GoogleTest: ${OP_GTEST_STATIC_LIB_RELEASE}")
  endif()
endif()

if(NOT OP_GTEST_STATIC_OK)
  find_package(GTest CONFIG REQUIRED)
  set(OP_GTEST_LINK_LIBS GTest::gtest)
endif()

set(OP_HAS_OPENCV_SOURCES FALSE)
set(OP_OPENCV_INCLUDE_DIR "")
set(OP_OPENCV_LIBS "")
if(NOT DEFINED OPENCV_LIB_SUFFIX)
  set(OPENCV_LIB_SUFFIX "500")
endif()

function(_op_find_static_lib _op_out_var _op_lib_dir _op_lib_name)
  set(_op_candidates
    "${_op_lib_dir}/${_op_lib_name}.lib"
    "${_op_lib_dir}/lib${_op_lib_name}.a"
    "${_op_lib_dir}/${_op_lib_name}.a"
  )
  foreach(_op_candidate IN LISTS _op_candidates)
    if(EXISTS "${_op_candidate}")
      set(${_op_out_var} "${_op_candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${_op_out_var} "" PARENT_SCOPE)
endfunction()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libop/opencv/TemplateMatcher.h"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libop/opencv/TemplateMatcher.cpp")
  set(_op_opencv_root "")
  if(DEFINED OPENCV_ROOT)
    set(_op_opencv_root "${OPENCV_ROOT}")
  elseif(DEFINED ENV{OPENCV_ROOT})
    set(_op_opencv_root "$ENV{OPENCV_ROOT}")
  endif()

  if(NOT _op_opencv_root STREQUAL "")
    file(TO_CMAKE_PATH "${_op_opencv_root}" _op_opencv_root)
    set(OP_OPENCV_INCLUDE_DIR "${_op_opencv_root}/include")
    # 静态库目录随工具集版本命名（vc17/vc18），存在哪个用哪个。
    set(_op_opencv_toolset_subdirs "vc18" "vc17")
    set(_op_opencv_lib_dir "")
    foreach(_op_toolset IN LISTS _op_opencv_toolset_subdirs)
      if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_op_candidate_dir "${_op_opencv_root}/x64/${_op_toolset}/staticlib")
      else()
        set(_op_candidate_dir "${_op_opencv_root}/x86/${_op_toolset}/staticlib")
      endif()
      if(IS_DIRECTORY "${_op_candidate_dir}")
        set(_op_opencv_lib_dir "${_op_candidate_dir}")
        break()
      endif()
    endforeach()

    set(_op_required_lib_names
      "opencv_calib${OPENCV_LIB_SUFFIX}"
      "opencv_objdetect${OPENCV_LIB_SUFFIX}"
      "opencv_stereo${OPENCV_LIB_SUFFIX}"
      "opencv_features${OPENCV_LIB_SUFFIX}"
      "opencv_imgcodecs${OPENCV_LIB_SUFFIX}"
      "opencv_imgproc${OPENCV_LIB_SUFFIX}"
      "opencv_geometry${OPENCV_LIB_SUFFIX}"
      "opencv_flann${OPENCV_LIB_SUFFIX}"
      "opencv_core${OPENCV_LIB_SUFFIX}"
      "libclapack"
      "libjpeg-turbo"
      "libpng"
      "zlib"
    )

    set(_op_expected_libs "")
    set(_op_all_libs_exist TRUE)
    foreach(_op_lib_name IN LISTS _op_required_lib_names)
      _op_find_static_lib(_op_lib_path "${_op_opencv_lib_dir}" "${_op_lib_name}")
      if(_op_lib_path STREQUAL "")
        set(_op_all_libs_exist FALSE)
      else()
        list(APPEND _op_expected_libs "${_op_lib_path}")
      endif()
    endforeach()

    if(EXISTS "${OP_OPENCV_INCLUDE_DIR}/opencv2/core.hpp" AND _op_all_libs_exist)
      set(OP_HAS_OPENCV_SOURCES TRUE)
      set(OP_OPENCV_LIBS ${_op_expected_libs})
      message(STATUS "OpenCV tests enabled via manual static libraries: ${_op_opencv_lib_dir}")
    else()
      message(WARNING "OpenCV source adapter exists but required OpenCV headers or libraries were not found. Skipping OpenCV tests.")
    endif()
  else()
    message(WARNING "OpenCV source adapter exists but OPENCV_ROOT is not set. Skipping OpenCV tests.")
  endif()
endif()

set(OP_TEST_SOURCES
  "main.cpp"
  "test_support.cpp"
  "handle_compat_test.cpp"
  "op_algorithm_windows_test.cpp"
  "mouse_keyboard_test.cpp"
  "image_color_test.cpp"
  "capture_mode_test.cpp"
  "wgc_test.cpp"
  "ocr_test.cpp"
  "yolo_test.cpp"
  "utils_test.cpp"
  "memory_search_test.cpp"
  "memory_readwrite_test.cpp"
  "window_state_test.cpp"
  # inject_dll 失败上报（2026-09-28 真机暴露：DllInjector 的 -1~-7 被透传，
  # 而对外约定 0=失败非 0=成功 ⇒ 永远无法上报失败）。本测试只注入本进程已加载模块，无需外部进程。
  "process_inject_test.cpp"
  "hook_capture_test.cpp"
  # H14/H20：注入侧导出护栏与"每会话一次"诊断开关都是纯函数（ExportGuard.h / HookDiagnostics.h
  # 均为 header-only），直接编入即可，无需链 op_com 的导出。
  "hook_export_guard_test.cpp"
  # 共享帧读/写链路（SharedFrame.h 为 header-only 纯函数）。这些实现原先住在
  # HookCapture.cpp 的匿名命名空间里，而该文件依赖 blackbone 编不进测试进程，
  # 于是「撕裂帧判据 + 共享内存容量判据」长期零覆盖 —— 本轮搬到 SharedFrame.h 后可测。
  "shared_frame_test.cpp"
  # 32 位 hook 导出名解析（HookExportName.h）。32 位 dll 的 __stdcall 导出名带 `_Name@N`
  # 修饰，而 blackbone 的 GetExport 只做精确名匹配 -> 32 位目标上所有依赖注入的显示模式
  # 绑定失败（实测蜀门 client.exe）。纯函数 + 真实 PE 产物双层判据。
  "hook_export_name_test.cpp"
  "algorithm_test.cpp"
  "../libop/base/Utils.cpp"
  "../libop/base/Environment.cpp"
  "../libop/window/WindowLayout.cpp"
  "../libop/input/mouse/CursorShape.cpp"
  # 交换链格式白名单是纯函数（GetImageBufferFormat / CopyImageData），但 op_com 是 SHARED
  # 且只导出 op.def 里的符号，测试链接不到 -> 与 OpenCv 测试同一做法：直接编入本目标。
  "../libop/hook/DxCaptureCommon.cpp"
  "${OP_COM_I_C}"
)
set_source_files_properties("${OP_COM_I_C}" PROPERTIES GENERATED TRUE)

if(OP_HAS_OPENCV_SOURCES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/opencv_test.cpp")
  list(APPEND OP_TEST_SOURCES
    "opencv_test.cpp"
    "../libop/opencv/OpenCvHelpers.cpp"
    "../libop/opencv/TemplateMatcher.cpp"
  )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/keypad/edit_control_keypad_test.cpp")
  list(APPEND OP_TEST_SOURCES "keypad/edit_control_keypad_test.cpp")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/keypad/keypad_message_test.cpp")
  list(APPEND OP_TEST_SOURCES "keypad/keypad_message_test.cpp")
endif()

add_executable(op_test)
target_sources(op_test PRIVATE ${OP_TEST_SOURCES})
target_compile_definitions(op_test PRIVATE _UNICODE UNICODE _CMAKE_BUILD)
if(DEFINED OP_ARCH_DEFINITION AND NOT OP_ARCH_DEFINITION STREQUAL "")
  target_compile_definitions(op_test PRIVATE "${OP_ARCH_DEFINITION}")
endif()
if(enable_wgc)
  target_compile_definitions(op_test PRIVATE OP_ENABLE_WGC)
endif()

target_include_directories(op_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/libop"
  "${PROJECT_SOURCE_DIR}/libop/runtime"
  "${PROJECT_SOURCE_DIR}/libop/window"
  "${PROJECT_SOURCE_DIR}/libop/image"
  "${PROJECT_SOURCE_DIR}/libop/ocr"
  "${PROJECT_SOURCE_DIR}/libop/opencv"
  "${OP_COM_GENERATED_DIR}"
  "${BLACKBONE_INCLUDE_DIR}"
  "${BLACKBONE_INCLUDE_DIR}/3rd_party"
)
if(OP_GTEST_INCLUDE_DIR)
  target_include_directories(op_test PRIVATE "${OP_GTEST_INCLUDE_DIR}")
endif()

target_link_libraries(op_test PRIVATE ${op_com} ${op_c_api} ${OP_GTEST_LINK_LIBS} dinput8.lib dxguid.lib dwmapi.lib shlwapi.lib)

if(OP_HAS_OPENCV_SOURCES)
  target_include_directories(op_test PRIVATE ${OP_OPENCV_INCLUDE_DIR})
  target_link_libraries(op_test PRIVATE ${OP_OPENCV_LIBS})
endif()

# 图形捕获真机载体（独立进程）：真 D3D9 / D3D11 / OpenGL 交换链 + 四象限纯色画面。
# tests/ 里其余窗口载体都是纯 GDI（FillRect），DisplayHook 的 Present / EndScene /
# wglSwapBuffers detour 从未被真正触发过；本目标为 hook_capture_test.cpp 提供可复现的渲染目标。
add_executable(dx_carrier "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/dx_carrier.cpp")
target_compile_definitions(dx_carrier PRIVATE _UNICODE UNICODE)
target_link_libraries(dx_carrier PRIVATE d3d9.lib d3d11.lib dxgi.lib opengl32.lib user32.lib gdi32.lib)
# 与 op_test 生成到同一目录，测试侧运行时按 exe 同目录查找（不做编译期路径传递：
# 窄字符串宏无法构造 std::wstring，反斜杠路径还会被当成转义序列）。
add_dependencies(op_test dx_carrier)

add_test(NAME op_test COMMAND op_test)
set_tests_properties(op_test
  PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:${op_com}>;$<TARGET_FILE_DIR:${op_c_api}>;$ENV{PATH}")

if(CMAKE_CL_64)
  install(TARGETS op_test DESTINATION "${PROJECT_SOURCE_DIR}/bin/x64")
else()
  install(TARGETS op_test DESTINATION "${PROJECT_SOURCE_DIR}/bin/x86")
endif()

set(OP_DEBUG_SOURCES
  "debug_runner/main_test.cpp"
)
if(OP_HAS_OPENCV_SOURCES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debug_runner/main_test.cpp")
  add_executable(op_debug)
  target_sources(op_debug PRIVATE ${OP_DEBUG_SOURCES})
  target_compile_definitions(op_debug PRIVATE _UNICODE UNICODE _CMAKE_BUILD)
  target_include_directories(op_debug PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/libop"
    "${PROJECT_SOURCE_DIR}/libop/runtime"
    "${PROJECT_SOURCE_DIR}/libop/window"
    "${PROJECT_SOURCE_DIR}/libop/image"
    "${PROJECT_SOURCE_DIR}/libop/ocr"
    "${PROJECT_SOURCE_DIR}/libop/opencv"
  )
  target_link_libraries(op_debug PRIVATE ${op_com})

  set_target_properties(op_debug PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:op_debug>"
    VS_DEBUGGER_COMMAND_ARGUMENTS "smoke"
  )

  if(CMAKE_CL_64)
    install(TARGETS op_debug DESTINATION "${PROJECT_SOURCE_DIR}/bin/x64")
  else()
    install(TARGETS op_debug DESTINATION "${PROJECT_SOURCE_DIR}/bin/x86")
  endif()
endif()
