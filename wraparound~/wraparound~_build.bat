@echo off
SET VC=C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC
SET PD=C:\Code\pd

:ECHO %VC%
:ECHO %PD%

SET PDNTCFLAGS=/W3 /WX /DNT /DPD /nologo
SET PDNTINCLUDE=/I"%PD%\tcl\include" /I"%PD%\src" /I"%VC%\include"
SET PDNTLDIR=%VC%\lib
SET PDNTLIB="%PDNTLDIR%\libcmt.lib" "%PDNTLDIR%\oldnames.lib" "%PD%\bin\pd.lib"

:ECHO %PDNTCFLAGS%
:ECHO %PDNTINCLUDE%
:ECHO %PDNTLDIR%
:ECHO %PDNTLIB%

:SET /p SOURCEFILE=What is the source file for this external without file extension? 
:SET /p SETUPFUNC=What is the name of the setup function for this external?
SET SOURCEFILE=wraparound~
SET SETUPFUNC=wraparound_tilde_setup

cl %PDNTCFLAGS% %PDNTINCLUDE% /c %SOURCEFILE%.c
link /dll /export:%SETUPFUNC% %SOURCEFILE%.obj %PDNTLIB%
rm *.obj *.lib *.exp