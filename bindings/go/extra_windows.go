//go:build windows

// 代码生成，勿手改：python scripts/gen_go_bindings.py
//
// 补齐 C API 已有、go 绑定此前缺失的接口（five_layer_matrix.py 报 127 个未覆盖）。
// 生成后必须 `go build ./...` 验证 —— 类型不对编译器当场就报。

package opcapi

import (
	"golang.org/x/sys/windows"
)

var (
	procAStarFindPathBM *windows.LazyProc
	procAStarFindPathWay *windows.LazyProc
	procAudioGetPidFromHwnd *windows.LazyProc
	procAudioGetSystemVolume *windows.LazyProc
	procAudioQueryPid *windows.LazyProc
	procAudioQueryWindow *windows.LazyProc
	procAudioSetSessionMute *windows.LazyProc
	procAudioSetSessionVolume *windows.LazyProc
	procAudioSetSystemMute *windows.LazyProc
	procAudioSetSystemVolume *windows.LazyProc
	procAutoOcrFromFile *windows.LazyProc
	procBase64Decode *windows.LazyProc
	procBase64Encode *windows.LazyProc
	procBeep *windows.LazyProc
	procClearPicCache *windows.LazyProc
	procCmpColorFromMem *windows.LazyProc
	procCopyFile *windows.LazyProc
	procCreateFolder *windows.LazyProc
	procCreateQRCode *windows.LazyProc
	procCropImage *windows.LazyProc
	procCvFlip *windows.LazyProc
	procCvMatchTemplateRot *windows.LazyProc
	procCvRotate *windows.LazyProc
	procDecodeQRCode *windows.LazyProc
	procDecodeQRCodeFromFile *windows.LazyProc
	procDeleteFile *windows.LazyProc
	procDeleteFolder *windows.LazyProc
	procDisableMinMax *windows.LazyProc
	procDownCpu *windows.LazyProc
	procDrawClear *windows.LazyProc
	procDrawLine *windows.LazyProc
	procDrawRect *windows.LazyProc
	procDrawSetVisible *windows.LazyProc
	procDrawText *windows.LazyProc
	procEnableDebugPrivilege *windows.LazyProc
	procFileExists *windows.LazyProc
	procFindColorBlockExFromMem *windows.LazyProc
	procFindColorExFromMem *windows.LazyProc
	procFindColorFromMem *windows.LazyProc
	procFindMultiColorExFromMem *windows.LazyProc
	procFindMultiColorFromMem *windows.LazyProc
	procFindNearestFeasiblePoint *windows.LazyProc
	procFindNearestPathPoint *windows.LazyProc
	procFindPicExFromMem *windows.LazyProc
	procFindPicFromMem *windows.LazyProc
	procFindStrBatch *windows.LazyProc
	procFindStrEx2 *windows.LazyProc
	procFindStrMem *windows.LazyProc
	procFreeAllImages *windows.LazyProc
	procFreeImage *windows.LazyProc
	procGaiLu *windows.LazyProc
	procGetColorFromMem *windows.LazyProc
	procGetColorNumFromMem *windows.LazyProc
	procGetCursorImage *windows.LazyProc
	procGetDPI *windows.LazyProc
	procGetDisplayMethod *windows.LazyProc
	procGetFPS *windows.LazyProc
	procGetFileList *windows.LazyProc
	procGetFileName *windows.LazyProc
	procGetFileSize *windows.LazyProc
	procGetImageCount *windows.LazyProc
	procGetImageData *windows.LazyProc
	procGetImageSize *windows.LazyProc
	procGetLastErrorString *windows.LazyProc
	procGetMachineCode *windows.LazyProc
	procGetModuleInfoList *windows.LazyProc
	procGetModuleSize *windows.LazyProc
	procGetRandomDouble *windows.LazyProc
	procGetRandomNumber *windows.LazyProc
	procGetScreenDepth *windows.LazyProc
	procGetScreenHeight *windows.LazyProc
	procGetScreenWidth *windows.LazyProc
	procGetTime *windows.LazyProc
	procGetWindowScale *windows.LazyProc
	procHttpDownloadFile *windows.LazyProc
	procHttpGet *windows.LazyProc
	procHttpPost *windows.LazyProc
	procIsDisplayDead *windows.LazyProc
	procIsElevated *windows.LazyProc
	procIsLineBlocked *windows.LazyProc
	procJsonAppend *windows.LazyProc
	procJsonDelete *windows.LazyProc
	procJsonGetBool *windows.LazyProc
	procJsonGetKeys *windows.LazyProc
	procJsonGetNumber *windows.LazyProc
	procJsonGetSize *windows.LazyProc
	procJsonGetString *windows.LazyProc
	procJsonIsValid *windows.LazyProc
	procJsonSetBool *windows.LazyProc
	procJsonSetNumber *windows.LazyProc
	procJsonSetString *windows.LazyProc
	procKeyPressHotkey *windows.LazyProc
	procLoadImageBmp *windows.LazyProc
	procLoadImageMem *windows.LazyProc
	procLockWindowPosition *windows.LazyProc
	procLockWindowSize *windows.LazyProc
	procMd5 *windows.LazyProc
	procMoveFile *windows.LazyProc
	procOcrAutoColor *windows.LazyProc
	procOcrAutoColorEx2 *windows.LazyProc
	procOcrAutoColorFromFile *windows.LazyProc
	procOcrAutoColorFromMemEx2 *windows.LazyProc
	procOcrEx2 *windows.LazyProc
	procOcrFromMemEx2 *windows.LazyProc
	procOcrLastProfile *windows.LazyProc
	procOcrWarmup *windows.LazyProc
	procPointInPolygon *windows.LazyProc
	procReadFileString *windows.LazyProc
	procSetAStarMap *windows.LazyProc
	procSetAStarMapData *windows.LazyProc
	procSetIme *windows.LazyProc
	procSetPicCacheMax *windows.LazyProc
	procSha1 *windows.LazyProc
	procSimplifyPath *windows.LazyProc
	procSmoothPathByLOS *windows.LazyProc
	procSortPosDistance *windows.LazyProc
	procTerminateProcess *windows.LazyProc
	procTerminateProcessTree *windows.LazyProc
	procToAbsolutePath *windows.LazyProc
	procVirtualAllocEx *windows.LazyProc
	procVirtualProtectEx *windows.LazyProc
	procWriteStringToFile *windows.LazyProc
	procYoloDetectEx2 *windows.LazyProc
	procYoloDetectFromFileEx2 *windows.LazyProc
	procYoloDetectFromMem *windows.LazyProc
	procYoloDetectFromMemEx2 *windows.LazyProc
	// 以下两个此前只在 bindProcs 里赋值、var 块漏了声明，导致整包编不过（2026-11-21 修）。
	procFindColorBlockExS *windows.LazyProc
	procFindLineExS        *windows.LazyProc
)

