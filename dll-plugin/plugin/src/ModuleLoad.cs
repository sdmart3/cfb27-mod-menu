// ModuleLoad.cs: load modsettings.dll into the game process ourselves, without a separate injector executable.
//
// Why no helper exe: the Dynasty Hooks package shipped inject.exe, and Windows Defender flags fresh copies of it
// (Behavior:Win32/DefenseEvasion.A!ml). Mod Settings does the same documented work (ported unchanged from the Auto Kick
// plugin) from inside the managed plugin, which is already on disk and scanned. There is nothing here aimed at hiding
// the DLL; every step is logged to plugin.log and the result is verified by reading the target's module list.
//
// The sequence is the textbook LoadLibraryW remote call, used because the game DLL's own DllMain is what wires the
// any-game hooks:
//   1. OpenProcess with the minimal rights the calls below need.
//   2. IsWow64Process: modsettings.dll is 64-bit, so a 32-bit target is refused.
//   3. VirtualAllocEx a small buffer in the target and WriteProcessMemory the UTF-16 DLL path into it.
//   4. kernel32.dll is mapped at the same base in every process of the same bitness on one Windows boot, so the
//      address of LoadLibraryW in THIS process is its address in the target. GetModuleHandle + GetProcAddress here.
//   5. CreateRemoteThread at LoadLibraryW with the buffer as its argument; wait inject_timeout for it.
//   6. GetExitCodeThread (the low 32 bits of the returned HMODULE) tells us LoadLibraryW did not return null.
//   7. VirtualFreeEx the buffer.
//   8. verify modsettings.dll is now in the target's module list (ProcUtil.FindModule).
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace ModSettings
{
    internal static class ModuleLoad
    {
        /// <summary>Loads <paramref name="dll"/> into <paramref name="pid"/>. True only when the module is confirmed
        /// in the target afterwards. Logs each step; sets <paramref name="error"/> on failure.</summary>
        public static bool Load(int pid, string dll, string moduleName, int timeoutMs, out string error)
        {
            error = null;
            IntPtr h = IntPtr.Zero, mem = IntPtr.Zero, thread = IntPtr.Zero;
            try
            {
                h = OpenProcess(ACCESS, false, pid);
                if (h == IntPtr.Zero)
                {
                    int e = Marshal.GetLastWin32Error();
                    error = "OpenProcess failed (Win32 " + e + (e == 5 ? "; access denied - is the game elevated? run the Mod Manager as administrator" : "") + ")";
                    Log.Write("load: " + error); return false;
                }
                Log.Write("load: 1/8 OpenProcess(pid " + pid + ") ok");

                bool wow;
                if (IsWow64Process(h, out wow) && wow) { error = "the game process is 32-bit but " + moduleName + " is 64-bit"; Log.Write("load: " + error); return false; }
                Log.Write("load: 2/8 target is 64-bit");

                byte[] path = Encoding.Unicode.GetBytes(dll + "\0");
                mem = VirtualAllocEx(h, IntPtr.Zero, new UIntPtr((uint)path.Length), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (mem == IntPtr.Zero) { error = "VirtualAllocEx failed (Win32 " + Marshal.GetLastWin32Error() + ")"; Log.Write("load: " + error); return false; }
                Log.Write("load: 3/8 VirtualAllocEx " + path.Length + " bytes at 0x" + mem.ToInt64().ToString("x"));

                UIntPtr written;
                if (!WriteProcessMemory(h, mem, path, (uint)path.Length, out written) || written.ToUInt64() != (ulong)path.Length)
                { error = "WriteProcessMemory wrote " + written + "/" + path.Length + " bytes (Win32 " + Marshal.GetLastWin32Error() + ")"; Log.Write("load: " + error); return false; }
                Log.Write("load: 4/8 wrote the DLL path into the target");

                IntPtr k32 = GetModuleHandleW("kernel32.dll");
                IntPtr loadLib = k32 != IntPtr.Zero ? GetProcAddress(k32, "LoadLibraryW") : IntPtr.Zero;
                if (loadLib == IntPtr.Zero) { error = "could not resolve LoadLibraryW (Win32 " + Marshal.GetLastWin32Error() + ")"; Log.Write("load: " + error); return false; }
                Log.Write("load: 5/8 LoadLibraryW at 0x" + loadLib.ToInt64().ToString("x"));

                thread = CreateRemoteThread(h, IntPtr.Zero, UIntPtr.Zero, loadLib, mem, 0, IntPtr.Zero);
                if (thread == IntPtr.Zero) { error = "CreateRemoteThread failed (Win32 " + Marshal.GetLastWin32Error() + ")"; Log.Write("load: " + error); return false; }
                Log.Write("load: 6/8 CreateRemoteThread started; waiting up to " + (timeoutMs / 1000) + " s");

                uint wait = WaitForSingleObject(thread, (uint)timeoutMs);
                if (wait != 0) { error = wait == 0x102 ? "the load thread did not finish in " + (timeoutMs / 1000) + " s" : "WaitForSingleObject returned 0x" + wait.ToString("x"); Log.Write("load: " + error); return false; }
                uint exit; GetExitCodeThread(thread, out exit);
                Log.Write("load: 7/8 load thread finished; LoadLibraryW returned 0x" + exit.ToString("x") + " (low 32 bits of the module handle)");

                string loaded = ProcUtil.FindModule(pid, moduleName);
                if (loaded == null)
                {
                    if (exit == 0) error = "LoadLibraryW returned null - the DLL did not load (see " + Names.Stem + ".log; a missing dependency or a blocked file)";
                    else error = "the load thread succeeded but " + moduleName + " is not in the module list yet (it may still be initialising)";
                    Log.Write("load: " + error); return false;
                }
                Log.Write("load: 8/8 verified " + moduleName + " in pid " + pid + " at " + loaded);
                return true;
            }
            catch (Exception ex) { error = ex.GetType().Name + ": " + ex.Message; Log.Write("load: error " + error); return false; }
            finally
            {
                try { if (mem != IntPtr.Zero && h != IntPtr.Zero) VirtualFreeEx(h, mem, UIntPtr.Zero, MEM_RELEASE); } catch { }
                try { if (thread != IntPtr.Zero) CloseHandle(thread); } catch { }
                try { if (h != IntPtr.Zero) CloseHandle(h); } catch { }
            }
        }

        // the minimal rights for VirtualAllocEx + WriteProcessMemory + CreateRemoteThread + the query calls
        private const uint ACCESS = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
        private const uint PROCESS_CREATE_THREAD = 0x0002, PROCESS_QUERY_INFORMATION = 0x0400, PROCESS_VM_OPERATION = 0x0008, PROCESS_VM_WRITE = 0x0020, PROCESS_VM_READ = 0x0010;
        private const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000, PAGE_READWRITE = 0x04;

        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool IsWow64Process(IntPtr h, out bool wow);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, UIntPtr size, uint type, uint protect);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool VirtualFreeEx(IntPtr h, IntPtr addr, UIntPtr size, uint type);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, uint size, out UIntPtr written);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string name);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)] private static extern IntPtr GetProcAddress(IntPtr mod, string name);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr attrs, UIntPtr stack, IntPtr start, IntPtr arg, uint flags, IntPtr tid);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern uint WaitForSingleObject(IntPtr h, uint ms);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetExitCodeThread(IntPtr h, out uint code);
    }
}
