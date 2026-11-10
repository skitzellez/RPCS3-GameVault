// Loads every DLL and function rpcs3_uwp.exe imports and writes which ones are missing to
// LocalState/import_check.txt. Run it on a device where the main app fails to start: the
// loader refuses a whole exe when one import is missing and doesn't say which.

#include "../app/winrt_min.h"
#include "import_table.h"

#include <cstdio>
#include <string>

extern "C" HMODULE WINAPI LoadLibraryW(LPCWSTR);

static std::wstring local_state_dir()
{
	using namespace winrt_min;
	IApplicationDataStatics* statics = nullptr;
	IApplicationData* data = nullptr;
	IInspectable* folder = nullptr;
	IStorageItem* item = nullptr;
	HSTRING path = nullptr;
	std::wstring result;

	if (SUCCEEDED(get_factory(L"Windows.Storage.ApplicationData", IID_IApplicationDataStatics, &statics)) &&
		SUCCEEDED(statics->get_Current(&data)) && SUCCEEDED(data->get_LocalFolder(&folder)) &&
		SUCCEEDED(folder->QueryInterface(IID_IStorageItem, reinterpret_cast<void**>(&item))) && SUCCEEDED(item->get_Path(&path)))
	{
		result = WindowsGetStringRawBuffer(path, nullptr);
		result += L'\\';
	}

	if (path) WindowsDeleteString(path);
	if (item) item->Release();
	if (folder) folder->Release();
	if (data) data->Release();
	if (statics) statics->Release();
	return result;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	RoInitialize(RO_INIT_MULTITHREADED);
	const std::wstring path = local_state_dir() + L"import_check.txt";
	FILE* f = _wfopen(path.c_str(), L"w");
	if (!f)
		return 1;

	unsigned missing_dlls = 0, missing_funcs = 0;
	const wchar_t* last_dll = nullptr;
	HMODULE module = nullptr;

	for (const auto& imp : g_imports)
	{
		if (!last_dll || wcscmp(last_dll, imp.dll) != 0)
		{
			last_dll = imp.dll;
			module = LoadLibraryW(imp.dll);
			fprintf(f, "%ls: %s (error %lu)\n", imp.dll, module ? "loaded" : "MISSING", module ? 0ul : GetLastError());
			fflush(f);
			if (!module)
				missing_dlls++;
		}

		if (module && !GetProcAddress(module, imp.name))
		{
			fprintf(f, "  MISSING function %s\n", imp.name);
			missing_funcs++;
		}
	}

	fprintf(f, "done: %u missing DLLs, %u missing functions\n", missing_dlls, missing_funcs);
	fflush(f);

	// Game Vault: the memory this app gets (Game type ~5 GB, App type ~1 GB) and the address space rpcs3_uwp.exe
	// reserves before its first log line (vm.cpp's static g_base_addr/g_exec_addr/g_hook_addr/g_stat_addr).
	{
		using namespace winrt_min;
		IMemoryManagerStatics* mm = nullptr;
		UINT64 usage = 0, limit = 0;
		if (SUCCEEDED(get_factory(L"Windows.System.MemoryManager", IID_IMemoryManagerStatics, &mm)))
		{
			mm->get_AppMemoryUsage(&usage);
			mm->get_AppMemoryUsageLimit(&limit);
			mm->Release();
		}
		fprintf(f, "memory: limit %llu MB, in use %llu MB (%s)\n", limit >> 20, usage >> 20,
			limit >= (3ull << 30) ? "Game type" : "App type or unknown: rpcs3 needs Game type");
		fflush(f);

		using alloc2_t = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
		const HMODULE mem = LoadLibraryW(L"api-ms-win-core-memory-l1-1-6.dll");
		const auto alloc2 = mem ? reinterpret_cast<alloc2_t>(GetProcAddress(mem, "VirtualAlloc2FromApp")) : nullptr;
		const struct { unsigned long long addr, size; const char* name; } tries[] = {
			{0x200000000ull, 0x200000000ull, "base+sudo 8 GB"},
			{0x400000000ull, 0x300000000ull, "exec 12 GB"},
			{0x700000000ull, 0x800000000ull, "hook 32 GB"},
			{0xF00000000ull, 0x100000000ull, "stat 4 GB"},
		};
		for (const auto& t : tries)
		{
			void* p = alloc2 ? alloc2(nullptr, reinterpret_cast<void*>(t.addr), t.size, MEM_RESERVE | 0x00040000 /*MEM_RESERVE_PLACEHOLDER*/, PAGE_NOACCESS, nullptr, 0) : nullptr;
			fprintf(f, "reserve %s at 0x%llx: %s (error %lu)\n", t.name, t.addr, p ? "ok" : "FAILED", p ? 0ul : GetLastError());
			fflush(f);
		}
		// commit 2 GB in 256 MB steps somewhere free: fails early on App type
		unsigned committed = 0;
		for (int i = 0; i < 8 && alloc2; i++)
		{
			void* p = alloc2(nullptr, nullptr, 256u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
			if (!p)
				break;
			committed += 256;
		}
		fprintf(f, "commit test: %u MB of 2048 MB\n", committed);
	}

	fclose(f);
	return 0;
}
