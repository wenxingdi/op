//go:build windows

// 2026-11 补齐的 126 个接口的运行时冒烟（不是"能编译"了事）。
//
// 跑法（需要先构建 x64 DLL）：
//   cd bindings/go
//   set GOTOOLCHAIN=local
//   go test -run TestExtraBindingsSmoke -v
//
// 注意：`go build` 只能证明签名能过编译，证明不了 GetProcAddress 拿得到、
// 参数编排对、返回值编解码对 —— 那些只有真调一次才知道。
package opcapi

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func testOp(t *testing.T) *Op {
	t.Helper()
	dll := filepath.Join("..", "..", "build", "nmake-x64-Release", "libop", "op_c_api_x64.dll")
	if _, err := os.Stat(dll); err != nil {
		t.Skipf("dll not found: %s", dll)
	}
	op, err := NewWithOptions(Options{DLLPath: dll})
	if err != nil {
		t.Fatalf("NewWithOptions: %v", err)
	}
	return op
}

func TestExtraBindingsSmoke(t *testing.T) {
	op := testOp(t)

	// 无参整数返回
	if w := op.GetScreenWidth(); w <= 0 {
		t.Errorf("GetScreenWidth() = %d, want > 0", w)
	}
	if h := op.GetScreenHeight(); h <= 0 {
		t.Errorf("GetScreenHeight() = %d, want > 0", h)
	}
	if d := op.GetScreenDepth(); d <= 0 {
		t.Errorf("GetScreenDepth() = %d, want > 0", d)
	}
	if ts := op.GetTime(); ts == "" {
		t.Error("GetTime() 为空")
	}

	// 文件尺寸（out 参数路径）
	tmp := filepath.Join(t.TempDir(), "size.txt")
	if err := os.WriteFile(tmp, []byte("hello"), 0o644); err != nil {
		t.Fatalf("write temp: %v", err)
	}
	if size, ret := op.GetFileSize(tmp); size != 5 || ret != 1 {
		t.Errorf("GetFileSize() = (%d, %d), want (5, 1)", size, ret)
	}

	// 内存版障碍地图 + 视线遮挡（本轮新加的两个：SetAStarMapData / IsLineBlocked）
	const w, h = 32, 32
	pixels := make([]byte, w*h*4)
	for i := range pixels {
		pixels[i] = 255
	}
	for y := 14; y < 18; y++ {
		for x := 14; x < 18; x++ {
			p := (y*w + x) * 4
			pixels[p], pixels[p+1], pixels[p+2] = 0, 0, 0
			pixels[p+3] = 255
		}
	}
	if ret := op.SetAStarMapData(w, h, pixels, 1, 0, 0); ret != 1 {
		t.Fatalf("SetAStarMapData() = %d, want 1", ret)
	}
	blocked := op.IsLineBlocked(10, 16, 22, 16)
	if blocked != 1 {
		t.Errorf("IsLineBlocked across the obstacle = %d, want 1 (-1 = 地图没设上)", blocked)
	}
	// 障碍上方竖直方向不该被挡
	if free := op.IsLineBlocked(2, 2, 29, 2); free != 0 {
		t.Errorf("IsLineBlocked clear row = %d, want 0", free)
	}

	// 多边形命中（字符串往返 + 布尔语义）
	if in := op.PointInPolygon("5,5", "0,0|10,0|10,10|0,10"); in != 1 {
		t.Errorf("PointInPolygon inside = %d, want 1", in)
	}

	// JSON 取值（out double 路径）
	j := `{"value":3.5}`
	ret, num := op.JsonGetNumber(j, "value")
	if ret != 1 || num < 3.4 || num > 3.6 {
		t.Errorf("JsonGetNumber() = (%d, %v), want (1, ~3.5)", ret, num)
	}

	// 字符串返回 + 出参组合
	if keys := op.JsonGetKeys(j, ""); keys == "" || !strings.Contains(keys, "value") {
		t.Errorf("JsonGetKeys() = %q, want contains value", keys)
	}
}
