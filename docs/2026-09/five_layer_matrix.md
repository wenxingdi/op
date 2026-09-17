# 五层覆盖矩阵（主键 = C-API 导出名）

生成方式：`python scripts/five_layer_matrix.py`（自动扫描，勿手改）

当前接口面：c_api 声明 **226** · 契约 libop.h **224** 方法 · op/ 实现 **224**

## 缺口汇总

| 层 | 缺口数 | 说明 |
|---|---|---|
| op/ 实现层未转发 | 2 | 契约有、实现缺 → 真断链 |
| c_api.cpp 无定义 | 0 | 头文件声明但未实现（若>0 通常是宏生成未识别） |
| COM 出口无对应 | 4 | C-API 有、COM 没有 |
| python api.py 未暴露 | 3 | 绑定层缺口 |
| _ffi.py 无 ctypes 签名 | 3 | 调不通 |
| go 未覆盖 | 5 |  |
| 契约 libop.h 无声明 | 2 | 仅 C-API 独有（生命周期/测试钩子/别名） |

## 明细：op/ 实现层未转发

- `OpDestroy` (impl=无)
- `OpRequestCaptureForTest` (impl=无)

## 明细：COM 出口无对应

- `OpDestroy` (impl=无)
- `OpRequestCaptureForTest` (impl=无)
- `OpAutoOcr` (impl=OpOcr.cpp)
- `OpAutoOcrFromFile` (impl=OpOcr.cpp)

## 明细：python api.py 未暴露

- `OpRequestCaptureForTest` (impl=无)
- `OpAutoOcr` (impl=OpOcr.cpp)
- `OpAutoOcrFromFile` (impl=OpOcr.cpp)

## 明细：_ffi.py 无 ctypes 签名

- `OpRequestCaptureForTest` (impl=无)
- `OpAutoOcr` (impl=OpOcr.cpp)
- `OpAutoOcrFromFile` (impl=OpOcr.cpp)

## 明细：go 未覆盖

- `OpSetDxAttr` (impl=OpInput.cpp)
- `OpGetDxAttr` (impl=OpInput.cpp)
- `OpRequestCaptureForTest` (impl=无)
- `OpAutoOcr` (impl=OpOcr.cpp)
- `OpAutoOcrFromFile` (impl=OpOcr.cpp)

## 明细：契约 libop.h 无声明

`OpDestroy`, `OpRequestCaptureForTest`

## 能力层服务被调用频次（经 op/ 门面统计）

| m_context 服务 | 承载 API 数 |
|---|---|
| image_proc | 68 |
| bkproc | 63 |
| window_service | 26 |
| curr_path | 4 |
| screen_data_mode | 3 |
| vkmap | 3 |
| id | 1 |
| screenData | 1 |
| screenDataBmp | 1 |
