cmake_minimum_required(VERSION 3.10)

set(OP_HAS_OPENCV FALSE)
set(OP_OPENCV_INCLUDE_DIR "")
set(OP_OPENCV_LIB_DIR "")
set(OP_OPENCV_LINK_LIBS "")
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

set(_op_opencv_root "")
if(DEFINED OPENCV_ROOT)
  set(_op_opencv_root "${OPENCV_ROOT}")
elseif(DEFINED ENV{OPENCV_ROOT})
  set(_op_opencv_root "$ENV{OPENCV_ROOT}")
endif()

if(NOT _op_opencv_root STREQUAL "")
  file(TO_CMAKE_PATH "${_op_opencv_root}" _op_opencv_root)
  set(OP_OPENCV_INCLUDE_DIR "${_op_opencv_root}/include")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_op_opencv_arch_dir "x64")
  else()
    set(_op_opencv_arch_dir "x86")
  endif()
  set(_op_opencv_lib_dir_candidates
    "${_op_opencv_root}/${_op_opencv_arch_dir}/vc18/staticlib"
    "${_op_opencv_root}/${_op_opencv_arch_dir}/vc17/staticlib"
    "${_op_opencv_root}/${_op_opencv_arch_dir}/vc16/staticlib"
  )
  foreach(_op_opencv_lib_dir_candidate IN LISTS _op_opencv_lib_dir_candidates)
    if(EXISTS "${_op_opencv_lib_dir_candidate}")
      set(OP_OPENCV_LIB_DIR "${_op_opencv_lib_dir_candidate}")
      break()
    endif()
  endforeach()
  if(OP_OPENCV_LIB_DIR STREQUAL "")
    list(GET _op_opencv_lib_dir_candidates 0 OP_OPENCV_LIB_DIR)
  endif()

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
    _op_find_static_lib(_op_lib_path "${OP_OPENCV_LIB_DIR}" "${_op_lib_name}")
    if(_op_lib_path STREQUAL "")
      set(_op_all_libs_exist FALSE)
      message(WARNING "OpenCV library not found: ${_op_lib_name} (.lib/.a) in ${OP_OPENCV_LIB_DIR}")
    else()
      list(APPEND _op_expected_libs "${_op_lib_path}")
    endif()
  endforeach()

  if(EXISTS "${OP_OPENCV_INCLUDE_DIR}/opencv2/core.hpp" AND _op_all_libs_exist)
    set(OP_HAS_OPENCV TRUE)
    set(OP_OPENCV_LINK_LIBS ${_op_expected_libs})
    message(STATUS "OpenCV enabled via manual static libraries: ${OP_OPENCV_LIB_DIR}")
  else()
    message(WARNING "OPENCV_ROOT is set but required OpenCV headers or libraries were not found. Continuing without OpenCV support.")
  endif()
endif()

set(OP_CAPTURE_SOURCES
  "binding/BindingSession.cpp"
  "capture/FrameInfo.cpp"
  "capture/ICaptureBackend.cpp"
  "capture/sources/MemoryImageSource.cpp"
  "capture/backends/GdiCapture.cpp"
  "capture/backends/DxgiCapture.cpp"
  "capture/backends/HookCapture.cpp"
)
if(enable_wgc)
  list(APPEND OP_CAPTURE_SOURCES "capture/backends/WgcCapture.cpp")
endif()

set(OP_INPUT_SOURCES
  "input/mouse/WinMouse.cpp"
  "input/mouse/DxMouse.cpp"
  "input/mouse/CursorShape.cpp"
  "input/keyboard/KeyboardBackend.cpp"
  "input/keyboard/WinKeyboard.cpp"
  "input/keyboard/DxKeyboard.cpp"
)

set(OP_HOOK_SOURCES
  "hook/ApiResolver.cpp"
  "hook/D3D9Capture.cpp"
  "hook/D3D10Capture.cpp"
  "hook/D3D11Capture.cpp"
  "hook/D3D12Capture.cpp"
  "hook/DisplayHook.cpp"
  "hook/DxCaptureCommon.cpp"
  "hook/InputHook.cpp"
  "hook/InputHookClient.cpp"
  "hook/HookExport.cpp"
  "hook/MinHookRuntime.cpp"
  "hook/OpenGLCapture.cpp"
)