func bindExtraProcs() {
	procAStarFindPathBM = dll.NewProc("OpAStarFindPathBM")
	procAStarFindPathWay = dll.NewProc("OpAStarFindPathWay")
	procAudioGetPidFromHwnd = dll.NewProc("OpAudioGetPidFromHwnd")
	procAudioGetSystemVolume = dll.NewProc("OpAudioGetSystemVolume")
	procAudioQueryPid = dll.NewProc("OpAudioQueryPid")
	procAudioQueryWindow = dll.NewProc("OpAudioQueryWindow")
	procAudioSetSessionMute = dll.NewProc("OpAudioSetSessionMute")
	procAudioSetSessionVolume = dll.NewProc("OpAudioSetSessionVolume")
	procAudioSetSystemMute = dll.NewProc("OpAudioSetSystemMute")
	procAudioSetSystemVolume = dll.NewProc("OpAudioSetSystemVolume")
	procAutoOcrFromFile = dll.NewProc("OpAutoOcrFromFile")
	procBase64Decode = dll.NewProc("OpBase64Decode")
	procBase64Encode = dll.NewProc("OpBase64Encode")
	procBeep = dll.NewProc("OpBeep")
	procClearPicCache = dll.NewProc("OpClearPicCache")
	procCmpColorFromMem = dll.NewProc("OpCmpColorFromMem")
	procCopyFile = dll.NewProc("OpCopyFile")
	procCreateFolder = dll.NewProc("OpCreateFolder")
	procCreateQRCode = dll.NewProc("OpCreateQRCode")
	procCropImage = dll.NewProc("OpCropImage")
	procCvFlip = dll.NewProc("OpCvFlip")
	procCvMatchTemplateRot = dll.NewProc("OpCvMatchTemplateRot")
	procCvRotate = dll.NewProc("OpCvRotate")
	procDecodeQRCode = dll.NewProc("OpDecodeQRCode")
	procDecodeQRCodeFromFile = dll.NewProc("OpDecodeQRCodeFromFile")
	procDeleteFile = dll.NewProc("OpDeleteFile")
	procDeleteFolder = dll.NewProc("OpDeleteFolder")
	procDisableMinMax = dll.NewProc("OpDisableMinMax")
	procDownCpu = dll.NewProc("OpDownCpu")
	procDrawClear = dll.NewProc("OpDrawClear")
	procDrawLine = dll.NewProc("OpDrawLine")
	procDrawRect = dll.NewProc("OpDrawRect")
	procDrawSetVisible = dll.NewProc("OpDrawSetVisible")
	procDrawText = dll.NewProc("OpDrawText")
	procEnableDebugPrivilege = dll.NewProc("OpEnableDebugPrivilege")
	procFileExists = dll.NewProc("OpFileExists")
	procFindColorBlockExFromMem = dll.NewProc("OpFindColorBlockExFromMem")
	procFindColorExFromMem = dll.NewProc("OpFindColorExFromMem")
	procFindColorFromMem = dll.NewProc("OpFindColorFromMem")
	procFindMultiColorExFromMem = dll.NewProc("OpFindMultiColorExFromMem")
	procFindMultiColorFromMem = dll.NewProc("OpFindMultiColorFromMem")
	procFindNearestFeasiblePoint = dll.NewProc("OpFindNearestFeasiblePoint")
	procFindNearestPathPoint = dll.NewProc("OpFindNearestPathPoint")
	procFindPicExFromMem = dll.NewProc("OpFindPicExFromMem")
	procFindPicFromMem = dll.NewProc("OpFindPicFromMem")
	procFindStrBatch = dll.NewProc("OpFindStrBatch")
	procFindStrEx2 = dll.NewProc("OpFindStrEx2")
	procFindStrMem = dll.NewProc("OpFindStrMem")
	procFreeAllImages = dll.NewProc("OpFreeAllImages")
	procFreeImage = dll.NewProc("OpFreeImage")
	procGaiLu = dll.NewProc("OpGaiLu")
	procGetColorFromMem = dll.NewProc("OpGetColorFromMem")
	procGetColorNumFromMem = dll.NewProc("OpGetColorNumFromMem")
	procGetCursorImage = dll.NewProc("OpGetCursorImage")
	procGetDPI = dll.NewProc("OpGetDPI")
	procGetDisplayMethod = dll.NewProc("OpGetDisplayMethod")
	procGetFPS = dll.NewProc("OpGetFPS")
	procGetFileList = dll.NewProc("OpGetFileList")
	procGetFileName = dll.NewProc("OpGetFileName")
	procGetFileSize = dll.NewProc("OpGetFileSize")
	procGetImageCount = dll.NewProc("OpGetImageCount")
	procGetImageData = dll.NewProc("OpGetImageData")
	procGetImageSize = dll.NewProc("OpGetImageSize")
	procGetLastErrorString = dll.NewProc("OpGetLastErrorString")
	procGetMachineCode = dll.NewProc("OpGetMachineCode")
	procGetModuleInfoList = dll.NewProc("OpGetModuleInfoList")
	procGetModuleSize = dll.NewProc("OpGetModuleSize")
	procGetRandomDouble = dll.NewProc("OpGetRandomDouble")
	procGetRandomNumber = dll.NewProc("OpGetRandomNumber")
	procGetScreenDepth = dll.NewProc("OpGetScreenDepth")
	procGetScreenHeight = dll.NewProc("OpGetScreenHeight")
	procGetScreenWidth = dll.NewProc("OpGetScreenWidth")
	procGetTime = dll.NewProc("OpGetTime")
	procGetWindowScale = dll.NewProc("OpGetWindowScale")
	procHttpDownloadFile = dll.NewProc("OpHttpDownloadFile")
	procHttpGet = dll.NewProc("OpHttpGet")
	procHttpPost = dll.NewProc("OpHttpPost")
	procIsDisplayDead = dll.NewProc("OpIsDisplayDead")
	procIsElevated = dll.NewProc("OpIsElevated")
	procIsLineBlocked = dll.NewProc("OpIsLineBlocked")
	procJsonAppend = dll.NewProc("OpJsonAppend")
	procJsonDelete = dll.NewProc("OpJsonDelete")
	procJsonGetBool = dll.NewProc("OpJsonGetBool")
	procJsonGetKeys = dll.NewProc("OpJsonGetKeys")
	procJsonGetNumber = dll.NewProc("OpJsonGetNumber")
	procJsonGetSize = dll.NewProc("OpJsonGetSize")
	procJsonGetString = dll.NewProc("OpJsonGetString")
	procJsonIsValid = dll.NewProc("OpJsonIsValid")
	procJsonSetBool = dll.NewProc("OpJsonSetBool")
	procJsonSetNumber = dll.NewProc("OpJsonSetNumber")
	procJsonSetString = dll.NewProc("OpJsonSetString")
	procKeyPressHotkey = dll.NewProc("OpKeyPressHotkey")
	procLoadImageBmp = dll.NewProc("OpLoadImageBmp")
	procLoadImageMem = dll.NewProc("OpLoadImageMem")
	procLockWindowPosition = dll.NewProc("OpLockWindowPosition")
	procLockWindowSize = dll.NewProc("OpLockWindowSize")
	procMd5 = dll.NewProc("OpMd5")
	procMoveFile = dll.NewProc("OpMoveFile")
	procOcrAutoColor = dll.NewProc("OpOcrAutoColor")
	procOcrAutoColorEx2 = dll.NewProc("OpOcrAutoColorEx2")
	procOcrAutoColorFromFile = dll.NewProc("OpOcrAutoColorFromFile")
	procOcrAutoColorFromMemEx2 = dll.NewProc("OpOcrAutoColorFromMemEx2")
	procOcrEx2 = dll.NewProc("OpOcrEx2")
	procOcrFromMemEx2 = dll.NewProc("OpOcrFromMemEx2")
	procOcrLastProfile = dll.NewProc("OpOcrLastProfile")
	procOcrWarmup = dll.NewProc("OpOcrWarmup")
	procPointInPolygon = dll.NewProc("OpPointInPolygon")
	procReadFileString = dll.NewProc("OpReadFileString")
	procSetAStarMap = dll.NewProc("OpSetAStarMap")
	procSetAStarMapData = dll.NewProc("OpSetAStarMapData")
	procSetIme = dll.NewProc("OpSetIme")
	procSetPicCacheMax = dll.NewProc("OpSetPicCacheMax")
	procSha1 = dll.NewProc("OpSha1")
	procSimplifyPath = dll.NewProc("OpSimplifyPath")
	procSmoothPathByLOS = dll.NewProc("OpSmoothPathByLOS")
	procSortPosDistance = dll.NewProc("OpSortPosDistance")
	procTerminateProcess = dll.NewProc("OpTerminateProcess")
	procTerminateProcessTree = dll.NewProc("OpTerminateProcessTree")
	procToAbsolutePath = dll.NewProc("OpToAbsolutePath")
	procVirtualAllocEx = dll.NewProc("OpVirtualAllocEx")
	procVirtualProtectEx = dll.NewProc("OpVirtualProtectEx")
	procWriteStringToFile = dll.NewProc("OpWriteStringToFile")
	procYoloDetectEx2 = dll.NewProc("OpYoloDetectEx2")
	procYoloDetectFromFileEx2 = dll.NewProc("OpYoloDetectFromFileEx2")
	procYoloDetectFromMem = dll.NewProc("OpYoloDetectFromMem")
	procYoloDetectFromMemEx2 = dll.NewProc("OpYoloDetectFromMemEx2")
}

