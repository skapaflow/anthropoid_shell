@echo off
color a

set name=core
set src=%name%.c
set exe=%name%.exe
set flags=-Wwrite-strings -Wextra -Werror -Wall

echo --------------------
echo GCC: BUILD %name%
echo --------------------
if EXIST %exe% (del %exe%)

gcc -o %exe% %src% %flags% -s -std=gnu99 -lwinmm -lkernel32

if EXIST %exe% (goto :DONE) else (goto :FAILED)
:DONE
	echo DONE: %exe%
	echo --------------------
	echo BUILT!
	pause > nul
	exit
:FAILED
	echo ERROR: %src%
	echo -------------------------
	pause > nul
	exit