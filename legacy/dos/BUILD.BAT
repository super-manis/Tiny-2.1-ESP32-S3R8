@ECHO OFF
REM Rebuild TINY.EXE with Turbo C 2.0. Change TC= if Turbo C is elsewhere.
SET TC=C:\TC
%TC%\TCC -ml -I%TC%\INCLUDE -L%TC%\LIB TINY.C
SET TC=
