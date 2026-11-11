// Loader.cs: the ExecutionAction the Mod Manager calls. Pre records running games; Post starts a bounded,
// foreground watcher that waits for the new game, settles, decides whether to stand aside, and otherwise loads
// modsettings.dll. Post returns in milliseconds; the Launching window is never held open.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;
using Frosty.Core;
using FrostySdk;
using FrostySdk.Interfaces;

namespace ModSettings
{
    public sealed class LaunchAction : ExecutionAction
    {
        public const string Version = "0.2.0";

        private static readonly object StateGate = new object();
        private static HashSet<int> s_preexisting = new HashSet<int>();
        private static string s_gameDir;
        private static bool s_preRan;
        private static int s_watcherBusy;                                  // 1 while a watcher thread runs
        private static readonly HashSet<int> s_handled = new HashSet<int>(); // PIDs this plugin already acted on

        /// <summary>Test seam: the watcher thread of the latest Post (null when none was started).</summary>
        public static Thread LastWatcher;

        public override Action<ILogger, PluginManagerType, CancellationToken> PreLaunchAction { get { return Pre; } }
        public override Action<ILogger, PluginManagerType, CancellationToken> PostLaunchAction { get { return Post; } }

        /// <summary>Plugins\ModSettings, beside Plugins\ModSettingsPlugin.dll.</summary>
        internal static string PluginDir()
        {
            string asm = typeof(LaunchAction).Assembly.Location;
            return Path.Combine(Path.GetDirectoryName(asm), Names.Folder);
        }

        /// <summary>Plugins (the parent of Plugins\ModSettings): where DynastyHooksPlugin.dll would be.</summary>
        internal static string PluginsDir()
        {
            string asm = typeof(LaunchAction).Assembly.Location;
            return Path.GetDirectoryName(asm);
        }

        private static void Pre(ILogger logger, PluginManagerType type, CancellationToken token)
        {
            try
            {
                string dir = PluginDir();
                Log.FilePath = Path.Combine(dir, "plugin.log");
                if (!Directory.Exists(dir)) return;                            // not installed properly: stay silent
                List<string> notes = new List<string>();
                Settings cfg = Settings.Load(Path.Combine(dir, "plugin.cfg"), notes);
                Log.Write("---- launch from the Mod Manager (" + Names.Product + " plugin " + Version + ") ----");
                foreach (string n in notes) Log.Write(n);
                HashSet<int> pre = new HashSet<int>(ProcUtil.PidsByName(cfg.ProcName));
                string gameDir = TryGameDir();
                lock (StateGate) { s_preexisting = pre; s_gameDir = gameDir; s_preRan = true; }
                Log.Write("pre-launch: profile=" + SafeProfileName() + " game folder=" + (gameDir ?? "(unknown)") +
                          " already running " + cfg.GameExe + ": " + (pre.Count == 0 ? "none" : string.Join(",", new List<int>(pre).ToArray())));
            }
            catch (Exception ex) { Log.Write("pre-launch: error ignored: " + ex.GetType().Name + ": " + ex.Message); }
        }

