import os
import subprocess
import sys

REPO = r"D:\AutoPro\op-master\op"
os.chdir(REPO)
env = dict(os.environ)
env["PATH"] = os.path.join(REPO, "build", "nmake-x64-Release", "libop") + ";" + env["PATH"]

f = "*RunApp*"
if len(sys.argv) > 1:
    f = sys.argv[1]

r = subprocess.run(
    [sys.executable, os.path.join(REPO, "scripts", "probes", "run_optest.py"), f],
    capture_output=True, text=True, cwd=REPO, env=env, errors="replace",
)
out = (r.stdout or "") + (r.stderr or "")
lines = out.splitlines()
for ln in lines[-40:]:
    print(ln)
print("EXIT =", r.returncode)
