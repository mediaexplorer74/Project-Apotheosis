# Fix build.ninja for GNU-compiled files
param([string]$buildFile)

$content = [System.IO.File]::ReadAllText($buildFile)

# Fix MacroAssemblerX86_64 deps - remove PCH
$old1 = 'MacroAssemblerX86_64.cpp.obj: CXX_COMPILER__JavaScriptCore_gnu_Release C$:\Users\Admin\source\repos\!OpenCode\Apotheosis\WebKit\Source\JavaScriptCore\assembler\MacroAssemblerX86_64.cpp | Source\JavaScriptCore\CMakeFiles\JavaScriptCore.dir\cmake_pch.hxx Source\JavaScriptCore\CMakeFiles\JavaScriptCore.dir\cmake_pch.cxx.pch || cmake_object_order_depends_target_JavaScriptCore'
$new1 = 'MacroAssemblerX86_64.cpp.obj: CXX_COMPILER__JavaScriptCore_gnu_Release C$:\Users\Admin\source\repos\!OpenCode\Apotheosis\WebKit\Source\JavaScriptCore\assembler\MacroAssemblerX86_64.cpp || cmake_object_order_depends_target_JavaScriptCore'
$content = $content.Replace($old1, $new1)

# Fix MacroAssemblerX86_64 FLAGS
$old2 = 'FLAGS = -fdiagnostics-color=always -fcolor-diagnostics -Wno-character-conversion -Werror=undefined-internal -Werror=undefined-inline -Wno-noexcept-type -Wno-nullability-completeness -Wno-psabi -Wno-misleading-indentation -Wno-parentheses-equality -Qunused-arguments -Wundef -Wpointer-arith -Wmissing-format-attribute -Wformat-security -Wcast-align -DWIN32 -D_WINDOWS -DUNICODE -D_UNICODE -fno-strict-aliasing -O2 -O2 -DNDEBUG -std=c++23 -MD -O2 /YuC:/Users/Admin/source/repos/!OpenCode/Apotheosis/build-x64-gpu/Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir/cmake_pch.hxx /FpC:/Users/Admin/source/repos/!OpenCode/Apotheosis/build-x64-gpu/Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir/./cmake_pch.cxx.pch /FIC:/Users/Admin/source/repos/!OpenCode/Apotheosis/build-x64-gpu/Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir/cmake_pch.hxx -D_DLL'
$new2 = 'FLAGS = -fdiagnostics-color=always -fcolor-diagnostics -Wno-character-conversion -Werror=undefined-internal -Werror=undefined-inline -Wno-noexcept-type -Wno-nullability-completeness -Wno-psabi -Wno-misleading-indentation -Wno-parentheses-equality -Qunused-arguments -Wundef -Wpointer-arith -Wmissing-format-attribute -Wformat-security -Wcast-align -DWIN32 -D_WINDOWS -DUNICODE -D_UNICODE -fno-strict-aliasing -O2 -DNDEBUG -std=c++23 -D_DLL'
$content = $content.Replace($old2, $new2)

[System.IO.File]::WriteAllText($buildFile, $content, [System.Text.UTF8Encoding]::new($false))
Write-Host "Fixed MacroAssemblerX86_64 PCH deps and flags"