        private static void Post(ILogger logger, PluginManagerType type, CancellationToken token)
        {
            LastWatcher = null;
            try
            {
                string dir = PluginDir();
                Log.FilePath = Path.Combine(dir, "plugin.log");
                if (!Directory.Exists(dir)) return;
                List<string> notes = new List<string>();
                Settings cfg = Settings.Load(Path.Combine(dir, "plugin.cfg"), notes);
                foreach (string n in notes) Log.Write(n);
                if (!cfg.Enabled) { Log.Write("post-launch: plugin_enabled = 0; " + Names.Product + " will not load"); Status.Write(dir, 0, null, "disabled", "plugin_enabled = 0 in plugin.cfg", null, null); return; }
                if (token.IsCancellationRequested) { Log.Write("post-launch: the launch was cancelled; nothing to do"); return; }
                string profile = SafeProfileName();
                if (profile != null && !profile.Equals("CollegeFB27", StringComparison.OrdinalIgnoreCase))
                { Log.Write("post-launch: profile " + profile + " is not CollegeFB27; nothing to do"); return; }

                string dll = Path.Combine(dir, cfg.DllName);
                if (!File.Exists(dll)) { Log.Write("post-launch: " + dll + " is missing; " + Names.Product + " cannot load"); Status.Write(dir, 0, null, "failed", null, null, cfg.DllName + " is missing from Plugins\\ModSettings (run Repair in the Mod Settings app)"); return; }
                Log.Write("post-launch: DLL = " + dll + " " + FileStamp(dll));
                Log.Write("post-launch: settle " + cfg.SettleSec + " s, wait-for-game " + cfg.WaitGameSec + " s, respect the other tool = " + (cfg.RespectDynastyHooks ? "yes" : "no (advanced override)"));

                if (Interlocked.CompareExchange(ref s_watcherBusy, 1, 0) != 0)
                { Log.Write("post-launch: a watcher from an earlier launch is still running; not starting a second one"); return; }

                HashSet<int> pre; string gameDir; bool preRan;
                lock (StateGate) { pre = new HashSet<int>(s_preexisting); gameDir = s_gameDir; preRan = s_preRan; s_preRan = false; }
                if (!preRan) Log.Write("post-launch: pre-launch did not run for this launch; any running game process counts as new");

                Thread t = new Thread(delegate() { Watch(cfg, dir, dll, pre, gameDir); });
                t.Name = Names.Product + " loader";
                t.IsBackground = false;   // keeps the manager process alive until the load is done or timed out
                LastWatcher = t;
                t.Start();
                Log.Write("post-launch: watching for " + cfg.GameExe);
                try { if (logger != null) logger.Log(Names.Product + ": loads about " + cfg.SettleSec + " s after the game starts (Plugins\\" + Names.Folder + "\\plugin.log)"); } catch { }
            }
            catch (Exception ex)
            {
                Interlocked.Exchange(ref s_watcherBusy, 0);
                Log.Write("post-launch: error ignored: " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        private static void Watch(Settings cfg, string dir, string dll, HashSet<int> pre, string gameDir)
        {
            try { WatchInner(cfg, dir, dll, pre, gameDir); }
            catch (Exception ex) { Log.Write("watcher: error ignored: " + ex.GetType().Name + ": " + ex.Message); }
            finally { Interlocked.Exchange(ref s_watcherBusy, 0); }
        }

        private static void WatchInner(Settings cfg, string dir, string dll, HashSet<int> pre, string gameDir)
        {
            Stopwatch sw = Stopwatch.StartNew();
            long deadline = (long)cfg.WaitGameSec * 1000;
            HashSet<int> gone = new HashSet<int>();
            while (true)
            {
                int pid = FindNewPid(cfg, pre, gone);
                if (pid == 0)
                {
                    if (sw.ElapsedMilliseconds > deadline)
                    {
                        Log.Write("gave up: no new " + cfg.GameExe + " appeared within " + cfg.WaitGameSec + " s; " + Names.Product + " was NOT loaded");
                        Status.Write(dir, 0, null, "failed", null, null, "no new " + cfg.GameExe + " appeared within " + cfg.WaitGameSec + " s");
                        return;
                    }
                    Thread.Sleep(cfg.PollMs);
                    continue;
                }
                string exePath = ProcUtil.ImagePath(pid);
                if (cfg.GameDir.Length > 0 && exePath != null && !string.Equals(Path.GetDirectoryName(exePath), cfg.GameDir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                {
                    Log.Write("skip pid " + pid + ": its folder " + Path.GetDirectoryName(exePath) + " is not game_dir " + cfg.GameDir);
                    gone.Add(pid); continue;
                }
                Log.Write("found " + cfg.GameExe + " pid " + pid + " after " + (sw.ElapsedMilliseconds / 1000) + " s (" + (exePath ?? "path unknown") + "); settling " + cfg.SettleSec + " s");
                bool died = false;
                for (int s = 0; s < cfg.SettleSec * 10; s++) { Thread.Sleep(100); if (!ProcUtil.Alive(pid)) { died = true; break; } }
                if (died || !ProcUtil.Alive(pid))
                {
                    gone.Add(pid);
                    Log.Write("pid " + pid + " exited during the settle period (a launcher relaunch?); waiting for the next one");
                    if (sw.ElapsedMilliseconds > deadline) { Log.Write("gave up: timeout reached after pid " + pid + " exited; " + Names.Product + " was NOT loaded"); Status.Write(dir, 0, null, "failed", null, null, "the game process exited during the settle period and the timeout was reached"); return; }
                    continue;
                }
                int target = ResolveTarget(cfg, pre, pid);
                if (target == 0) { gone.Add(pid); continue; }                  // every candidate exited: wait for the next one
                if (target != pid) exePath = ProcUtil.ImagePath(target);
                Decide(cfg, dir, dll, target, exePath, gameDir);
                return;
            }
        }

        /// <summary>The live game processes this launch started (pre-existing stale ones are not counted).</summary>
        private static List<int> LiveCandidates(Settings cfg, HashSet<int> pre)
        {
            List<int> r = new List<int>();
            foreach (int id in ProcUtil.PidsByName(cfg.ProcName))
            {
                if (cfg.RequireNewProcess && pre.Contains(id)) continue;
                if (ProcUtil.Alive(id)) r.Add(id);
            }
            r.Sort();
            return r;
        }

        /// <summary>The game restarts itself at startup, so the process set can hold two for a while. Wait until it holds
        /// still for stable_sec; if it never does within resolve_sec, take the biggest process (the real game, not a
        /// launcher stub). Returns 0 when nothing is left alive.</summary>
        private static int ResolveTarget(Settings cfg, HashSet<int> pre, int first)
        {
            Stopwatch sw = Stopwatch.StartNew();
            string lastKey = null; long stableSince = 0; bool logged = false;
            while (true)
            {
                List<int> c = LiveCandidates(cfg, pre);
                if (c.Count == 0) return 0;
                string key = string.Join(",", c.ConvertAll(x => x.ToString()).ToArray());
                if (key != lastKey)
                {
                    lastKey = key; stableSince = sw.ElapsedMilliseconds;
                    if (c.Count > 1 || c[0] != first) { Log.Write("waiting for the game's process set to hold still (" + key + "); it restarts itself at startup"); logged = true; }
                }
                bool held = sw.ElapsedMilliseconds - stableSince >= (long)cfg.StableSec * 1000;
                if (c.Count == 1 && held)
                {
                    if (logged) Log.Write("process set settled on pid " + c[0]);
                    return c[0];
                }
                if (sw.ElapsedMilliseconds >= (long)cfg.ResolveSec * 1000)
                {
                    int best = c[0]; long bestWs = -1;
                    foreach (int id in c)
                    {
                        long ws = 0;
                        try { using (Process pr = Process.GetProcessById(id)) ws = pr.WorkingSet64; } catch { }
                        if (ws > bestWs) { bestWs = ws; best = id; }
                    }
                    Log.Write("the process set (" + key + ") did not settle in " + cfg.ResolveSec + " s; taking pid " + best + " (the largest)");
                    return best;
                }
                Thread.Sleep(cfg.PollMs);
            }
        }

        /// <summary>The stand-aside checks, then the load. Runs once for the confirmed game PID.</summary>
        private static void Decide(Settings cfg, string dir, string dll, int pid, string exePath, string gameDir)
        {
            lock (StateGate) { if (s_handled.Contains(pid)) { Log.Write("skip: this plugin already acted on pid " + pid); return; } s_handled.Add(pid); }
            string gd = exePath != null ? Path.GetDirectoryName(exePath) : gameDir;

            // (b) Mod Settings already loaded: the DLL owns modsettings-status.json from here on
            string already = ProcUtil.FindModule(pid, cfg.DllName);
            if (already != null) { Log.Write("skip: pid " + pid + " already has " + cfg.DllName + " loaded (" + already + "); the DLL owns " + Status.FileName); return; }

            // (c) Dynasty Hooks running its own copy of the any-game feature
            DhVerdict dh = ResolveDynastyHooks(cfg, pid, gd);
            if (cfg.RespectDynastyHooks && dh != null && dh.FeatureOn)
            {
                string reason = "another tool runs its own copy of this feature in this game (" + string.Join(", ", dh.KeysOn.ToArray()) + " in " + (dh.IniPath ?? dh.HooksDir) + "); " + Names.Product + " stands aside";
                Log.Write("stand aside: " + reason);
                Status.Write(dir, pid, exePath, "standing_aside", reason, dh, null);
                return;
            }
            if (dh != null) Log.Write("the other tool is present with its own copy of this feature off: " + dh.Describe() + "; loading " + Names.Product + " normally");
            else Log.Write("no other copy of this feature found; loading " + Names.Product);

            // load
            string err;
            bool ok = ModuleLoad.Load(pid, dll, cfg.DllName, cfg.InjectTimeoutSec * 1000, out err);
            for (int attempt = 2; !ok && attempt <= 4 && err != null && err.Contains("Win32 5") && ProcUtil.Alive(pid); attempt++)
            {
                Log.Write("load: access denied; the game may still be starting. Retrying in 10 s (attempt " + attempt + " of 4)");
                Thread.Sleep(10000);
                if (!ProcUtil.Alive(pid)) break;
                ok = ModuleLoad.Load(pid, dll, cfg.DllName, cfg.InjectTimeoutSec * 1000, out err);
            }
            if (ok) Log.Write("loaded: " + cfg.DllName + " is in pid " + pid + "; the DLL now writes " + Status.FileName);
            else Status.Write(dir, pid, exePath, "failed", null, dh, "could not load " + cfg.DllName + ": " + err);
        }

        /// <summary>The Dynasty Hooks verdict from the loaded module first (its ini beside it), else the installed
        /// plugin. Null when neither is present.</summary>
        private static DhVerdict ResolveDynastyHooks(Settings cfg, int pid, string gameDir)
        {
            try
            {
                string mod = ProcUtil.FindDynastyHooksModule(pid, gameDir);
                if (mod != null)
                {
                    string modDir = Path.GetDirectoryName(mod);
                    DhVerdict v = DhVerdict.FromIni("loaded module", modDir, "autoprogress.dll/proxy already in the game at " + mod, cfg.StandAsideKeys);
                    Log.Write(v.Describe());
                    return v;
                }
                DhVerdict pv = DhVerdict.FromPlugin(PluginsDir(), cfg.StandAsideKeys);
                if (pv != null) Log.Write(pv.Describe());
                return pv;
            }
            catch (Exception ex) { Log.Write("other-tool check: error ignored (" + ex.GetType().Name + ": " + ex.Message + "); not standing aside on an error"); return null; }
        }

        private static int FindNewPid(Settings cfg, HashSet<int> pre, HashSet<int> gone)
        {
            int best = 0;
            foreach (int id in ProcUtil.PidsByName(cfg.ProcName))
            {
                if (gone.Contains(id)) continue;
                if (cfg.RequireNewProcess && pre.Contains(id)) continue;
                if (best == 0 || id > best) best = id;
            }
            return best;
        }

        private static string SafeProfileName() { try { return ProfilesLibrary.ProfileName; } catch { return null; } }

        private static string TryGameDir()
        {
            try
            {
                FileSystemManager fs = App.FileSystemManager;
                if (fs == null || string.IsNullOrEmpty(fs.BasePath)) return null;
                return fs.BasePath.TrimEnd('\\', '/');
            }
            catch { return null; }
        }

        private static string FileStamp(string path)
        {
            try { FileInfo fi = new FileInfo(path); return "(" + fi.Length.ToString("N0") + " bytes, " + fi.LastWriteTime.ToString("yyyy-MM-dd HH:mm") + ")"; }
            catch (Exception ex) { return "(not readable: " + ex.GetType().Name + ")"; }
        }
    }
}