// AStarFindPathBM 由 scripts/gen_go_bindings.py 生成（对应 C API OpAStarFindPathBM）。
func (o *Op) AStarFindPathBM(beginX int, beginY int, endX int, endY int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procAStarFindPathBM.Call(o.handle, uintptr(beginX), uintptr(beginY), uintptr(endX), uintptr(endY))
	return wcharString(ret)
}

// AStarFindPathWay 由 scripts/gen_go_bindings.py 生成（对应 C API OpAStarFindPathWay）。
func (o *Op) AStarFindPathWay(points string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procAStarFindPathWay.Call(o.handle, strArg(points))
	return wcharString(ret)
}

// AudioGetPidFromHwnd 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioGetPidFromHwnd）。
func (o *Op) AudioGetPidFromHwnd(hwnd int64) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procAudioGetPidFromHwnd.Call(o.handle, uintptr(hwnd))
	return int(ret)
}

// AudioGetSystemVolume 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioGetSystemVolume）。
func (o *Op) AudioGetSystemVolume() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procAudioGetSystemVolume.Call(o.handle)
	return wcharString(ret)
}

// AudioQueryPid 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioQueryPid）。
func (o *Op) AudioQueryPid(pid int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procAudioQueryPid.Call(o.handle, uintptr(pid))
	return wcharString(ret)
}

// AudioQueryWindow 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioQueryWindow）。
func (o *Op) AudioQueryWindow(hwnd int64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procAudioQueryWindow.Call(o.handle, uintptr(hwnd))
	return wcharString(ret)
}

// AudioSetSessionMute 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioSetSessionMute）。
func (o *Op) AudioSetSessionMute(pid int, muted int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procAudioSetSessionMute.Call(o.handle, uintptr(pid), uintptr(muted))
	return int(ret)
}

// AudioSetSessionVolume 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioSetSessionVolume）。
func (o *Op) AudioSetSessionVolume(pid int, volume float64) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procAudioSetSessionVolume.Call(o.handle, uintptr(pid), f64Arg(volume))
	return int(ret)
}

// AudioSetSystemMute 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioSetSystemMute）。
func (o *Op) AudioSetSystemMute(muted int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procAudioSetSystemMute.Call(o.handle, uintptr(muted))
	return int(ret)
}

// AudioSetSystemVolume 由 scripts/gen_go_bindings.py 生成（对应 C API OpAudioSetSystemVolume）。
func (o *Op) AudioSetSystemVolume(volume float64) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procAudioSetSystemVolume.Call(o.handle, f64Arg(volume))
	return int(ret)
}

// AutoOcrFromFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpAutoOcrFromFile）。
func (o *Op) AutoOcrFromFile(fileName string, colorFormat string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procAutoOcrFromFile.Call(o.handle, strArg(fileName), strArg(colorFormat), f64Arg(sim))
	return wcharString(ret)
}

// Base64Decode 由 scripts/gen_go_bindings.py 生成（对应 C API OpBase64Decode）。
func (o *Op) Base64Decode(base64 string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procBase64Decode.Call(o.handle, strArg(base64))
	return wcharString(ret)
}

