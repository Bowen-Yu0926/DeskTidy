$procArgs = @{
    CommandLine = '"E:\deskTidy\native\build\Release\desktidy_native.exe"'
    CurrentDirectory = 'E:\deskTidy\native\build\Release'
}
$res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments $procArgs
Write-Host "DeskTidy started with PID: $($res.ProcessId)"
