// ProcUtil.cs: read-only process inspection the loader needs - is a PID alive, its image path, its module list,
// is it 64-bit. All of it swallows errors and returns a safe default. This is the same Toolhelp / OpenProcess
// code the Dynasty Hooks, Call Sheet and Auto Kick plugins use.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace ModSettings
{
    /// <summary>One module in a process (from a Toolhelp snapshot): file name, full path, load address.</summary>
    internal struct ModInfo { public string Name; public string Path; public IntPtr Base; }

    internal static class ProcUtil
    {
        public static Process[] ByName(string name)
        {
            try { return Process.GetProcessesByName(name); } catch { return new Process[0]; }
        }

        /// <summary>The PIDs of every process with this image name right now.</summary>
        public static List<int> PidsByName(string name)
        {
            List<int> ids = new List<int>();
            foreach (Process p in ByName(name)) { ids.Add(p.Id); p.Dispose(); }
            return ids;
        }

        public static bool Alive(int pid)
        {
            IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (h == IntPtr.Zero) return Marshal.GetLastWin32Error() == 5;   // access denied means it exists
            try { uint code; return GetExitCodeProcess(h, out code) && code == 259; } finally { CloseHandle(h); }
        }

        public static string ImagePath(int pid)
        {
            IntPtr h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (h == IntPtr.Zero) return null;
            try
            {
                StringBuilder sb = new StringBuilder(1024); int n = sb.Capacity;
                return QueryFullProcessImageName(h, 0, sb, ref n) ? sb.ToString(0, n) : null;
            }
            finally { CloseHandle(h); }
        }

        /// <summary>Every module in the process, or an empty list when the snapshot cannot be taken.</summary>
        public static List<ModInfo> Modules(int pid)
        {
            List<ModInfo> list = new List<ModInfo>();
            IntPtr snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, (uint)pid);
            if (snap == INVALID_HANDLE_VALUE) return list;
            try
            {
                MODULEENTRY32W me = new MODULEENTRY32W(); me.dwSize = (uint)Marshal.SizeOf(typeof(MODULEENTRY32W));
                if (!Module32FirstW(snap, ref me)) return list;
                do { list.Add(new ModInfo { Name = me.szModule, Path = me.szExePath, Base = me.modBaseAddr }); }
                while (Module32NextW(snap, ref me));
            }
            catch { }
            finally { CloseHandle(snap); }
            return list;
        }

        /// <summary>Full path of a module with this file name already in the process, else null.</summary>
        public static string FindModule(int pid, string moduleName)
        {
            foreach (ModInfo m in Modules(pid))
                if (string.Equals(m.Name, moduleName, StringComparison.OrdinalIgnoreCase)) return m.Path;
            return null;
        }

        /// <summary>Full path of the autoprogress.dll hook, or a Dynasty Hooks version.dll proxy, already in the
        /// process, else null. The proxy is the same binary as autoprogress.dll, so it carries the ini name.</summary>
        public static string FindDynastyHooksModule(int pid, string gameDir)
        {
            foreach (ModInfo m in Modules(pid))
            {
                if (string.Equals(m.Name, "autoprogress.dll", StringComparison.OrdinalIgnoreCase)) return m.Path;
                if (string.Equals(m.Name, "version.dll", StringComparison.OrdinalIgnoreCase) && IsDynastyHooksProxy(m.Path))
                {
                    if (gameDir == null) return m.Path;
                    if (string.Equals(Path.GetDirectoryName(m.Path), gameDir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return m.Path;
                }
            }
            return null;
        }

        /// <summary>A version.dll that is really the Dynasty Hooks proxy carries the string "autoprogress.ini".</summary>
        public static bool IsDynastyHooksProxy(string path)
        {
            try
            {
                byte[] b = File.ReadAllBytes(path);
                byte[] needle = Encoding.ASCII.GetBytes("autoprogress.ini");
                for (int i = 0; i + needle.Length <= b.Length; i++)
                {
                    int j = 0; while (j < needle.Length && b[i + j] == needle[j]) j++;
                    if (j == needle.Length) return true;
                }
            }
            catch { }
            return false;
        }

        // ---- Win32
        public const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        private const uint TH32CS_SNAPMODULE = 0x8, TH32CS_SNAPMODULE32 = 0x10;
        private static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct MODULEENTRY32W
        {
            public uint dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
            public IntPtr modBaseAddr;
            public uint modBaseSize;
            public IntPtr hModule;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szModule;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExePath;
        }

        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetExitCodeProcess(IntPtr h, out uint code);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool QueryFullProcessImageName(IntPtr h, uint flags, StringBuilder name, ref int size);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool Module32FirstW(IntPtr snap, ref MODULEENTRY32W me);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool Module32NextW(IntPtr snap, ref MODULEENTRY32W me);
    }
}
