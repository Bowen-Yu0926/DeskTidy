# 独立脱离终端 Job Object，将 DeskTidy 拉起至当前交互桌面会话
$args = @{
    CommandLine = '"E:\deskTidy\native\build\Release\desktidy_native.exe"'
    CurrentDirectory = 'E:\deskTidy\native\build\Release'
}
$res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments $args
Write-Host "DeskTidy started with PID: $($res.ProcessId)"
