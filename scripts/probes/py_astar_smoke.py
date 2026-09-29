import ctypes, sys
sys.path.insert(0, r"D:/AutoPro/op-master/op/bindings/python")
from op import api

op = api.Op(dll_dir=r"D:/AutoPro/op-master/op/build/nmake-x64-Release/libop")
print("dll ok")
# 16x16 map, wall x=8 y0..10
W = H = 16
buf = bytearray(W*H*4)
for i in range(len(buf)//4):
    buf[i*4+0] = buf[i*4+1] = buf[i*4+2] = 0xff; buf[i*4+3] = 0xff
for y in range(0, 11):
    idx = (y*W + 8)*4
    buf[idx] = buf[idx+1] = buf[idx+2] = 0x00
assert op.set_astar_map_data(W, H, bytes(buf), 1, 0, 0)
path = op.a_star_find_path_bm(2, 8, 14, 8)
print("path:", path[:60], "...")
assert path and "2,8" in path.split("|")[0]
blocked = op.is_line_blocked(2, 5, 14, 5)
print("is_line_blocked cross wall:", blocked)
assert blocked == 1
smooth = op.smooth_path_by_los(path)
print("smooth pts:", len(smooth.split("|")), "raw pts:", len(path.split("|")))
simple = op.simplify_path("0,0|1,0|2,0|3,0", 0.5)
print("simplify:", simple)
assert simple == "0,0|3,0"
way = op.a_star_find_path_way("2,8|14,8|14,14")
print("way ok:", bool(way), way.split("|")[-1])
assert way.split("|")[-1] == "14,14"
idx, nx, ny = op.find_nearest_path_point("0,0|10,0|20,0", 11, 1)
print("nearest:", idx, nx, ny)
assert (idx, nx, ny) == (1, 10, 0)
print("pip in:", op.point_in_polygon("5,5", "0,0|10,0|10,10|0,10"))
print("ALL PYTHON SMOKE OK")
