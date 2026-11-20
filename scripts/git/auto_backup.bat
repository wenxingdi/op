@echo off
rem ============================================================
rem OP 仓库定时备份脚本（供 Windows 任务计划程序调用）
rem 作用：把工作区未提交的改动自动提交并推送（post-commit 钩子会接力推送）
rem 用法：schtasks /Create /TN "OP-Git-AutoBackup" /SC MINUTE /MO 30 /TR "\"D:\AutoPro\op-master\op\scripts\git\auto_backup.bat\""
rem ============================================================
cd /d D:\AutoPro\op-master\op

rem 若有正在进行中的 rebase/merge 则跳过，避免搅乱现场
if exist ".git\rebase-merge" goto :eof
if exist ".git\rebase-apply" goto :eof
if exist ".git\MERGE_HEAD" goto :eof

git add -A
git diff --cached --quiet
if %errorlevel%==0 goto :push

git commit -m "auto-backup: %date% %time%" >nul 2>&1
rem post-commit 钩子会自动执行推送

:push
rem 兜底：即使没有新提交，也把可能漏推的提交推上去
git push origin master >nul 2>&1
