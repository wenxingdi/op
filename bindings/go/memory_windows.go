//go:build windows

package opcapi

func (o *Op) WriteData(hwnd uintptr, address, data string, size int) int {
	if !o.valid() {
		return 0
	}

	ret, _, _ := procWriteData.Call(o.handle, hwnd, strArg(address), strArg(data), uintptr(size))
	return int(ret)
}

func (o *Op) ReadData(hwnd uintptr, address string, size int) string {
	if !o.valid() {
		return ""
	}

	ret, _, _ := procReadData.Call(o.handle, hwnd, strArg(address), uintptr(size))
	return wcharString(ret)
}

func (o *Op) ReadInt(hwnd uintptr, address string, typ int) (int, int64) {
	if !o.valid() {
		return 0, 0
	}

	var value int64
	ret, _, _ := procReadInt.Call(o.handle, hwnd, strArg(address), uintptr(typ), int64Ptr(&value))
	return int(ret), value
}

func (o *Op) WriteInt(hwnd uintptr, address string, typ int, value int64) int {
	if !o.valid() {
		return 0
	}

	ret, _, _ := procWriteInt.Call(o.handle, hwnd, strArg(address), uintptr(typ), uintptr(value))
	return int(ret)
}

func (o *Op) ReadFloat(hwnd uintptr, address string) (int, float32) {
	if !o.valid() {
		return 0, 0
	}

	var value float32
	ret, _, _ := procReadFloat.Call(o.handle, hwnd, strArg(address), float32Ptr(&value))
	return int(ret), value
}

func (o *Op) WriteFloat(hwnd uintptr, address string, value float32) int {
	if !o.valid() {
		return 0
	}

	ret, _, _ := procWriteFloat.Call(o.handle, hwnd, strArg(address), f32Arg(value))
	return int(ret)
}

func (o *Op) ReadDouble(hwnd uintptr, address string) (int, float64) {
	if !o.valid() {
		return 0, 0
	}

	var value float64
	ret, _, _ := procReadDouble.Call(o.handle, hwnd, strArg(address), float64Ptr(&value))
	return int(ret), value
}

func (o *Op) WriteDouble(hwnd uintptr, address string, value float64) int {
	if !o.valid() {
		return 0
	}

	ret, _, _ := procWriteDouble.Call(o.handle, hwnd, strArg(address), f64Arg(value))
	return int(ret)
}

func (o *Op) ReadString(hwnd uintptr, address string, typ, length int) string {
	if !o.valid() {
		return ""
	}

	ret, _, _ := procReadString.Call(o.handle, hwnd, strArg(address), uintptr(typ), uintptr(length))
	return wcharString(ret)
}

func (o *Op) WriteString(hwnd uintptr, address string, typ int, value string) int {
	if !o.valid() {
		return 0
	}

	ret, _, _ := procWriteString.Call(o.handle, hwnd, strArg(address), uintptr(typ), strArg(value))
	return int(ret)
}

// FindData 特征码搜索内存：pattern 十六进制串支持 ?? 通配；addrRange "start-end" 十六进制，空=全部已提交可读区。
// 返回 "addr|addr|..."（大写十六进制）。
func (o *Op) FindData(hwnd uintptr, addrRange, pattern string) string {
	if !o.valid() {
		return ""
	}

	ret, _, _ := procFindData.Call(o.handle, hwnd, strArg(addrRange), strArg(pattern))
	return wcharString(ret)
}

// FindDataEx 同 FindData；step<=0 视为 1，count<=0 用默认上限 1024。
func (o *Op) FindDataEx(hwnd uintptr, addrRange, pattern string, step, count int) string {
	if !o.valid() {
		return ""
	}

	ret, _, _ := procFindDataEx.Call(o.handle, hwnd, strArg(addrRange), strArg(pattern), uintptr(step), uintptr(count))
	return wcharString(ret)
}

// GetModuleBaseAddr 取目标进程模块基址（大写十六进制串）；失败返回空串。
func (o *Op) GetModuleBaseAddr(hwnd uintptr, module string) string {
	if !o.valid() {
		return ""
	}

	ret, _, _ := procGetModuleBaseAddr.Call(o.handle, hwnd, strArg(module))
	return wcharString(ret)
}
