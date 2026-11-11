// ModSettingsPlugin.dll: the Mod Manager (MMC 1.1.0.6, Frosty-based) plugin that loads Mod Settings (modsettings.dll: per-mod settings
// from manifests, values.json, and the shared settings band of the loaded dynasty) into College Football 27. A copy of the Weekly
// Strategy Data plugin (itself the Press Box plugin) with the names changed (namespace ModSettings, Plugins\ModSettings, modsettings.dll /
// modsettings-status.json) and one stand-aside key ("modsettings", which no other tool has). 2026-11-07.
//
// This file holds the plugin manifest, settings, log, the Dynasty Hooks ini reader and the modsettings-status.json writer.
// The loader (the LaunchAction class) is in Loader.cs; the module loader helper is in ModuleLoad.cs.
//
// Written for the .NET Framework 4.x csc (C# 5): no ?. , no $"" , no expression-bodied members.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Frosty.Core.Attributes;
using FrostySdk;

[assembly: System.Reflection.AssemblyTitle(ModSettings.Names.Product)]
[assembly: System.Reflection.AssemblyVersion("0.1.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("0.1.0.0")]
[assembly: PluginDisplayName(ModSettings.Names.Product)]
[assembly: PluginAuthor(ModSettings.Names.Product)]
[assembly: PluginVersion("0.1.0")]
[assembly: PluginValidForProfile((int)ProfileVersion.CollegeFootball27)]
[assembly: RegisterExecutionAction(typeof(ModSettings.LaunchAction))]

namespace ModSettings
{
    /// <summary>THE NAME, in one place for the plugin ("Mod Settings"). The DLL's own
    /// copy is dll\src\product.h. The namespace / assembly file name (ModSettings / ModSettingsPlugin.dll) are the only other spots.</summary>
    internal static class Names
    {
        public const string Product = "Mod Settings";            // what a user reads
        public const string Stem = "modsettings";                // modsettings.dll / modsettings-status.json / "product" in the status
        public const string Folder = "ModSettings";              // Plugins\ModSettings
        public const string Dll = Stem + ".dll";
        public const string StatusFile = Stem + "-status.json";
    }

    /// <summary>plugin.cfg: key = value lines, ';' or '#' comments. Unknown keys are ignored. Missing file = defaults
    /// (the release ships a plugin.cfg holding exactly these defaults; the installer never replaces it).
    /// Mod Settings's own settings live in modsettings.ini and are read by the DLL, never by this loader.</summary>
    internal sealed class Settings
    {
        // The Dynasty Hooks keys that run Dynasty Hooks' own copy of this feature (its src/modsettings.h, byte-identical to the
        // vendored one: the same SeasonGameFlow handler calls and every other site). With any of them on, Dynasty Hooks owns
        // the any-game feature in that game and Mod Settings stays out (docs/SPEC.md "Stand-aside"; the DLL applies the same rule).
        // Its test switches (none) do nothing with these four off.
        public static readonly string[] DefaultStandAsideKeys = { "modsettings" };   // Dynasty Hooks has no copy of this feature: no key of its ever turns this on

        public bool Enabled = true;
        public int SettleSec = 30;
        public int WaitGameSec = 180;
        public bool RespectDynastyHooks = true;
        // advanced
        public int PollMs = 1000;
        public string GameExe = "CollegeFB27.exe";
        public string DllName = Names.Dll;
        public bool RequireNewProcess = true;
        public int InjectTimeoutSec = 15;
        public int StableSec = 10;                // the game process set must hold still this long before the load
        public int ResolveSec = 120;              // give up waiting for it to hold still after this long, then pick the biggest
        public string GameDir = "";               // "" = any folder; else only a game whose image is in this folder
        public string[] StandAsideKeys = DefaultStandAsideKeys;

        public static Settings Load(string path, List<string> notes)
        {
            Settings s = new Settings();
            if (!File.Exists(path)) { notes.Add("plugin.cfg not found; using the defaults"); return s; }
            string[] lines;
            try { lines = File.ReadAllLines(path); }
            catch (Exception ex) { notes.Add("plugin.cfg not readable (" + ex.GetType().Name + "); using the defaults"); return s; }
            foreach (string raw in lines)
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string k = line.Substring(0, eq).Trim().ToLowerInvariant();
                string v = line.Substring(eq + 1).Trim();
                int sc = v.IndexOf(';'); if (sc >= 0) v = v.Substring(0, sc).Trim();
                int n;
                switch (k)
                {
                    case "plugin_enabled": s.Enabled = !IsOff(v); break;
                    case "settle_sec": if (int.TryParse(v, out n) && n >= 0) s.SettleSec = Math.Min(n, 600); else notes.Add("plugin.cfg: settle_sec '" + v + "' ignored"); break;
                    case "wait_game_sec": if (int.TryParse(v, out n) && n > 0) s.WaitGameSec = Math.Min(n, 1800); else notes.Add("plugin.cfg: wait_game_sec '" + v + "' ignored"); break;
                    case "respect_other_tool":
                    case "respect_dynasty_hooks": s.RespectDynastyHooks = !IsOff(v); break;   // the second name: the older plugins' key
                    case "poll_ms": if (int.TryParse(v, out n) && n >= 100) s.PollMs = Math.Min(n, 10000); break;
                    case "game_exe": if (v.Length > 0) s.GameExe = v; break;
                    case "dll": if (v.Length > 0) s.DllName = v; break;
                    case "require_new_process": s.RequireNewProcess = !IsOff(v); break;
                    case "inject_timeout_sec": if (int.TryParse(v, out n) && n > 0) s.InjectTimeoutSec = Math.Min(n, 120); break;
                    case "stable_sec": if (int.TryParse(v, out n) && n >= 0) s.StableSec = Math.Min(n, 120); break;
                    case "resolve_sec": if (int.TryParse(v, out n) && n >= 0) s.ResolveSec = Math.Min(n, 600); break;
                    case "game_dir": s.GameDir = v.Trim().Trim('"').Trim(); break;
                    case "stand_aside_keys":
                        {
                            List<string> ks = new List<string>();
                            foreach (string p in v.Split(new[] { ',', ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries)) ks.Add(p.ToLowerInvariant());
                            if (ks.Count > 0) s.StandAsideKeys = ks.ToArray();
                            break;
                        }
                }
            }
            return s;
        }

        private static bool IsOff(string v)
        {
            return v == "0" || v.Equals("false", StringComparison.OrdinalIgnoreCase) || v.Equals("off", StringComparison.OrdinalIgnoreCase) || v.Equals("no", StringComparison.OrdinalIgnoreCase);
        }

        public string ProcName { get { return Path.GetFileNameWithoutExtension(GameExe); } }
    }

    /// <summary>Plain text log in Plugins\ModSettings. Never throws. Rotated to plugin.log.1 past 1 MB.</summary>
    internal static class Log
    {
        private static readonly object Gate = new object();
        public static string FilePath;

        public static void Write(string msg)
        {
            try
            {
                string line = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + msg + Environment.NewLine;
                lock (Gate)
                {
                    if (FilePath == null) return;
                    try
                    {
                        FileInfo fi = new FileInfo(FilePath);
                        if (fi.Exists && fi.Length > 1024 * 1024)
                        {
                            string old = FilePath + ".1";
                            if (File.Exists(old)) File.Delete(old);
                            File.Move(FilePath, old);
                        }
                    }
                    catch { }
                    File.AppendAllText(FilePath, line, Encoding.UTF8);
                }
            }
            catch { }
        }
    }

    /// <summary>Reads autoprogress.ini EXACTLY the way autoprogress.dll's read_config() does: fopen "r" (text mode:
    /// CRLF -> LF, Ctrl+Z ends the file), fgets into a 256-byte buffer (so a line longer than 255 bytes is read as
    /// several pieces), skip leading spaces/tabs, skip ';' '#' and empty pieces, cut at the first ';', split at the
    /// first '=', trim, _stricmp the key, atoi the value. Later occurrences overwrite earlier ones: the LAST wins.</summary>
    internal static class DhIni
    {
        public static Dictionary<string, string> Read(string path)
        {
            Dictionary<string, string> d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            byte[] raw = File.ReadAllBytes(path);
            List<byte> text = new List<byte>(raw.Length);
            for (int i = 0; i < raw.Length; i++)
            {
                byte b = raw[i];
                if (b == 0x1A) break;                                            // msvcrt text mode: Ctrl+Z = end of file
                if (b == (byte)'\r' && i + 1 < raw.Length && raw[i + 1] == (byte)'\n') continue;
                text.Add(b);
            }
            int pos = 0;
            while (pos < text.Count)
            {
                int start = pos, len = 0;
                while (pos < text.Count && len < 255) { byte b = text[pos++]; len++; if (b == (byte)'\n') break; }
                Piece(text, start, len, d);
            }
            return d;
        }

        private static void Piece(List<byte> t, int start, int len, Dictionary<string, string> d)
        {
            char[] c = new char[len];
            for (int i = 0; i < len; i++) c[i] = (char)t[start + i];   // bytes as Latin-1: keys are ASCII, values only atoi'd
            string s = new string(c);
            int k = 0; while (k < s.Length && (s[k] == ' ' || s[k] == '\t')) k++;
            s = s.Substring(k);
            if (s.Length == 0 || s[0] == ';' || s[0] == '#' || s[0] == '\n') return;
            int sc = s.IndexOf(';'); if (sc >= 0) s = s.Substring(0, sc);
            int eq = s.IndexOf('='); if (eq < 0) return;
            string key = s.Substring(0, eq).TrimEnd(' ', '\t', '\n', '\r');
            string v = s.Substring(eq + 1).TrimStart(' ', '\t').TrimEnd(' ', '\t', '\n', '\r');
            d[key] = v;
        }

        /// <summary>C atoi: leading white space, optional sign, digits; anything else stops it; no digits = 0.</summary>
        public static long AtoI(string v)
        {
            int i = 0; while (i < v.Length && (v[i] == ' ' || (v[i] >= '\t' && v[i] <= '\r'))) i++;
            bool neg = false;
            if (i < v.Length && (v[i] == '+' || v[i] == '-')) { neg = v[i] == '-'; i++; }
            long n = 0;
            while (i < v.Length && v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i] - '0'); if (n > int.MaxValue) n = int.MaxValue; i++; }
            return neg ? -n : n;
        }
    }

    /// <summary>Whether Dynasty Hooks is set to run its own copy of the any-game feature, from one autoprogress.ini.</summary>
    internal sealed class DhVerdict
    {
        public string Source;          // "plugin" | "loaded module"
        public string HooksDir;
        public string IniPath;
        public bool IniFound;
        public List<string> KeysOn = new List<string>();
        public string Note;            // how the folder was found
        public bool FeatureOn { get { return KeysOn.Count > 0; } }

        public string Describe()
        {
            return "the other tool (" + Source + ") folder " + HooksDir + (Note != null ? " [" + Note + "]" : "") + ": " +
                   (!IniFound ? "no autoprogress.ini there (it runs on its defaults: its own copy of this feature is off)" :
                    FeatureOn ? "its own copy of this feature is ON (" + string.Join(", ", KeysOn.ToArray()) + ")" : "its own copy of this feature is off");
        }

        public static DhVerdict FromIni(string source, string dir, string note, string[] keys)
        {
            DhVerdict v = new DhVerdict();
            v.Source = source; v.HooksDir = dir; v.Note = note;
            v.IniPath = Path.Combine(dir, "autoprogress.ini");
            v.IniFound = File.Exists(v.IniPath);
            if (!v.IniFound) return v;
            Dictionary<string, string> ini;
            try { ini = DhIni.Read(v.IniPath); }
            catch (Exception ex) { v.Note = (note != null ? note + "; " : "") + "ini not readable: " + ex.GetType().Name; v.IniFound = false; return v; }
            foreach (string k in keys)
            {
                string val;
                if (ini.TryGetValue(k, out val) && DhIni.AtoI(val) != 0) v.KeysOn.Add(k + "=" + val);
            }
            return v;
        }

        /// <summary>Plugins\DynastyHooksPlugin.dll present -> the verdict for its hooks folder; absent -> null.</summary>
        public static DhVerdict FromPlugin(string pluginsDir, string[] keys)
        {
            if (!File.Exists(Path.Combine(pluginsDir, "DynastyHooksPlugin.dll"))) return null;
            string dhPluginDir = Path.Combine(pluginsDir, "DynastyHooks");
            string cfg = Path.Combine(dhPluginDir, "plugin.cfg");
            string hooksDir = ReadHooksDir(cfg);
            string dir, note;
            if (string.IsNullOrEmpty(hooksDir)) { dir = dhPluginDir; note = File.Exists(cfg) ? "plugin.cfg has no hooks_dir" : "no plugin.cfg"; }
            else
            {
                try
                {
                    string d = Environment.ExpandEnvironmentVariables(hooksDir);
                    if (!Path.IsPathRooted(d)) d = Path.Combine(dhPluginDir, d);
                    dir = Path.GetFullPath(d).TrimEnd('\\', '/');
                    note = "hooks_dir = " + hooksDir;
                    if (!Directory.Exists(dir)) note += "; that folder does not exist, so that plugin loads nothing";
                }
                catch (Exception ex) { return Unusable(dhPluginDir, "hooks_dir '" + hooksDir + "' is not a usable path (" + ex.GetType().Name + "), so that plugin loads nothing"); }
            }
            return FromIni("plugin", dir, note, keys);
        }

        private static DhVerdict Unusable(string dir, string note)
        {
            DhVerdict v = new DhVerdict(); v.Source = "plugin"; v.HooksDir = dir; v.Note = note; v.IniPath = null; v.IniFound = false; return v;
        }

        /// <summary>The Dynasty Hooks plugin's own reading of hooks_dir: last occurrence wins, ';' comment cut, quotes trimmed.</summary>
        private static string ReadHooksDir(string cfg)
        {
            if (!File.Exists(cfg)) return null;
            string result = null;
            foreach (string raw in File.ReadAllLines(cfg))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == ';' || line[0] == '#') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                if (!line.Substring(0, eq).Trim().Equals("hooks_dir", StringComparison.OrdinalIgnoreCase)) continue;
                string v = line.Substring(eq + 1).Trim();
                int sc = v.IndexOf(';'); if (sc >= 0) v = v.Substring(0, sc).Trim();
                result = v.Trim().Trim('"').Trim();
            }
            return result;
        }
    }

    /// <summary>modsettings-status.json, written by the plugin ONLY when the DLL never gets to write it: the plugin stood
    /// aside, is disabled, or failed before the DLL loaded. Same file the DLL uses, with "source":"plugin" and the
    /// plugin's flat shape (state / reason at the top level). Atomic: tmp + MoveFileEx.</summary>
    internal static class Status
    {
        public const string FileName = Names.StatusFile;

        public static void Write(string dir, int pid, string gameExePath, string state, string reason, DhVerdict dh, string error)
        {
            try
            {
                StringBuilder j = new StringBuilder();
                j.Append("{\"product\":\"" + Names.Stem + "\",\"source\":\"plugin\",\"plugin_version\":\"").Append(LaunchAction.Version).Append('"');
                j.Append(",\"pid\":").Append(pid);
                string gv = GameVersion(gameExePath);
                j.Append(",\"game_version\":").Append(gv == null ? "null" : Q(gv));
                j.Append(",\"updated\":").Append(Q(DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'")));
                j.Append(",\"state\":").Append(Q(state));
                string why = reason ?? error;
                j.Append(",\"reason\":").Append(why == null ? "null" : Q(why));
                if (dh != null)
                {
                    j.Append(",\"other_tool\":{\"source\":").Append(Q(dh.Source)).Append(",\"hooks_dir\":").Append(Q(dh.HooksDir ?? ""));
                    j.Append(",\"ini\":").Append(dh.IniFound ? Q(dh.IniPath) : "null").Append(",\"keys_on\":[");
                    for (int i = 0; i < dh.KeysOn.Count; i++) { if (i > 0) j.Append(','); j.Append(Q(dh.KeysOn[i])); }
                    j.Append("]}");
                }
                j.Append("}\n");
                string target = Path.Combine(dir, FileName), tmp = target + ".plugin.tmp";
                File.WriteAllText(tmp, j.ToString(), new UTF8Encoding(false));
                if (!MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    int e = Marshal.GetLastWin32Error();
                    try { File.Delete(tmp); } catch { }
                    Log.Write(FileName + ": could not replace it (Win32 error " + e + ")");
                    return;
                }
                Log.Write(FileName + ": state " + state + (why != null ? " (" + why + ")" : ""));
            }
            catch (Exception ex) { Log.Write(FileName + ": not written (" + ex.GetType().Name + ": " + ex.Message + ")"); }
        }

        /// <summary>The game exe's PE TimeDateStamp as 8 lowercase hex digits, else null.</summary>
        internal static string GameVersion(string exe)
        {
            if (string.IsNullOrEmpty(exe)) return null;
            try
            {
                using (FileStream fs = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (BinaryReader br = new BinaryReader(fs))
                {
                    if (br.ReadUInt16() != 0x5A4D) return null;
                    fs.Position = 0x3C; uint lfanew = br.ReadUInt32();
                    if (lfanew > fs.Length - 12) return null;
                    fs.Position = lfanew; if (br.ReadUInt32() != 0x00004550) return null;
                    fs.Position = lfanew + 8;
                    return br.ReadUInt32().ToString("x8");
                }
            }
            catch { return null; }
        }

        private static string Q(string s)
        {
            StringBuilder b = new StringBuilder("\"");
            foreach (char c in s)
            {
                if (c == '"') b.Append("\\\""); else if (c == '\\') b.Append("\\\\");
                else if (c == '\n') b.Append("\\n"); else if (c == '\r') b.Append("\\r"); else if (c == '\t') b.Append("\\t");
                else if (c < 0x20) b.Append("\\u").Append(((int)c).ToString("x4"));
                else b.Append(c);
            }
            return b.Append('"').ToString();
        }

        private const uint MOVEFILE_REPLACE_EXISTING = 0x1, MOVEFILE_WRITE_THROUGH = 0x8;
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool MoveFileExW(string from, string to, uint flags);
    }
}
