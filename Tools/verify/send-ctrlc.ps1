# send-ctrlc.ps1 - send Ctrl+C to another console process (verify-server.ps1 smoke stage, 2026-10-21). ASCII only
# GameServer shuts down in order on Ctrl+C (OnConsoleCtrl -> RunShutdown). Stop-Process would skip that path, so the smoke
# stage uses this to test the real shutdown. Run it in its own powershell.exe: attaching to the target's console detaches
# this process from its own.
param([Parameter(Mandatory = $true)] [int] $ProcessId)
$src = @'
using System;
using System.Runtime.InteropServices;
public static class CtrlC {
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
    public static int Send(uint pid) {
        FreeConsole();
        if (!AttachConsole(pid)) return Marshal.GetLastWin32Error();
        SetConsoleCtrlHandler(IntPtr.Zero, true);      // do not kill ourselves
        bool ok = GenerateConsoleCtrlEvent(0, 0);      // CTRL_C_EVENT to every process on that console
        System.Threading.Thread.Sleep(500);
        FreeConsole();
        return ok ? 0 : Marshal.GetLastWin32Error();
    }
}
'@
Add-Type -TypeDefinition $src
exit [CtrlC]::Send([uint32] $ProcessId)