// Base64Encode 由 scripts/gen_go_bindings.py 生成（对应 C API OpBase64Encode）。
func (o *Op) Base64Encode(text string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procBase64Encode.Call(o.handle, strArg(text))
	return wcharString(ret)
}

// Beep 由 scripts/gen_go_bindings.py 生成（对应 C API OpBeep）。
func (o *Op) Beep(freq int, dur int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procBeep.Call(o.handle, uintptr(freq), uintptr(dur))
	return int(ret)
}

// ClearPicCache 由 scripts/gen_go_bindings.py 生成（对应 C API OpClearPicCache）。
func (o *Op) ClearPicCache() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procClearPicCache.Call(o.handle)
	return int(ret)
}

// CmpColorFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpCmpColorFromMem）。
func (o *Op) CmpColorFromMem(imageHandle string, x int, y int, color string, sim float64) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procCmpColorFromMem.Call(o.handle, strArg(imageHandle), uintptr(x), uintptr(y), strArg(color), f64Arg(sim))
	return int(ret)
}

// CopyFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpCopyFile）。
func (o *Op) CopyFile(srcFile string, dstFile string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procCopyFile.Call(o.handle, strArg(srcFile), strArg(dstFile))
	return int(ret)
}

// CreateFolder 由 scripts/gen_go_bindings.py 生成（对应 C API OpCreateFolder）。
func (o *Op) CreateFolder(folderName string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procCreateFolder.Call(o.handle, strArg(folderName))
	return int(ret)
}

// CreateQRCode 由 scripts/gen_go_bindings.py 生成（对应 C API OpCreateQRCode）。
func (o *Op) CreateQRCode(text string, fileName string, size int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procCreateQRCode.Call(o.handle, strArg(text), strArg(fileName), uintptr(size))
	return int(ret)
}

// CropImage 由 scripts/gen_go_bindings.py 生成（对应 C API OpCropImage）。
func (o *Op) CropImage(imageHandle string, x int, y int, width int, height int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procCropImage.Call(o.handle, strArg(imageHandle), uintptr(x), uintptr(y), uintptr(width), uintptr(height))
	return wcharString(ret)
}

// CvFlip 由 scripts/gen_go_bindings.py 生成（对应 C API OpCvFlip）。
func (o *Op) CvFlip(srcFile string, dstFile string, mode int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procCvFlip.Call(o.handle, strArg(srcFile), strArg(dstFile), uintptr(mode))
	return int(ret)
}

// CvMatchTemplateRot 由 scripts/gen_go_bindings.py 生成（对应 C API OpCvMatchTemplateRot）。
func (o *Op) CvMatchTemplateRot(x int, y int, width int, height int, templateName string, angles string, threshold float64, method int, colorMode int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procCvMatchTemplateRot.Call(o.handle, uintptr(x), uintptr(y), uintptr(width), uintptr(height), strArg(templateName), strArg(angles), f64Arg(threshold), uintptr(method), uintptr(colorMode))
	return wcharString(ret)
}

// CvRotate 由 scripts/gen_go_bindings.py 生成（对应 C API OpCvRotate）。
func (o *Op) CvRotate(srcFile string, dstFile string, angleDeg float64) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procCvRotate.Call(o.handle, strArg(srcFile), strArg(dstFile), f64Arg(angleDeg))
	return int(ret)
}

// DecodeQRCode 由 scripts/gen_go_bindings.py 生成（对应 C API OpDecodeQRCode）。
func (o *Op) DecodeQRCode(x1 int, y1 int, x2 int, y2 int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procDecodeQRCode.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2))
	return wcharString(ret)
}

// DecodeQRCodeFromFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpDecodeQRCodeFromFile）。
func (o *Op) DecodeQRCodeFromFile(fileName string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procDecodeQRCodeFromFile.Call(o.handle, strArg(fileName))
	return wcharString(ret)
}

// DeleteFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpDeleteFile）。
func (o *Op) DeleteFile(fileName string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDeleteFile.Call(o.handle, strArg(fileName))
	return int(ret)
}

// DeleteFolder 由 scripts/gen_go_bindings.py 生成（对应 C API OpDeleteFolder）。
func (o *Op) DeleteFolder(folderName string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDeleteFolder.Call(o.handle, strArg(folderName))
	return int(ret)
}

// DisableMinMax 由 scripts/gen_go_bindings.py 生成（对应 C API OpDisableMinMax）。
func (o *Op) DisableMinMax(hwnd uintptr, enable int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDisableMinMax.Call(o.handle, hwnd, uintptr(enable))
	return int(ret)
}

// DownCpu 由 scripts/gen_go_bindings.py 生成（对应 C API OpDownCpu）。
func (o *Op) DownCpu(typeV int, rate int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDownCpu.Call(o.handle, uintptr(typeV), uintptr(rate))
	return int(ret)
}

// DrawClear 由 scripts/gen_go_bindings.py 生成（对应 C API OpDrawClear）。
func (o *Op) DrawClear() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDrawClear.Call(o.handle)
	return int(ret)
}

// DrawLine 由 scripts/gen_go_bindings.py 生成（对应 C API OpDrawLine）。
func (o *Op) DrawLine(x1 int, y1 int, x2 int, y2 int, color string, width int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDrawLine.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), uintptr(width))
	return int(ret)
}

// DrawRect 由 scripts/gen_go_bindings.py 生成（对应 C API OpDrawRect）。
func (o *Op) DrawRect(x1 int, y1 int, x2 int, y2 int, color string, width int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDrawRect.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), uintptr(width))
	return int(ret)
}

// DrawSetVisible 由 scripts/gen_go_bindings.py 生成（对应 C API OpDrawSetVisible）。
func (o *Op) DrawSetVisible(show int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDrawSetVisible.Call(o.handle, uintptr(show))
	return int(ret)
}

// DrawText 由 scripts/gen_go_bindings.py 生成（对应 C API OpDrawText）。
func (o *Op) DrawText(x int, y int, text string, color string, fontSize int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procDrawText.Call(o.handle, uintptr(x), uintptr(y), strArg(text), strArg(color), uintptr(fontSize))
	return int(ret)
}

// EnableDebugPrivilege 由 scripts/gen_go_bindings.py 生成（对应 C API OpEnableDebugPrivilege）。
func (o *Op) EnableDebugPrivilege() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procEnableDebugPrivilege.Call(o.handle)
	return int(ret)
}

// FileExists 由 scripts/gen_go_bindings.py 生成（对应 C API OpFileExists）。
func (o *Op) FileExists(fileName string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procFileExists.Call(o.handle, strArg(fileName))
	return int(ret)
}

// FindColorBlockExFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindColorBlockExFromMem）。
func (o *Op) FindColorBlockExFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, color string, sim float64, count int, height int, width int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindColorBlockExFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim), uintptr(count), uintptr(height), uintptr(width))
	return wcharString(ret)
}

// FindColorExFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindColorExFromMem）。
func (o *Op) FindColorExFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, color string, sim float64, dir int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindColorExFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim), uintptr(dir))
	return wcharString(ret)
}

// FindColorFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindColorFromMem）。
func (o *Op) FindColorFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, color string, sim float64, dir int) (int, int, int) {
	if !o.valid() {
		return 0, 0, 0
	}
	var xOut int32
	var yOut int32
	ret, _, _ := procFindColorFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim), uintptr(dir), int32Ptr(&xOut), int32Ptr(&yOut))
	return int(ret), int(xOut), int(yOut)
}

// FindMultiColorExFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindMultiColorExFromMem）。
func (o *Op) FindMultiColorExFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, firstColor string, offsetColor string, sim float64, dir int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindMultiColorExFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(firstColor), strArg(offsetColor), f64Arg(sim), uintptr(dir))
	return wcharString(ret)
}

// FindMultiColorFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindMultiColorFromMem）。
func (o *Op) FindMultiColorFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, firstColor string, offsetColor string, sim float64, dir int) (int, int, int) {
	if !o.valid() {
		return 0, 0, 0
	}
	var xOut int32
	var yOut int32
	ret, _, _ := procFindMultiColorFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(firstColor), strArg(offsetColor), f64Arg(sim), uintptr(dir), int32Ptr(&xOut), int32Ptr(&yOut))
	return int(ret), int(xOut), int(yOut)
}

// FindNearestFeasiblePoint 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindNearestFeasiblePoint）。
func (o *Op) FindNearestFeasiblePoint(x int, y int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindNearestFeasiblePoint.Call(o.handle, uintptr(x), uintptr(y))
	return wcharString(ret)
}

// FindNearestPathPoint 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindNearestPathPoint）。
func (o *Op) FindNearestPathPoint(path string, x int, y int) (int, int, int, int) {
	if !o.valid() {
		return 0, 0, 0, 0
	}
	var indexOut int32
	var nxOut int32
	var nyOut int32
	ret, _, _ := procFindNearestPathPoint.Call(o.handle, strArg(path), uintptr(x), uintptr(y), int32Ptr(&indexOut), int32Ptr(&nxOut), int32Ptr(&nyOut))
	return int(ret), int(indexOut), int(nxOut), int(nyOut)
}

// FindPicExFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindPicExFromMem）。
func (o *Op) FindPicExFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, files string, deltaColor string, sim float64, dir int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindPicExFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(files), strArg(deltaColor), f64Arg(sim), uintptr(dir))
	return wcharString(ret)
}

// FindPicFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindPicFromMem）。
func (o *Op) FindPicFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, files string, deltaColor string, sim float64, dir int) (int, int, int) {
	if !o.valid() {
		return 0, 0, 0
	}
	var xOut int32
	var yOut int32
	ret, _, _ := procFindPicFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(files), strArg(deltaColor), f64Arg(sim), uintptr(dir), int32Ptr(&xOut), int32Ptr(&yOut))
	return int(ret), int(xOut), int(yOut)
}

// FindStrBatch 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindStrBatch）。
func (o *Op) FindStrBatch(x1 int, y1 int, x2 int, y2 int, jsonTargets string, color string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindStrBatch.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(jsonTargets), strArg(color), f64Arg(sim))
	return wcharString(ret)
}

// FindStrEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindStrEx2）。
func (o *Op) FindStrEx2(x1 int, y1 int, x2 int, y2 int, pattern string, color string, sim float64, flags int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindStrEx2.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(pattern), strArg(color), f64Arg(sim), uintptr(flags))
	return wcharString(ret)
}

// FindStrMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpFindStrMem）。
func (o *Op) FindStrMem(imageHandle string, jsonTargets string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procFindStrMem.Call(o.handle, strArg(imageHandle), strArg(jsonTargets), f64Arg(sim))
	return wcharString(ret)
}

// FreeAllImages 由 scripts/gen_go_bindings.py 生成（对应 C API OpFreeAllImages）。
func (o *Op) FreeAllImages() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procFreeAllImages.Call(o.handle)
	return int(ret)
}

// FreeImage 由 scripts/gen_go_bindings.py 生成（对应 C API OpFreeImage）。
func (o *Op) FreeImage(imageHandle string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procFreeImage.Call(o.handle, strArg(imageHandle))
	return int(ret)
}

// GaiLu 由 scripts/gen_go_bindings.py 生成（对应 C API OpGaiLu）。
func (o *Op) GaiLu(p int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGaiLu.Call(o.handle, uintptr(p))
	return int(ret)
}

// GetColorFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetColorFromMem）。
func (o *Op) GetColorFromMem(imageHandle string, x int, y int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetColorFromMem.Call(o.handle, strArg(imageHandle), uintptr(x), uintptr(y))
	return wcharString(ret)
}

// GetColorNumFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetColorNumFromMem）。
func (o *Op) GetColorNumFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, color string, sim float64) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetColorNumFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim))
	return int(ret)
}

// GetCursorImage 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetCursorImage）。
func (o *Op) GetCursorImage() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetCursorImage.Call(o.handle)
	return wcharString(ret)
}

// GetDPI 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetDPI）。
func (o *Op) GetDPI() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetDPI.Call(o.handle)
	return int(ret)
}

// GetDisplayMethod 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetDisplayMethod）。
func (o *Op) GetDisplayMethod() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetDisplayMethod.Call(o.handle)
	return wcharString(ret)
}

// GetFPS 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetFPS）。
func (o *Op) GetFPS() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetFPS.Call(o.handle)
	return int(ret)
}

// GetFileList 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetFileList）。
func (o *Op) GetFileList(folder string, wildcard string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetFileList.Call(o.handle, strArg(folder), strArg(wildcard))
	return wcharString(ret)
}

// GetFileName 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetFileName）。
func (o *Op) GetFileName(filePath string, flag int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetFileName.Call(o.handle, strArg(filePath), uintptr(flag))
	return wcharString(ret)
}

// GetFileSize 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetFileSize）。
func (o *Op) GetFileSize(fileName string) (int64, int) {
	if !o.valid() {
		return 0, 0
	}
	var outRetOut int32
	ret, _, _ := procGetFileSize.Call(o.handle, strArg(fileName), int32Ptr(&outRetOut))
	return int64(ret), int(outRetOut)
}

