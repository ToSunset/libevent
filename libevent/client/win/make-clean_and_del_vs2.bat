@rem Visual C++ 工程清理脚本
@rem 本清理脚本由周中亚编写，欢迎传播

@echo off
echo -------------------------------------------------------
echo **    清理脚本 Ver 0.1        
echo **                                                                     
echo **     作者: 周中亚                                            
echo **
echo -------------------------------------------------------

cd /d %~dp0 

del /S *.obj 
del /S *.ilk 
del /S *.pdb 
del /S *.plg 
del /S *.bsc 

del /S *.trc 
del /S *.pch 
del /S *.idb 
del /S *.exp 
del /S *.sbr 
rem  del /S *.res       rem 会误删除c#的 *.resx文件
del /S *.ncb
del /S *.opt
del /S *.SUP
del /S *.aps

del /S *.suo
rem del /S *.manifest   VS2013需要该文件
del /S *.dep
del /S *.sdf
del /S *.tlog
del /S *.log
del /S *.ipch
del /S *.lastbuildstate
rem del /S *.htm
del /S *.user


del /S *.bak
del /S *.o
del /S *.db

del /S /Q *.iobj
del /S /Q *.ipdb
del /S /Q *.recipe

rem 删除子目录：  .vs
set "targetDirectory=.vs" 

for /d /r %%D in (*) do (
    if exist "%%D\%targetDirectory%" (
        echo Deleting "%%D\%targetDirectory%"...
        rd /s /q "%%D\%targetDirectory%"
        echo Subdirectory deleted successfully.
    )
)

set "targetDirectory=ipch" 
for /d /r %%D in (*) do (
    if exist "%%D\%targetDirectory%" (
        echo Deleting "%%D\%targetDirectory%"...
        rd /s /q "%%D\%targetDirectory%"
        echo Subdirectory deleted successfully.
    )
)

@echo off

