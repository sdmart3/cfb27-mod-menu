/* loadhost.c: a dummy x64 host for the load test (build.ps1), after Call Sheet's. LoadLibraryA modsettings.dll into THIS
 * process (which is not CollegeFB27.exe), wait for its worker to write modsettings-status.json and exit 0 (with the DLL still loaded, as in the game). The DLL
 * must find no game, say so in its log and status file, install nothing, and not crash. Also proves the double-load
 * guard: a second LoadLibrary of the same file returns the same module (Windows refcount, DllMain not run again), and a
 * COPY under another name must be refused by the named mutex (LoadLibrary returns NULL). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: loadhost <modsettings.dll>\n"); return 2; }
  HMODULE h = LoadLibraryA(argv[1]);
  if (!h) { fprintf(stderr, "LoadLibrary failed (%lu)\n", GetLastError()); return 1; }
  printf("loaded %s at %p\n", argv[1], (void *)h);
  HMODULE same = LoadLibraryA(argv[1]);
  printf("the same file again: %s\n", same == h ? "the same module (refcount)" : "A DIFFERENT MODULE");
  if (same != h) return 1;
  FreeLibrary(same);
  char copy[MAX_PATH]; snprintf(copy, sizeof copy, "%s.second.dll", argv[1]);
  if (CopyFileA(argv[1], copy, FALSE)) {
    HMODULE h2 = LoadLibraryA(copy);
    printf("second copy: %s (%lu)\n", h2 ? "LOADED (the guard did not hold)" : "refused by the guard", h2 ? 0 : GetLastError());
    if (h2) { FreeLibrary(h2); DeleteFileA(copy); return 1; }
    DeleteFileA(copy);
  }
  Sleep(2500);   /* the worker writes the status file on its first tick */
  char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s", argv[1]); char *sl = strrchr(dir, '\\'); if (sl) *sl = 0;
  char st[MAX_PATH]; snprintf(st, sizeof st, "%s\\modsettings-status.json", dir);
  FILE *f = fopen(st, "rb"); if (!f) { fprintf(stderr, "no modsettings-status.json at %s\n", st); return 1; }
  char buf[65536]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
  char pid[48]; snprintf(pid, sizeof pid, "\"pid\":%lu,", GetCurrentProcessId());
  int ok = strstr(buf, "\"state\":\"failed\"") && strstr(buf, "game not found") && strstr(buf, pid) && strstr(buf, "\"settings\":{\"enabled\":") && strstr(buf, "\"manifests\":");
  printf("modsettings-status.json: %s\n", ok ? "state failed / game not found, this pid, settings + manifests present" : "UNEXPECTED");
  if (!ok) printf("%s\n", buf);
  /* no FreeLibrary(h): the worker thread lives as long as the process (as in the game); unloading under it would crash */
  (void)h;
  printf("exiting with the DLL loaded\n");
  return ok ? 0 : 1;
}