// GetImageCount 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetImageCount）。
func (o *Op) GetImageCount() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetImageCount.Call(o.handle)
	return int(ret)
}

// GetImageData 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetImageData）。
func (o *Op) GetImageData(imageHandle string, fmt string, buffer []byte) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetImageData.Call(o.handle, strArg(imageHandle), strArg(fmt), bytesPtr(buffer), uintptr(len(buffer)))
	return int(ret)
}

// GetImageSize 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetImageSize）。
func (o *Op) GetImageSize(imageHandle string) (int, int, int, int) {
	if !o.valid() {
		return 0, 0, 0, 0
	}
	var widthOut int32
	var heightOut int32
	var channelsOut int32
	ret, _, _ := procGetImageSize.Call(o.handle, strArg(imageHandle), int32Ptr(&widthOut), int32Ptr(&heightOut), int32Ptr(&channelsOut))
	return int(ret), int(widthOut), int(heightOut), int(channelsOut)
}

// GetLastErrorString 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetLastErrorString）。
func (o *Op) GetLastErrorString() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetLastErrorString.Call(o.handle)
	return wcharString(ret)
}

// GetMachineCode 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetMachineCode）。
func (o *Op) GetMachineCode() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetMachineCode.Call(o.handle)
	return wcharString(ret)
}

// GetModuleInfoList 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetModuleInfoList）。
func (o *Op) GetModuleInfoList(hwnd uintptr) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetModuleInfoList.Call(o.handle, hwnd)
	return wcharString(ret)
}

// GetModuleSize 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetModuleSize）。
func (o *Op) GetModuleSize(hwnd uintptr, module string) {
	if !o.valid() {
		return
	}
	procGetModuleSize.Call(o.handle, hwnd, strArg(module))
}

// GetRandomDouble 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetRandomDouble）。
func (o *Op) GetRandomDouble(min float64, max float64) {
	if !o.valid() {
		return
	}
	procGetRandomDouble.Call(o.handle, f64Arg(min), f64Arg(max))
}

// GetRandomNumber 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetRandomNumber）。
func (o *Op) GetRandomNumber(min int, max int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetRandomNumber.Call(o.handle, uintptr(min), uintptr(max))
	return int(ret)
}

// GetScreenDepth 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetScreenDepth）。
func (o *Op) GetScreenDepth() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetScreenDepth.Call(o.handle)
	return int(ret)
}

// GetScreenHeight 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetScreenHeight）。
func (o *Op) GetScreenHeight() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetScreenHeight.Call(o.handle)
	return int(ret)
}

// GetScreenWidth 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetScreenWidth）。
func (o *Op) GetScreenWidth() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procGetScreenWidth.Call(o.handle)
	return int(ret)
}

// GetTime 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetTime）。
func (o *Op) GetTime() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetTime.Call(o.handle)
	return wcharString(ret)
}

// GetWindowScale 由 scripts/gen_go_bindings.py 生成（对应 C API OpGetWindowScale）。
func (o *Op) GetWindowScale(hwnd uintptr) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procGetWindowScale.Call(o.handle, hwnd)
	return wcharString(ret)
}

// HttpDownloadFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpHttpDownloadFile）。
func (o *Op) HttpDownloadFile(url string, fileName string, timeoutMs int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procHttpDownloadFile.Call(o.handle, strArg(url), strArg(fileName), uintptr(timeoutMs))
	return int(ret)
}

// HttpGet 由 scripts/gen_go_bindings.py 生成（对应 C API OpHttpGet）。
func (o *Op) HttpGet(url string, headersJson string, timeoutMs int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procHttpGet.Call(o.handle, strArg(url), strArg(headersJson), uintptr(timeoutMs))
	return wcharString(ret)
}

// HttpPost 由 scripts/gen_go_bindings.py 生成（对应 C API OpHttpPost）。
func (o *Op) HttpPost(url string, body string, headersJson string, timeoutMs int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procHttpPost.Call(o.handle, strArg(url), strArg(body), strArg(headersJson), uintptr(timeoutMs))
	return wcharString(ret)
}

// IsDisplayDead 由 scripts/gen_go_bindings.py 生成（对应 C API OpIsDisplayDead）。
func (o *Op) IsDisplayDead(x1 int, y1 int, x2 int, y2 int, checkTimes int, intervalMs int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procIsDisplayDead.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), uintptr(checkTimes), uintptr(intervalMs))
	return int(ret)
}

// IsElevated 由 scripts/gen_go_bindings.py 生成（对应 C API OpIsElevated）。
func (o *Op) IsElevated() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procIsElevated.Call(o.handle)
	return int(ret)
}

// IsLineBlocked 由 scripts/gen_go_bindings.py 生成（对应 C API OpIsLineBlocked）。
func (o *Op) IsLineBlocked(x1 int, y1 int, x2 int, y2 int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procIsLineBlocked.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2))
	return int(ret)
}

// JsonAppend 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonAppend）。
func (o *Op) JsonAppend(jsonText string, path string, rawJson string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonAppend.Call(o.handle, strArg(jsonText), strArg(path), strArg(rawJson))
	return wcharString(ret)
}

// JsonDelete 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonDelete）。
func (o *Op) JsonDelete(jsonText string, path string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonDelete.Call(o.handle, strArg(jsonText), strArg(path))
	return wcharString(ret)
}

// JsonGetBool 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonGetBool）。
func (o *Op) JsonGetBool(jsonText string, path string) (int, int) {
	if !o.valid() {
		return 0, 0
	}
	var valueOut int32
	ret, _, _ := procJsonGetBool.Call(o.handle, strArg(jsonText), strArg(path), int32Ptr(&valueOut))
	return int(ret), int(valueOut)
}

// JsonGetKeys 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonGetKeys）。
func (o *Op) JsonGetKeys(jsonText string, path string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonGetKeys.Call(o.handle, strArg(jsonText), strArg(path))
	return wcharString(ret)
}

// JsonGetNumber 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonGetNumber）。
func (o *Op) JsonGetNumber(jsonText string, path string) (int, float64) {
	if !o.valid() {
		return 0, 0
	}
	var valueOut float64
	ret, _, _ := procJsonGetNumber.Call(o.handle, strArg(jsonText), strArg(path), float64Ptr(&valueOut))
	return int(ret), float64(valueOut)
}

// JsonGetSize 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonGetSize）。
func (o *Op) JsonGetSize(jsonText string, path string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procJsonGetSize.Call(o.handle, strArg(jsonText), strArg(path))
	return int(ret)
}

