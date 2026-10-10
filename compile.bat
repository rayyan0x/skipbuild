@echo off
call "F:\VS_BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /std:c++20 /I"C:\Users\wasey\OneDrive\Desktop\build C++\cache" /I"F:\vcpkg\buildtrees\openssl\src\nssl-3.6.5-c2a478abe6.clean\include" /c "C:\Users\wasey\OneDrive\Desktop\build C++\cache\core.cpp" /Fo"core.obj"
cl /std:c++20 /I"C:\Users\wasey\OneDrive\Desktop\build C++\cache" /I"F:\vcpkg\buildtrees\openssl\src\nssl-3.6.5-c2a478abe6.clean\include" /c "C:\Users\wasey\OneDrive\Desktop\build C++\cli\main.cpp" /Fo"main.obj"
link /OUT:remake.exe core.obj main.obj "F:\vcpkg\buildtrees\openssl\x64-windows-rel\libcrypto.lib" "F:\vcpkg\buildtrees\openssl\x64-windows-rel\libssl.lib" /LIBPATH:"F:\vcpkg\buildtrees\openssl\x64-windows-rel"