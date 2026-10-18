// Tiny sampling profiler: a thread suspends the target thread ~every 1 ms,
// unwinds its stack with the x64 unwind tables and appends the return
// addresses to samples.bin. Resolve with prof.py.
#include <windows.h>
#include <stdio.h>
#include <psapi.h>

static HANDLE target;
static volatile int running;
static FILE *out;

static DWORD WINAPI sampler(LPVOID arg)
{
	timeBeginPeriod(1);
	while (running) {
		Sleep(1);
		if (SuspendThread(target) == (DWORD)-1) break;
		CONTEXT c = { 0 };
		c.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(target, &c)) {
			DWORD64 pcs[48];
			int n = 0;
			for (; n < 48 && c.Rip; n++) {
				pcs[n] = c.Rip;
				DWORD64 base;
				PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
				if (!fn) { // leaf function: return address is at rsp
					c.Rip = *(DWORD64 *)c.Rsp;
					c.Rsp += 8;
				} else {
					void *hd;
					DWORD64 est;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &hd, &est, NULL);
				}
			}
			fwrite(&n, sizeof n, 1, out);
			fwrite(pcs, sizeof *pcs, n, out);
		}
		ResumeThread(target);
	}
	return 0;
}

static HANDLE th;

void sampler_start(const char *path)
{
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
	out = fopen(path, "wb");
	DWORD64 base = (DWORD64)GetModuleHandleA(NULL);
	fwrite(&base, sizeof base, 1, out);
	running = 1;
	th = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
	SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
}

void sampler_stop(void)
{
	running = 0;
	WaitForSingleObject(th, INFINITE);
	// trailer: module table, marked by a negative count
	HMODULE mods[256];
	DWORD need;
	EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need);
	int nm = -(int)(need / sizeof(HMODULE));
	fwrite(&nm, sizeof nm, 1, out);
	for (int i = 0; i < -nm; i++) {
		MODULEINFO mi;
		char name[64] = "";
		GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
		GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof name);
		DWORD64 b = (DWORD64)mi.lpBaseOfDll, e = b + mi.SizeOfImage;
		fwrite(&b, 8, 1, out);
		fwrite(&e, 8, 1, out);
		fwrite(name, 64, 1, out);
	}
	fclose(out);
}