// JsonGetString 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonGetString）。
func (o *Op) JsonGetString(jsonText string, path string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonGetString.Call(o.handle, strArg(jsonText), strArg(path))
	return wcharString(ret)
}

// JsonIsValid 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonIsValid）。
func (o *Op) JsonIsValid(jsonText string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procJsonIsValid.Call(o.handle, strArg(jsonText))
	return int(ret)
}

// JsonSetBool 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonSetBool）。
func (o *Op) JsonSetBool(jsonText string, path string, value int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonSetBool.Call(o.handle, strArg(jsonText), strArg(path), uintptr(value))
	return wcharString(ret)
}

// JsonSetNumber 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonSetNumber）。
func (o *Op) JsonSetNumber(jsonText string, path string, value float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonSetNumber.Call(o.handle, strArg(jsonText), strArg(path), f64Arg(value))
	return wcharString(ret)
}

// JsonSetString 由 scripts/gen_go_bindings.py 生成（对应 C API OpJsonSetString）。
func (o *Op) JsonSetString(jsonText string, path string, value string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procJsonSetString.Call(o.handle, strArg(jsonText), strArg(path), strArg(value))
	return wcharString(ret)
}

// KeyPressHotkey 由 scripts/gen_go_bindings.py 生成（对应 C API OpKeyPressHotkey）。
func (o *Op) KeyPressHotkey(combo string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procKeyPressHotkey.Call(o.handle, strArg(combo))
	return int(ret)
}

// LoadImageBmp 由 scripts/gen_go_bindings.py 生成（对应 C API OpLoadImageBmp）。
func (o *Op) LoadImageBmp(data []byte) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procLoadImageBmp.Call(o.handle, bytesPtr(data), uintptr(len(data)))
	return wcharString(ret)
}

// LoadImageMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpLoadImageMem）。
func (o *Op) LoadImageMem(data []byte, height int, stride int, fmt string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procLoadImageMem.Call(o.handle, bytesPtr(data), uintptr(len(data)), uintptr(height), uintptr(stride), strArg(fmt))
	return wcharString(ret)
}

// LockWindowPosition 由 scripts/gen_go_bindings.py 生成（对应 C API OpLockWindowPosition）。
func (o *Op) LockWindowPosition(hwnd uintptr, enable int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procLockWindowPosition.Call(o.handle, hwnd, uintptr(enable))
	return int(ret)
}

// LockWindowSize 由 scripts/gen_go_bindings.py 生成（对应 C API OpLockWindowSize）。
func (o *Op) LockWindowSize(hwnd uintptr, enable int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procLockWindowSize.Call(o.handle, hwnd, uintptr(enable))
	return int(ret)
}

// Md5 由 scripts/gen_go_bindings.py 生成（对应 C API OpMd5）。
func (o *Op) Md5(text string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procMd5.Call(o.handle, strArg(text))
	return wcharString(ret)
}

// MoveFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpMoveFile）。
func (o *Op) MoveFile(srcFile string, dstFile string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procMoveFile.Call(o.handle, strArg(srcFile), strArg(dstFile))
	return int(ret)
}

// OcrAutoColor 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrAutoColor）。
func (o *Op) OcrAutoColor(x1 int, y1 int, x2 int, y2 int, color string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procOcrAutoColor.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim))
	return wcharString(ret)
}

// OcrAutoColorEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrAutoColorEx2）。
func (o *Op) OcrAutoColorEx2(x1 int, y1 int, x2 int, y2 int, color string, sim float64) (string, int) {
	if !o.valid() {
		return "", 0
	}
	var findCountOut int32
	ret, _, _ := procOcrAutoColorEx2.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim), int32Ptr(&findCountOut))
	return wcharString(ret), int(findCountOut)
}

// OcrAutoColorFromFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrAutoColorFromFile）。
func (o *Op) OcrAutoColorFromFile(fileName string, colorFormat string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procOcrAutoColorFromFile.Call(o.handle, strArg(fileName), strArg(colorFormat), f64Arg(sim))
	return wcharString(ret)
}

// OcrAutoColorFromMemEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrAutoColorFromMemEx2）。
func (o *Op) OcrAutoColorFromMemEx2(imageHandle string, x1 int, y1 int, x2 int, y2 int, color string, sim float64) (string, int) {
	if !o.valid() {
		return "", 0
	}
	var findCountOut int32
	ret, _, _ := procOcrAutoColorFromMemEx2.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim), int32Ptr(&findCountOut))
	return wcharString(ret), int(findCountOut)
}

// OcrEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrEx2）。
func (o *Op) OcrEx2(x1 int, y1 int, x2 int, y2 int, color string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procOcrEx2.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim))
	return wcharString(ret)
}

// OcrFromMemEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrFromMemEx2）。
func (o *Op) OcrFromMemEx2(imageHandle string, x1 int, y1 int, x2 int, y2 int, color string, sim float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procOcrFromMemEx2.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), strArg(color), f64Arg(sim))
	return wcharString(ret)
}

// OcrLastProfile 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrLastProfile）。
func (o *Op) OcrLastProfile() string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procOcrLastProfile.Call(o.handle)
	return wcharString(ret)
}

// OcrWarmup 由 scripts/gen_go_bindings.py 生成（对应 C API OpOcrWarmup）。
func (o *Op) OcrWarmup() int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procOcrWarmup.Call(o.handle)
	return int(ret)
}

// PointInPolygon 由 scripts/gen_go_bindings.py 生成（对应 C API OpPointInPolygon）。
func (o *Op) PointInPolygon(point string, polygon string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procPointInPolygon.Call(o.handle, strArg(point), strArg(polygon))
	return int(ret)
}

// ReadFileString 由 scripts/gen_go_bindings.py 生成（对应 C API OpReadFileString）。
func (o *Op) ReadFileString(fileName string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procReadFileString.Call(o.handle, strArg(fileName))
	return wcharString(ret)
}

// SetAStarMap 由 scripts/gen_go_bindings.py 生成（对应 C API OpSetAStarMap）。
func (o *Op) SetAStarMap(bitmapFile string, scale int, offsetX int, offsetY int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procSetAStarMap.Call(o.handle, strArg(bitmapFile), uintptr(scale), uintptr(offsetX), uintptr(offsetY))
	return int(ret)
}

