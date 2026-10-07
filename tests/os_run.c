/* os_run_logged on Windows (ingest starts one process per source with it): a start that fails, a child that dies
 * with an NT status, a child that exits, the variable the child inherits, the input it does not read. The child is
 * this program again. Built and run as the test os_run (ctest) on Windows only. */
#define _CRT_SECURE_NO_WARNINGS
#include "os.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static char exe[4096], log[4096];
static void first_line(char *line, size_t n){ FILE *f = fopen(log, "r"); line[0] = 0; if (f) { if (!fgets(line, (int)n, f)) line[0] = 0; fclose(f); } }
int main(int argc, char **argv){
    if (argc > 1 && !strcmp(argv[1], "child-exit")) { printf("hello\n"); return atoi(argv[2]); }
    if (argc > 1 && !strcmp(argv[1], "child-die")) { ExitProcess(0xC0000142); }                     /* STATUS_DLL_INIT_FAILED, as a process that could not start ends */
    if (argc > 1 && !strcmp(argv[1], "child-env")) { printf("%s\n", getenv("LAPLACE_TEST") ? getenv("LAPLACE_TEST") : "(unset)"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "child-stdin")) { char c; printf("%s\n", fread(&c, 1, 1, stdin) ? "read a byte" : "nothing to read"); return 0; }
    DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe); if (!k || k >= sizeof exe) { fprintf(stderr, "no path of my own\n"); return 2; }
    for (char *p = exe; *p; p++) if (*p == '\\') *p = '/';
    { char tmp[4096]; if (!GetTempPathA(sizeof tmp, tmp)) return 2; snprintf(log, sizeof log, "%slaplace-os_run-%lu.log", tmp, (unsigned long)GetCurrentProcessId()); }
    int fails = 0; char line[64];
    { char *av[] = { "C:/no/such/program.exe", NULL }; errno = 0;
      int r = os_run_logged("C:/no/such/program.exe", av, log, "LAPLACE_TEST", "1");
      printf("no such program: returned %d, errno %d (%s)\n", r, errno, strerror(errno)); if (r != -1 || errno != ENOENT) fails++; }
    { char *av[] = { (char *)exe, "child-die", NULL };
      int r = os_run_logged(exe, av, log, "LAPLACE_TEST", "1");
      printf("died with an NT status: returned %d\n", r); if (r != 128) fails++; }
    { char *av[] = { (char *)exe, "child-exit", "3", NULL };
      int r = os_run_logged(exe, av, log, "LAPLACE_TEST", "1"); first_line(line, sizeof line);
      printf("exited 3: returned %d, log says %s", r, line); if (r != 3 || strncmp(line, "hello", 5)) fails++; }
    { char *av[] = { (char *)exe, "child-env", NULL };
      int r = os_run_logged(exe, av, log, "LAPLACE_TEST", "1"); first_line(line, sizeof line);
      printf("the child inherits the variable: returned %d, log says %s", r, line); if (r || strncmp(line, "1", 1)) fails++;
      printf("the parent's variable after: %s\n", getenv("LAPLACE_TEST") ? getenv("LAPLACE_TEST") : "(unset)"); if (getenv("LAPLACE_TEST")) fails++; }
    { char *av[] = { (char *)exe, "child-stdin", NULL };
      int r = os_run_logged(exe, av, log, "LAPLACE_TEST", "1"); first_line(line, sizeof line);
      printf("the child's input is NUL: returned %d, log says %s", r, line); if (r || strncmp(line, "nothing", 7)) fails++; }
    remove(log); printf(fails ? "FAILED %d\n" : "ok\n", fails); return fails;
}