set(OP_KIERO2_SOURCES
  "../3rd_party/kiero2/kiero_intern.cpp"
  "../3rd_party/kiero2/kiero_d3d9.cpp"
  "../3rd_party/kiero2/kiero_d3d10.cpp"
  "../3rd_party/kiero2/kiero_d3d11.cpp"
  "../3rd_party/kiero2/kiero_d3d12.cpp"
  "../3rd_party/kiero2/kiero_opengl.cpp"
)

# 局部启用 C++20: 只覆盖确实使用 C++20 的内部实现,避免影响 public C API、
# COM/IDL 生成代码、三方源码以及尚未改造的捕获路径。
set(OP_LOCAL_CXX20_SOURCES
  "binding/BindingSession.cpp"
  "capture/sources/MemoryImageSource.cpp"
  "capture/backends/GdiCapture.cpp"
  "capture/backends/HookCapture.cpp"
  "hook/D3D10Capture.cpp"
  "hook/D3D11Capture.cpp"
)
if(enable_wgc)
  list(APPEND OP_LOCAL_CXX20_SOURCES "capture/backends/WgcCapture.cpp")
endif()

set(OP_BASE_SOURCES
  "base/AutomationModes.cpp"
  "base/Utils.cpp"
  "base/Environment.cpp"
  "base/WindowsVersion.cpp"
)

set(OP_WINDOW_SOURCES
  "window/DllInjector.cpp"
  "window/WindowProcess.cpp"
  "window/WindowService.cpp"
  "window/WindowState.cpp"
  "window/WindowText.cpp"
  "window/WindowLayout.cpp"
)

set(OP_IMAGE_SOURCES
  "image/ImageSearchAlgorithms.cpp"
  "image/ImageSearchService.cpp"
)

set(OP_NETWORK_SOURCES
  "network/HttpClient.cpp"
)

set(OP_OCR_SOURCES
  "ocr/OcrService.cpp"
  "ocr/OnnxOcrEngine.cpp"
)
# 注意：ocr_models.rc（OCR 模型资源）不放在 libop 静态库里，
# 因为 MSVC 链接器会丢弃静态库中无人引用的 .res 对象，导致 op_c_api_x64.dll
# 缺资源。改为在下方各 DLL 目标（op_com / op_c_api）里直接编译，保证两个 DLL 都内嵌模型。

# 由模板生成 ocr_models.rc，把模型文件绝对路径注入资源（避免仓库迁移后路径失效）。
set(_op_ocr_models_dir "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/ocr_models")
set(OCR_MODEL_DET "${_op_ocr_models_dir}/ch_PP-OCRv4_det_infer.onnx")
set(OCR_MODEL_REC "${_op_ocr_models_dir}/ch_PP-OCRv4_rec_infer.onnx")
set(OCR_MODEL_KEYS "${_op_ocr_models_dir}/ppocr_keys_v1.txt")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/ocr/ocr_models.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ocr_models.rc"
  @ONLY
)
set(_op_ocr_models_rc "${CMAKE_CURRENT_BINARY_DIR}/ocr_models.rc")

# YOLO 模型（方案B：默认不内置，DLL 零增量）。将自训 onnx 放到
# build/_deps/yolo_models/yolo.onnx 即在配置期自动生成 rc 并编入两个 DLL
# 资源段；缺省只编译 OnnxYoloEngine（外挂模型路径方式）。
set(_op_yolo_models_dir "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/yolo_models")
set(YOLO_MODEL "${_op_yolo_models_dir}/yolo.onnx")
set(_op_yolo_models_rc "")
if(EXISTS "${YOLO_MODEL}")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/yolo/yolo_models.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/yolo_models.rc"
    @ONLY
  )
  set(_op_yolo_models_rc "${CMAKE_CURRENT_BINARY_DIR}/yolo_models.rc")
  message(STATUS "YOLO model found, embedding as resource: ${YOLO_MODEL}")