// SetAStarMapData 由 scripts/gen_go_bindings.py 生成（对应 C API OpSetAStarMapData）。
func (o *Op) SetAStarMapData(width int, height int, bgraData []byte, scale int, offsetX int, offsetY int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procSetAStarMapData.Call(o.handle, uintptr(width), uintptr(height), bytesPtr(bgraData), uintptr(len(bgraData)), uintptr(scale), uintptr(offsetX), uintptr(offsetY))
	return int(ret)
}

// SetIme 由 scripts/gen_go_bindings.py 生成（对应 C API OpSetIme）。
func (o *Op) SetIme(hwnd uintptr, enable int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procSetIme.Call(o.handle, hwnd, uintptr(enable))
	return int(ret)
}

// SetPicCacheMax 由 scripts/gen_go_bindings.py 生成（对应 C API OpSetPicCacheMax）。
func (o *Op) SetPicCacheMax(maxCount int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procSetPicCacheMax.Call(o.handle, uintptr(maxCount))
	return int(ret)
}

// Sha1 由 scripts/gen_go_bindings.py 生成（对应 C API OpSha1）。
func (o *Op) Sha1(text string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procSha1.Call(o.handle, strArg(text))
	return wcharString(ret)
}

// SimplifyPath 由 scripts/gen_go_bindings.py 生成（对应 C API OpSimplifyPath）。
func (o *Op) SimplifyPath(path string, epsilon float64) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procSimplifyPath.Call(o.handle, strArg(path), f64Arg(epsilon))
	return wcharString(ret)
}

// SmoothPathByLOS 由 scripts/gen_go_bindings.py 生成（对应 C API OpSmoothPathByLOS）。
func (o *Op) SmoothPathByLOS(path string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procSmoothPathByLOS.Call(o.handle, strArg(path))
	return wcharString(ret)
}

// SortPosDistance 由 scripts/gen_go_bindings.py 生成（对应 C API OpSortPosDistance）。
func (o *Op) SortPosDistance(allPos string, typeV int, x int, y int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procSortPosDistance.Call(o.handle, strArg(allPos), uintptr(typeV), uintptr(x), uintptr(y))
	return wcharString(ret)
}

// TerminateProcess 由 scripts/gen_go_bindings.py 生成（对应 C API OpTerminateProcess）。
func (o *Op) TerminateProcess(pid int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procTerminateProcess.Call(o.handle, uintptr(pid))
	return int(ret)
}

// TerminateProcessTree 由 scripts/gen_go_bindings.py 生成（对应 C API OpTerminateProcessTree）。
func (o *Op) TerminateProcessTree(pid int) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procTerminateProcessTree.Call(o.handle, uintptr(pid))
	return int(ret)
}

// ToAbsolutePath 由 scripts/gen_go_bindings.py 生成（对应 C API OpToAbsolutePath）。
func (o *Op) ToAbsolutePath(path string) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procToAbsolutePath.Call(o.handle, strArg(path))
	return wcharString(ret)
}

// VirtualAllocEx 由 scripts/gen_go_bindings.py 生成（对应 C API OpVirtualAllocEx）。
func (o *Op) VirtualAllocEx(hwnd uintptr, address string, size int, allocType int, protect int) string {
	if !o.valid() {
		return ""
	}
	ret, _, _ := procVirtualAllocEx.Call(o.handle, hwnd, strArg(address), uintptr(size), uintptr(allocType), uintptr(protect))
	return wcharString(ret)
}

// VirtualProtectEx 由 scripts/gen_go_bindings.py 生成（对应 C API OpVirtualProtectEx）。
func (o *Op) VirtualProtectEx(hwnd uintptr, address string, size int, newProtect int) (int, int) {
	if !o.valid() {
		return 0, 0
	}
	var oldProtectOut int32
	ret, _, _ := procVirtualProtectEx.Call(o.handle, hwnd, strArg(address), uintptr(size), uintptr(newProtect), int32Ptr(&oldProtectOut))
	return int(ret), int(oldProtectOut)
}

// WriteStringToFile 由 scripts/gen_go_bindings.py 生成（对应 C API OpWriteStringToFile）。
func (o *Op) WriteStringToFile(fileName string, content string) int {
	if !o.valid() {
		return 0
	}
	ret, _, _ := procWriteStringToFile.Call(o.handle, strArg(fileName), strArg(content))
	return int(ret)
}

// YoloDetectEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpYoloDetectEx2）。
func (o *Op) YoloDetectEx2(x1 int, y1 int, x2 int, y2 int, conf float64, iou float64, jsonOptions string) (string, int) {
	if !o.valid() {
		return "", 0
	}
	var findCountOut int32
	ret, _, _ := procYoloDetectEx2.Call(o.handle, uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), f64Arg(conf), f64Arg(iou), strArg(jsonOptions), int32Ptr(&findCountOut))
	return wcharString(ret), int(findCountOut)
}

// YoloDetectFromFileEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpYoloDetectFromFileEx2）。
func (o *Op) YoloDetectFromFileEx2(fileName string, conf float64, iou float64, jsonOptions string) (string, int) {
	if !o.valid() {
		return "", 0
	}
	var findCountOut int32
	ret, _, _ := procYoloDetectFromFileEx2.Call(o.handle, strArg(fileName), f64Arg(conf), f64Arg(iou), strArg(jsonOptions), int32Ptr(&findCountOut))
	return wcharString(ret), int(findCountOut)
}

// YoloDetectFromMem 由 scripts/gen_go_bindings.py 生成（对应 C API OpYoloDetectFromMem）。
func (o *Op) YoloDetectFromMem(imageHandle string, x1 int, y1 int, x2 int, y2 int, conf float64, iou float64) (string, int) {
	if !o.valid() {
		return "", 0
	}
	var findCountOut int32
	ret, _, _ := procYoloDetectFromMem.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), f64Arg(conf), f64Arg(iou), int32Ptr(&findCountOut))
	return wcharString(ret), int(findCountOut)
}

// YoloDetectFromMemEx2 由 scripts/gen_go_bindings.py 生成（对应 C API OpYoloDetectFromMemEx2）。
func (o *Op) YoloDetectFromMemEx2(imageHandle string, x1 int, y1 int, x2 int, y2 int, conf float64, iou float64, jsonOptions string) (string, int) {
	if !o.valid() {
		return "", 0
	}
	var findCountOut int32
	ret, _, _ := procYoloDetectFromMemEx2.Call(o.handle, strArg(imageHandle), uintptr(x1), uintptr(y1), uintptr(x2), uintptr(y2), f64Arg(conf), f64Arg(iou), strArg(jsonOptions), int32Ptr(&findCountOut))
	return wcharString(ret), int(findCountOut)
}