else()
  message(STATUS "YOLO model not present (build/_deps/yolo_models/yolo.onnx): OnnxYoloEngine external-path only")
endif()

set(OP_YOLO_SOURCES
  "yolo/YoloDetector.cpp"
  "yolo/HttpYoloEngine.cpp"
  "yolo/OnnxYoloEngine.cpp"
)

set(OP_MEMORY_SOURCES
  "memory/DiaGuids.cpp"
  "memory/ProcessMemory.cpp"
)

set(OP_IPC_SOURCES
  "ipc/Pipe.cpp"
)

set(OP_CLASS_SOURCES
  "op/OpContext.cpp"
  "op/OpRuntime.cpp"
  "op/OpProcess.cpp"
  "op/OpClipboard.cpp"
  "op/OpWindow.cpp"
  "op/OpInput.cpp"
  "op/OpAlgorithm.cpp"
  "op/OpImage.cpp"
  "op/OpOpenCv.cpp"
  "op/OpOcr.cpp"
  "op/OpYolo.cpp"
  "op/OpMemory.cpp"
)

set(OP_OPENCV_SOURCES "")
if(OP_HAS_OPENCV AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/opencv/TemplateMatcher.cpp")
  set(OP_OPENCV_SOURCES
    "opencv/OpenCvBridge.cpp"
    "opencv/OpenCvHelpers.cpp"
    "opencv/TemplateMatcher.cpp"
  )
endif()

set(OP_LIBOP_SOURCES
  ${OP_CLASS_SOURCES}
  ${OP_CAPTURE_SOURCES}
  ${OP_INPUT_SOURCES}
  ${OP_HOOK_SOURCES}
  ${OP_BASE_SOURCES}
  ${OP_WINDOW_SOURCES}
  ${OP_IMAGE_SOURCES}
  ${OP_NETWORK_SOURCES}
  ${OP_OCR_SOURCES}
  ${OP_YOLO_SOURCES}
  ${OP_MEMORY_SOURCES}
  ${OP_IPC_SOURCES}
  ${OP_OPENCV_SOURCES}
  ${OP_KIERO2_SOURCES}
  "libop.cpp"
)

set_source_files_properties("../3rd_party/kiero2/kiero_d3d9.cpp" PROPERTIES COMPILE_DEFINITIONS "KIERO_IMPL_FIRST_SLOT=0")
set_source_files_properties("../3rd_party/kiero2/kiero_d3d10.cpp" PROPERTIES COMPILE_DEFINITIONS "KIERO_IMPL_FIRST_SLOT=1")
set_source_files_properties("../3rd_party/kiero2/kiero_d3d11.cpp" PROPERTIES COMPILE_DEFINITIONS "KIERO_IMPL_FIRST_SLOT=2")
set_source_files_properties("../3rd_party/kiero2/kiero_d3d12.cpp" PROPERTIES COMPILE_DEFINITIONS "KIERO_IMPL_FIRST_SLOT=3")
set_source_files_properties("../3rd_party/kiero2/kiero_opengl.cpp" PROPERTIES COMPILE_DEFINITIONS "KIERO_IMPL_FIRST_SLOT=4")

set(OP_COM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/com_generated")
set(OP_COM_I_C "${OP_COM_GENERATED_DIR}/op_i.c")
set(OP_COM_I_H "${OP_COM_GENERATED_DIR}/op_i.h")
set(OP_COM_TLB "${OP_COM_GENERATED_DIR}/op.tlb")
set(OP_COM_PROXY_C "${OP_COM_GENERATED_DIR}/op_p.c")
set(OP_COM_DLLDATA_C "${OP_COM_GENERATED_DIR}/dlldata.c")
set(OP_COM_GENERATED_FILES
  "${OP_COM_I_C}"
  "${OP_COM_I_H}"
  "${OP_COM_TLB}"
  "${OP_COM_PROXY_C}"
  "${OP_COM_DLLDATA_C}"
)
set_source_files_properties(${OP_COM_GENERATED_FILES} PROPERTIES GENERATED TRUE)
set(OP_COM_GENERATED_DIR "${OP_COM_GENERATED_DIR}" PARENT_SCOPE)
set(OP_COM_I_C "${OP_COM_I_C}" PARENT_SCOPE)
set(OP_COM_I_H "${OP_COM_I_H}" PARENT_SCOPE)
set(OP_COM_TLB "${OP_COM_TLB}" PARENT_SCOPE)
set(OP_COM_GENERATED_FILES "${OP_COM_GENERATED_FILES}" PARENT_SCOPE)

set(OP_COM_SOURCES
  "com/op.cpp"
  "com/OpAutomation.cpp"
  "com/stdafx.cpp"
  "com/compreg.cpp"
  "com/dllmain.cpp"
  # 延迟加载失败兜底（方案 A1），与 op_c_api 同名同源，各自编入本 DLL。
  "hook/DliFailureHook.cpp"
  "${OP_COM_I_C}"
  "${CMAKE_CURRENT_SOURCE_DIR}/com/op.def"
  "${CMAKE_CURRENT_SOURCE_DIR}/com/op.rc"
  # 内置 OCR 模型资源（与 op_c_api 共用同一份 .rc，各自独立编入 DLL）
  "${_op_ocr_models_rc}"
)
# 内置 YOLO 模型资源（仅当构建期检测到自训模型时非空）
if(_op_yolo_models_rc)
  list(APPEND OP_COM_SOURCES "${_op_yolo_models_rc}")
endif()

set(OP_C_API_SOURCES
  "c_api/op_c_api.cpp"
  "hook/HookExport.cpp"
  # 延迟加载失败兜底（方案 A1）：注入目标进程后 onnxruntime.dll 解析失败时，
  # 改用 op 安装目录绝对路径加载，避免目标进程 0xC0000005 崩溃。
  # 必须与 /DELAYLOAD 的模块在同一目标内，故显式列入（见下方 ONNX 链接段）。
  "hook/DliFailureHook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/c_api/op_c_api.rc"
  # 内置 OCR 模型资源（与 op_com 共用同一份 .rc，各自独立编入 DLL）
  "${_op_ocr_models_rc}"
)
if(_op_yolo_models_rc)
  list(APPEND OP_C_API_SOURCES "${_op_yolo_models_rc}")
endif()

set(OP_COMMON_DEFINITIONS
  WIN32
  _WINDOWS
  _CRT_NON_CONFORMING_SWPRINTFS
  _CRT_SECURE_NO_WARNINGS
  _SILENCE_CXX17_ADAPTOR_TYPEDEFS_DEPRECATION_WARNING
  _UNICODE
  UNICODE
  OP_VERSION="0.4.8.3"
)
if(DEFINED OP_ARCH_DEFINITION AND NOT OP_ARCH_DEFINITION STREQUAL "")
  list(APPEND OP_COMMON_DEFINITIONS "${OP_ARCH_DEFINITION}")
endif()

set(OP_PRIVATE_INCLUDE_DIRS
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
  "${CMAKE_CURRENT_SOURCE_DIR}/window"
  "${CMAKE_CURRENT_SOURCE_DIR}/image"
  "${CMAKE_CURRENT_SOURCE_DIR}/network"
  "${CMAKE_CURRENT_SOURCE_DIR}/ocr"
  "${CMAKE_CURRENT_SOURCE_DIR}/opencv"
  "${CMAKE_CURRENT_SOURCE_DIR}/yolo"
  "${CMAKE_CURRENT_SOURCE_DIR}/memory"
  "${CMAKE_CURRENT_SOURCE_DIR}/ipc"
  "${CMAKE_CURRENT_SOURCE_DIR}/hook"
  "${PROJECT_SOURCE_DIR}/3rd_party/kiero2"
  "${BLACKBONE_INCLUDE_DIR}"
  "${BLACKBONE_INCLUDE_DIR}/3rd_party"
)
if(OP_HAS_OPENCV)
  list(APPEND OP_PRIVATE_INCLUDE_DIRS "${OP_OPENCV_INCLUDE_DIR}")
endif()
list(APPEND OP_PRIVATE_INCLUDE_DIRS "${OP_COM_GENERATED_DIR}")

add_library(libop)
target_sources(libop PRIVATE ${OP_LIBOP_SOURCES})
target_compile_definitions(libop PRIVATE ${OP_COMMON_DEFINITIONS} U_STATIC_IMPLEMENTATION)
target_include_directories(libop
  PUBLIC "${PROJECT_SOURCE_DIR}/include"
  PRIVATE ${OP_PRIVATE_INCLUDE_DIRS}
)

add_custom_command(
  OUTPUT ${OP_COM_GENERATED_FILES}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${OP_COM_GENERATED_DIR}"
  COMMAND midl "${CMAKE_CURRENT_SOURCE_DIR}/com/op.idl"
    /iid "${OP_COM_I_C}" /env win32 /h "${OP_COM_I_H}" /W1 /char signed
    /tlb "${OP_COM_TLB}" /Oicf /target "NT60" /D "NDEBUG" /robust /nologo
    /proxy "${OP_COM_PROXY_C}" /dlldata "${OP_COM_DLLDATA_C}"
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/com/op.idl
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/com/op.rc" PROPERTIES OBJECT_DEPENDS "${OP_COM_TLB}")

add_library(${op_com} SHARED)
target_sources(${op_com} PRIVATE ${OP_LIBOP_SOURCES} ${OP_COM_SOURCES})
target_compile_definitions(${op_com} PRIVATE
  ${OP_COMMON_DEFINITIONS}
  _USRDLL
  _WINDLL
  OP_EXPORTS
)
target_include_directories(${op_com}
  PRIVATE ${OP_PRIVATE_INCLUDE_DIRS}
)
set_target_properties(${op_com} PROPERTIES LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/com/op.def\" /NODEFAULTLIB:\"mfc110d\"")

add_library(${op_c_api} SHARED)
target_sources(${op_c_api} PRIVATE ${OP_C_API_SOURCES})
target_compile_definitions(${op_c_api} PRIVATE
  ${OP_COMMON_DEFINITIONS}
  U_STATIC_IMPLEMENTATION
  OP_C_API_EXPORTS
)
target_include_directories(${op_c_api}
  PUBLIC "${PROJECT_SOURCE_DIR}/include"
  PRIVATE ${OP_PRIVATE_INCLUDE_DIRS}
)

if(enable_wgc)
  target_compile_definitions(libop PRIVATE OP_ENABLE_WGC)
  target_compile_definitions(${op_com} PRIVATE OP_ENABLE_WGC)
  target_compile_definitions(${op_c_api} PRIVATE OP_ENABLE_WGC)
endif()

if(MSVC)
  set_source_files_properties(${OP_LOCAL_CXX20_SOURCES}
    PROPERTIES
      COMPILE_OPTIONS "/std:c++20;/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS"
  )
endif()

target_link_libraries(${op_com} PRIVATE "${BLACKBONE_LIBRARY}")
target_link_libraries(${op_c_api} PRIVATE libop "${BLACKBONE_LIBRARY}")

if(enable_wgc)
  target_link_libraries(libop PRIVATE windowsapp runtimeobject.lib)
  target_link_libraries(${op_com} PRIVATE windowsapp runtimeobject.lib)
  target_link_libraries(${op_c_api} PRIVATE windowsapp runtimeobject.lib)
endif()

# MinHook 通过 vcpkg 提供的 imported target 链接,头文件与库均由 minhook::minhook
# 的 INTERFACE_INCLUDE_DIRECTORIES / IMPORTED_LOCATION 提供。链到所有编译
# MinHook 源码(kiero.cpp / InputHook.cpp)的 target,确保 include 目录可达。
find_package(minhook CONFIG REQUIRED)
find_package(directx-headers CONFIG REQUIRED)
target_link_libraries(libop       PRIVATE minhook::minhook)
target_link_libraries(${op_com}   PRIVATE minhook::minhook)
target_link_libraries(${op_c_api} PRIVATE minhook::minhook)
target_link_libraries(libop       PRIVATE Microsoft::DirectX-Headers)
target_link_libraries(${op_com}   PRIVATE Microsoft::DirectX-Headers)
target_link_libraries(${op_c_api} PRIVATE Microsoft::DirectX-Headers)

# ---- 内置 ONNX Runtime OCR 引擎（模型编入 DLL 资源段）----
# 链接策略（用户决策：2026-08-28 选「共享」）：
#   * 优先静态 onnxruntime_static.lib  -> 0 新增 DLL（需源码构建或旧版 v1.14.1 主包内含）
#   * 否则共享 onnxruntime.lib(导入库) -> +2 DLL(onnxruntime.dll + onnxruntime_providers_shared.dll)，随插件分发
# 注：Microsoft 不为 Windows x64 发布独立静态包，≥1.16 主包亦不含静态库，故默认走共享。
set(_op_onnxrt_root "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/onnxruntime")
file(GLOB _op_onnxrt_dirs "${_op_onnxrt_root}/*")
foreach(_d ${_op_onnxrt_dirs})
  if(IS_DIRECTORY "${_d}/include" AND IS_DIRECTORY "${_d}/lib")
    set(ONNXRUNTIME_DIR "${_d}")
  endif()
endforeach()
if(DEFINED ONNXRUNTIME_DIR AND ONNXRUNTIME_DIR)
  message(STATUS "ONNX Runtime found: ${ONNXRUNTIME_DIR}")
  foreach(_t libop ${op_com} ${op_c_api})
    target_include_directories(${_t} PRIVATE "${ONNXRUNTIME_DIR}/include")
  endforeach()
  set(_op_onnxrt_static "${ONNXRUNTIME_DIR}/lib/onnxruntime_static.lib")
  set(_op_onnxrt_shared  "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
  set(_op_onnxrt_prov   "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.lib")
  if(EXISTS "${_op_onnxrt_static}")
    # 静态链接：0 新增 DLL（需要 onnxruntime 静态库，预编译通常不含）
    foreach(_t libop ${op_com} ${op_c_api})
      target_link_libraries(${_t} PRIVATE "${_op_onnxrt_static}"
        advapi32 userenv ole32 shell32 user32 gdi32 crypt32 rpcrt4 ncrypt ws2_32)
    endforeach()
    message(STATUS "ONNX Runtime: STATIC linking (0 extra DLL)")
  elseif(EXISTS "${_op_onnxrt_shared}")
    # 共享链接：+2 DLL（onnxruntime.dll + onnxruntime_providers_shared.dll），随插件分发
    foreach(_t libop ${op_com} ${op_c_api})
      target_link_libraries(${_t} PRIVATE "${_op_onnxrt_shared}")
      if(EXISTS "${_op_onnxrt_prov}")
        target_link_libraries(${_t} PRIVATE "${_op_onnxrt_prov}")
      endif()
    endforeach()
    # ── 延迟加载 onnxruntime.dll（2026-09-16，修复注入依赖污染）──────────
    # 背景：op_x64.dll / op_c_api_x64.dll 会被注入目标进程（DX 输入通道 hook）。
    # 静态导入 onnxruntime.dll 使「目标进程能否解析该依赖」成为注入成功的前置条件。
    # 实测 BlueStacks 5（已加载 dinput8.dll）返回 0xC0000135 STATUS_DLL_NOT_FOUND，
    # DX 三通道 10/10 绑定失败；A/B 对照实验证明 onnxruntime.dll 可解析与否是唯一变量。
    # 改为延迟加载后 onnxruntime.dll 从导入表消失：不调用 OCR 的进程（注入后的游戏）
    # 永不需要它；宿主进程调用 OCR 时仍按原搜索路径解析，较静态导入不劣化
    # （静态导入同样要求该 DLL 可解析，否则宿主侧连插件都无法加载）。
    # 注：onnxruntime_providers_shared.dll 不出现在导入表（由 ORT 运行时自行加载），
    #     对其加 /DELAYLOAD 会触发 LNK4199，故只延迟 onnxruntime.dll。
    foreach(_t ${op_com} ${op_c_api})
      set_property(TARGET ${_t} APPEND_STRING PROPERTY LINK_FLAGS " /DELAYLOAD:onnxruntime.dll")
      target_link_libraries(${_t} PRIVATE delayimp.lib)
    endforeach()
    message(STATUS "ONNX Runtime: delay-loading onnxruntime.dll (safe for injection)")
    # 构建期把运行时 DLL 拷到插件输出目录旁，保证 OcrEx 运行期可加载
    foreach(_t ${op_com} ${op_c_api})
      add_custom_command(TARGET ${_t} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
                "$<TARGET_FILE_DIR:${_t}>/onnxruntime.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
                "$<TARGET_FILE_DIR:${_t}>/onnxruntime_providers_shared.dll"
        COMMENT "Copy onnxruntime DLLs next to ${_t}")
    endforeach()
    # 安装期把运行时 DLL 一并装入 bin/<arch>（与插件同目录，供运行期加载）
    install(FILES "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll"
                   "${ONNXRUNTIME_DIR}/lib/onnxruntime_providers_shared.dll"
            DESTINATION "${PROJECT_SOURCE_DIR}/bin/${_op_install_arch}")
    message(STATUS "ONNX Runtime: SHARED linking (+2 DLL: onnxruntime.dll, onnxruntime_providers_shared.dll)")
  else()
    message(WARNING "Neither onnxruntime_static.lib nor onnxruntime.lib found under ${ONNXRUNTIME_DIR}/lib; built-in OCR disabled")
  endif()
else()
  message(WARNING "ONNX Runtime not found under ${_op_onnxrt_root}; built-in OCR disabled")
endif()

target_link_libraries(libop PRIVATE dwmapi.lib)
target_link_libraries(${op_com} PRIVATE dwmapi.lib)
target_link_libraries(${op_c_api} PRIVATE dwmapi.lib)

if(OP_HAS_OPENCV)
  target_link_libraries(libop PRIVATE ${OP_OPENCV_LINK_LIBS})
  target_link_libraries(${op_com} PRIVATE ${OP_OPENCV_LINK_LIBS})
  target_link_libraries(${op_c_api} PRIVATE ${OP_OPENCV_LINK_LIBS})
endif()

set(OP_HAS_OPENCV "${OP_HAS_OPENCV}" PARENT_SCOPE)

install(FILES "${PROJECT_SOURCE_DIR}/include/libop.h" DESTINATION "${PROJECT_SOURCE_DIR}/include")
install(FILES "${PROJECT_SOURCE_DIR}/include/op_c_api.h" DESTINATION "${PROJECT_SOURCE_DIR}/include")
if(CMAKE_CL_64)
  set(_op_install_arch "x64")
else()
  set(_op_install_arch "x86")
endif()

install(TARGETS ${op_com}
  RUNTIME DESTINATION "${PROJECT_SOURCE_DIR}/bin/${_op_install_arch}"
)
install(TARGETS ${op_c_api}
  RUNTIME DESTINATION "${PROJECT_SOURCE_DIR}/bin/${_op_install_arch}"
  ARCHIVE DESTINATION "${PROJECT_SOURCE_DIR}/lib/${_op_install_arch}"
)

# MinHook 若以 DLL 形式链接(x64-windows triplet)，CMake install(TARGETS) 不会
# 自动复制第三方 imported target 的 RUNTIME，需手动把 DLL 安装到 bin 目录。
# 若以静态库形式链接(x64-windows-static triplet)，IMPORTED_LOCATION 指向 .lib，
# 已编进主 DLL，无需也不应复制到 bin —— 下方仅当路径以 .dll 结尾时才复制。
if(TARGET minhook::minhook)
  get_target_property(_minhook_dll minhook::minhook IMPORTED_LOCATION_RELEASE)
  if(NOT _minhook_dll)
    get_target_property(_minhook_dll minhook::minhook IMPORTED_LOCATION)
  endif()
  if(_minhook_dll AND _minhook_dll MATCHES "\\.dll$" AND EXISTS "${_minhook_dll}")
    install(FILES "${_minhook_dll}" DESTINATION "${PROJECT_SOURCE_DIR}/bin/${_op_install_arch}")
  endif()
endif()
