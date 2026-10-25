#!/usr/bin/env python3
"""Infer each import's stdcall argument count from the push sequences at its call sites.
   Also count fs:/gs: segment use (SEH/TLS)."""
import sys, struct, collections
sys.path.insert(0,'tools')
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter
EXE=sys.argv[1]; pe=pefile.PE(EXE); IB=pe.OPTIONAL_HEADER.ImageBase; L=Lifter(EXE)
pe.parse_data_directories()
iat={}
# IMPDLL=<original module>: read the import table from the unrelocated file
# (pefile refuses the relocated analysis copy's import directory) and move
# the IAT addresses to this image's base.
import os
_ipe=pefile.PE(os.environ['IMPDLL']) if os.environ.get('IMPDLL') else pe
_idelta=IB-_ipe.OPTIONAL_HEADER.ImageBase
for e in _ipe.DIRECTORY_ENTRY_IMPORT:
    for imp in e.imports:
        iat[(imp.address+_idelta)&0xffffffff]=(e.dll.decode('latin1'), imp.name.decode('latin1') if imp.name else f"#{imp.ordinal}")
def rd32(a):
    for nm,va,vs,d in L.secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
md=L.md
seeds={IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint}
# Second guest modules (gen.sh, server.dll): RELOCS = the module's fixup
# slots, IMPOUT = output file, SYMPFX = table prefix (g_<pfx>imp), STRICT=1 =
# every import must be in KNOWN (no guessed stdcall counts for a module).
# The exe's defaults are under the game's GEN_DIR and OUT_DIR (gen.sh).
import os
RELOCS=os.environ.get('RELOCS',os.path.join(os.environ.get('OUT_DIR','out'),'relocs.txt'))
IMPOUT=os.environ.get('IMPOUT',os.path.join(os.environ.get('GEN_DIR','gen'),'all','impargs.c'))
SYMPFX=os.environ.get('SYMPFX',''); STRICT=os.environ.get('STRICT','')=='1'
for a in (int(x,16) for x in open(RELOCS)):
    v=rd32(a)
    if v is not None and L.in_text(v): seeds.add(v)
cov=bytearray(L.TSZ); q=collections.deque(seeds); seen=set(seeds)
argv=collections.defaultdict(collections.Counter)
unreliable=set()
seg=collections.Counter(); seg_sites=collections.Counter()
while q:
    va=q.popleft(); recent=[]
    while True:
        if not L.in_text(va): break
        off=va-L.TVA
        if off>=L.TSZ or cov[off]: break
        try: i=next(md.disasm(L.TDATA[off:off+16],va))
        except StopIteration: break
        for k in range(i.size):
            if off+k<L.TSZ: cov[off+k]=1
        m=i.mnemonic; ops=i.operands; o=ops[0] if ops else None
        for op2 in ops:
            if op2.type==X86_OP_MEM and op2.mem.segment:
                s=i.reg_name(op2.mem.segment) or '?'
                seg[s]+=1; seg_sites[(s,m)]+=1
        if m in ('call','jmp') and o is not None and o.type==X86_OP_MEM and o.mem.base==0 and o.mem.index==0:
            t=o.mem.disp & 0xffffffff
            if t in iat:
                n=0; hit_prologue=False
                prev_was_mov_ebp_esp=False
                for prev in reversed(recent):
                    m2=prev.mnemonic; ops2=prev.op_str
                    # function prologue: once we see 'mov ebp, esp' the arguments are over
                    if m2=='mov' and ops2.replace(' ','')=='ebp,esp':
                        hit_prologue=True; break
                    if m2=='sub' and ops2.startswith('esp'):
                        hit_prologue=True; break
                    if m2=='push': n+=1
                    elif m2 in ('lea','xor','test','cmp','movzx','movsx','inc','dec','and','or','mov','add'):
                        continue
                    else: break
                if hit_prologue and n>0:
                    n = max(0, n)   # ran into the prologue: the count is unreliable, mark it
                    argv.setdefault(t, collections.Counter())
                    unreliable.add(t)
                argv[t][n]+=1
        recent.append(i)
        if len(recent)>24: recent.pop(0)
        if m=='call':
            if o is not None and o.type==X86_OP_IMM and L.in_text(o.imm) and o.imm not in seen:
                seen.add(o.imm); q.append(o.imm)
            va=i.address+i.size; recent=[]; continue
        if m=='jmp':
            if o is not None and o.type==X86_OP_IMM: va=o.imm; recent=[]; continue
            break
        if m.startswith('j'):
            if o is not None and o.type==X86_OP_IMM and o.imm not in seen:
                seen.add(o.imm); q.append(o.imm)
            va=i.address+i.size; continue
        if m in ('ret','retf','int3','hlt','ud2'): break
        va=i.address+i.size

print("=== SEGMENT PREFIX USE (a sign of SEH / TLS) ===")
for s,n in seg.most_common(): print(f"  {s}: {n} accesses")
print("  most common patterns:")
for (s,m),n in seg_sites.most_common(8): print(f"    {s}: {m:8s} {n}")

# cross-check against known correct values
KNOWN={
 'GetVersion':0,'GetVersionExA':1,'GetVersionExW':1,'GetModuleHandleA':1,'GetModuleHandleW':1,
 'GetModuleFileNameA':3,'GetModuleFileNameW':3,'GetProcAddress':2,'LoadLibraryA':1,'LoadLibraryW':1,
 'FreeLibrary':1,'GetCommandLineA':0,'GetCommandLineW':0,'GetStartupInfoA':1,'GetStartupInfoW':1,
 'GetEnvironmentStrings':0,'GetEnvironmentStringsA':0,'GetEnvironmentStringsW':0,
 'FreeEnvironmentStringsA':1,'FreeEnvironmentStringsW':1,'GetEnvironmentVariableA':3,
 'GetStdHandle':1,'SetStdHandle':2,'GetFileType':1,'SetHandleCount':1,'CloseHandle':1,
 'CreateFileA':7,'CreateFileW':7,'ReadFile':5,'WriteFile':5,'SetFilePointer':4,'GetFileSize':2,
 'DeleteFileA':1,'FlushFileBuffers':1,'SetEndOfFile':1,'GetFileAttributesA':1,'SetFileAttributesA':2,
 'FindFirstFileA':2,'FindNextFileA':2,'FindClose':1,'CreateDirectoryA':2,'RemoveDirectoryA':1,
 'GetCurrentDirectoryA':2,'SetCurrentDirectoryA':1,'GetFullPathNameA':4,'GetTempPathA':2,
 'MoveFileA':2,'CopyFileA':3,'GetDriveTypeA':1,'GetDiskFreeSpaceA':5,'GetLogicalDrives':0,
 'GetShortPathNameA':3,'GetLongPathNameA':3,'DuplicateHandle':7,'CreatePipe':4,'PeekNamedPipe':6,
 'HeapCreate':3,'HeapDestroy':1,'HeapAlloc':3,'HeapReAlloc':4,'HeapFree':3,'HeapSize':3,
 'GetProcessHeap':0,'VirtualAlloc':4,'VirtualFree':3,'VirtualProtect':4,'VirtualQuery':3,
 'GlobalAlloc':2,'GlobalFree':1,'GlobalLock':1,'GlobalUnlock':1,'LocalAlloc':2,'LocalFree':1,
 'GlobalMemoryStatus':1,'GetSystemInfo':1,
 'TlsAlloc':0,'TlsGetValue':1,'TlsSetValue':2,'TlsFree':1,
 'InitializeCriticalSection':1,'InitializeCriticalSectionAndSpinCount':2,'EnterCriticalSection':1,
 'LeaveCriticalSection':1,'DeleteCriticalSection':1,'TryEnterCriticalSection':1,
 'GetCurrentThreadId':0,'GetCurrentProcessId':0,'GetCurrentProcess':0,'GetCurrentThread':0,
 'CreateThread':6,'ExitThread':1,'ResumeThread':1,'SuspendThread':1,'SetThreadPriority':2,
 'GetThreadPriority':1,'GetExitCodeThread':2,'GetExitCodeProcess':2,'SetThreadAffinityMask':2,
 'WaitForSingleObject':2,'WaitForMultipleObjects':4,'CreateEventA':4,'SetEvent':1,'ResetEvent':1,
 'CreateMutexA':3,'ReleaseMutex':1,'CreateSemaphoreA':4,'ReleaseSemaphore':3,
 'InterlockedIncrement':1,'InterlockedDecrement':1,'InterlockedExchange':2,
 'InterlockedCompareExchange':3,'InterlockedExchangeAdd':2,
 'Sleep':1,'GetTickCount':0,'QueryPerformanceCounter':1,'QueryPerformanceFrequency':1,
 'GetSystemTimeAsFileTime':1,'GetLocalTime':1,'GetSystemTime':1,'GetTimeZoneInformation':1,
 'FileTimeToSystemTime':2,'FileTimeToLocalFileTime':2,'SystemTimeToFileTime':2,
 'GetLastError':0,'SetLastError':1,'ExitProcess':1,'TerminateProcess':2,'SetErrorMode':1,
 'SetUnhandledExceptionFilter':1,'UnhandledExceptionFilter':1,'RtlUnwind':4,'RaiseException':4,
 'IsProcessorFeaturePresent':1,'IsDebuggerPresent':0,'OutputDebugStringA':1,
 'SetConsoleCtrlHandler':2,'DebugBreak':0,
 'GetACP':0,'GetOEMCP':0,'GetCPInfo':2,'IsValidCodePage':1,'MultiByteToWideChar':6,
 'WideCharToMultiByte':8,'GetStringTypeA':5,'GetStringTypeW':4,'LCMapStringA':6,'LCMapStringW':6,
 'GetLocaleInfoA':4,'GetLocaleInfoW':4,'GetUserDefaultLCID':0,'GetThreadLocale':0,'SetThreadLocale':1,
 'CompareStringA':6,'CompareStringW':6,'IsValidLocale':2,'EnumSystemLocalesA':2,
 'lstrlenA':1,'lstrcpyA':2,'lstrcatA':2,'lstrcmpA':2,'lstrcmpiA':2,'lstrcpynA':3,
 'FindResourceA':3,'FindResourceW':3,'LoadResource':2,'LockResource':1,'SizeofResource':2,
 'SetCommMask':2,'PurgeComm':2,'GetCommState':2,'SetCommState':2,'SetupComm':3,
 'GetCommTimeouts':2,'SetCommTimeouts':2,'EscapeCommFunction':2,'ClearCommError':3,
 'ShowCursor':1,'GetCursorPos':1,'SetCursorPos':2,'MessageBoxA':4,'GetSystemMetrics':1,
 'DefWindowProcA':4,'ReleaseCapture':0,'SetCapture':1,'GetMessageA':4,'DispatchMessageA':1,
 'PeekMessageA':5,'TranslateMessage':1,'LoadIconA':2,'LoadCursorA':2,'RegisterClassExA':1,
 'AdjustWindowRect':3,'GetDesktopWindow':0,'CreateWindowExA':12,'UpdateWindow':1,'SetCursor':1,
 'SetFocus':1,'SetForegroundWindow':1,'ShowWindow':2,'SetWindowLongA':3,'GetWindowLongA':2,
 'DestroyWindow':1,'BeginPaint':2,'EndPaint':2,'GetClientRect':2,'GetWindowRect':2,
 'SetWindowPos':7,'PostQuitMessage':1,'PostMessageA':4,'SendMessageA':4,'ClientToScreen':2,
 'ScreenToClient':2,'InvalidateRect':3,'GetActiveWindow':0,'IsIconic':1,'SetWindowTextA':2,
 'UnregisterClassA':2,'GetKeyboardState':1,'GetAsyncKeyState':1,'MapVirtualKeyA':2,
 'RegOpenKeyA':3,'RegOpenKeyExA':5,'RegCreateKeyA':3,'RegCreateKeyExA':9,
 'RegQueryValueA':4,'RegSetValueA':5,'RegEnumKeyA':4,'RegFlushKey':1,'RegQueryInfoKeyA':12,
 'RegConnectRegistryA':3,'RegSaveKeyA':3,'RegRestoreKeyA':3,'RegQueryValueExA':6,'RegSetValueExA':6,'RegCloseKey':1,
 'RegDeleteKeyA':2,'RegDeleteValueA':2,'RegEnumKeyExA':8,'RegEnumValueA':8,
 'ExtTextOutA':8,'TextOutA':5,'CreateFontA':14,'SelectObject':2,'SetTextColor':2,
 'SetBkColor':2,'SetBkMode':2,'DeleteObject':1,'GetDeviceCaps':2,'CreateCompatibleDC':1,
 'CreateCompatibleBitmap':3,'BitBlt':9,'DeleteDC':1,'GetStockObject':1,'StretchBlt':11,
 'SetPixel':4,'GetPixel':3,'CreateSolidBrush':1,'Rectangle':5,'MoveToEx':4,'LineTo':3,
 'GetTextExtentPoint32A':4,'CreateDCA':4,'GetObjectA':3,'SetMapMode':2,'CreateBitmap':5,
 'SleepEx':2,'GetDiskFreeSpaceExA':4,'FlushInstructionCache':3,'GetSystemDirectoryA':2,
 'GetWindowsDirectoryA':2,'GetComputerNameA':2,'GetUserNameA':2,'GetProfileStringA':5,
 'WritePrivateProfileStringA':4,'GetPrivateProfileIntA':4,
 # --- the remaining 43: authoritative values ---
 'IsBadCodePtr':1,'IsBadWritePtr':2,'IsBadReadPtr':2,'SetEnvironmentVariableA':2,
 'Process32First':2,'Process32Next':2,'CreateToolhelp32Snapshot':2,
 'SetProcessAffinityMask':2,'GetProcessAffinityMask':3,'SetPriorityClass':2,'GetPriorityClass':1,
 'MapViewOfFile':5,'CreateFileMappingA':6,'UnmapViewOfFile':1,'TerminateThread':2,
 'WaitCommEvent':3,'GetOverlappedResult':4,'SetCommConfig':3,'GetCommConfig':3,
 'Netbios':1,'ShellExecuteA':6,'SHGetFolderPathA':5,
 'lineAnswer':3,'lineMakeCall':5,'lineGetID':6,'lineClose':1,'lineNegotiateAPIVersion':6,
 'lineOpen':9,'lineGetDevCaps':5,'lineInitialize':5,'lineShutdown':1,
 'wvsprintfA':3,'PostThreadMessageA':4,'SetRect':5,'SetActiveWindow':1,'GetForegroundWindow':0,
 'wsprintfA':0,   # cdecl variadic: the caller cleans up -> the callee NEVER does
 'timeSetEvent':5,'timeKillEvent':1,'timeGetDevCaps':2,
 'shutdown':2,'getpeername':3,'WSAIoctl':9,
 'timeGetTime':0,'timeBeginPeriod':1,'timeEndPeriod':1,'joyGetPosEx':2,'joyGetNumDevs':0,
 'joyGetDevCapsA':3,
 'Direct3DCreate9':1,'DirectInput8Create':5,'DirectDrawCreate':3,'DirectSoundCreate':3,
 # imports by ordinal: DLL#ordinal (see known() below)
 'DSOUND#1':3,   # DirectSoundCreate
 'DSOUND#6':3,   # DirectSoundCaptureCreate (MW)
 'WSAStartup':2,'WSACleanup':0,'socket':3,'bind':3,'listen':2,'accept':3,'connect':3,
 'send':4,'recv':4,'sendto':6,'recvfrom':6,'closesocket':1,'select':5,'ioctlsocket':3,
 'setsockopt':5,'getsockopt':5,'htons':1,'ntohs':1,'htonl':1,'ntohl':1,'inet_addr':1,
 'inet_ntoa':1,'gethostbyname':1,'gethostname':2,'WSAGetLastError':0,'getsockname':3,
 'GetFileTime':4,
 # --- Most Wanted (GAME.md 3.4): its speed.exe imports these, server.dll __WSAFDIsSet too ---
 'GlobalMemoryStatusEx':1,'FatalAppExitA':2,'QueueUserAPC':3,'CreateProcessA':10,'CreateWaitableTimerA':3,
 'SetWaitableTimer':6,'HeapValidate':3,'GetTimeFormatA':6,'GetDateFormatA':6,
 'D3DXMatrixMultiply':3,'D3DXVec4Transform':3,'D3DXVec3Transform':3,'D3DXMatrixInverse':3,
 'D3DXCreateEffectFromResourceA':9,'D3DXCreateEffectPool':1,'D3DXMatrixOrthoLH':5,'D3DXMatrixPerspectiveLH':5,
 'D3DXMatrixTranslation':4,'D3DXVec3TransformCoordArray':6,'D3DXVec3Normalize':2,'D3DXVec3TransformNormal':3,
 'D3DXMatrixTranspose':2,'GetKeyState':1,'MapVirtualKeyExA':3,'GetKeyboardLayout':1,'SendInput':3,'ToUnicode':6,
 'waveOutGetDevCapsA':3,'waveOutGetNumDevs':0,'waveOutWrite':3,'waveOutGetPosition':3,'waveOutSetVolume':2,
 'waveOutReset':1,'waveOutOpen':6,'waveOutClose':1,'waveOutPrepareHeader':3,'waveOutUnprepareHeader':3,
 'waveInStart':1,'waveInStop':1,'waveInReset':1,'waveInAddBuffer':3,'waveInGetPosition':3,'waveInOpen':6,
 'waveInClose':1,'waveInGetNumDevs':0,'waveInGetDevCapsA':3,'waveInPrepareHeader':3,'waveInUnprepareHeader':3,
 'WSACreateEvent':0,'WSACloseEvent':1,'WSASetEvent':1,'WSAResetEvent':1,'WSARecv':7,'WSARecvFrom':9,
 'WSAGetOverlappedResult':5,'WSAWaitForMultipleEvents':5,'__WSAFDIsSet':2,
}
# An import by ordinal is named "#<n>" (in g_imp too), and ordinals are per
# DLL: its count is KNOWN["<DLL>#<n>"], the DLL's name in capitals without .dll.
def known(dll, nm):
    if nm.startswith('#'):
        nm = dll.upper().removesuffix('.DLL') + nm
    return KNOWN.get(nm)
hit=miss=0; rows=[]
for a,(dll,nm) in iat.items():
    cnt=argv.get(a)
    best=cnt.most_common(1)[0][0] if cnt else 0
    rows.append((dll,nm,best,sum(cnt.values()) if cnt else 0,a))
    k=known(dll,nm)
    if k is not None and cnt:
        if k==best: hit+=1
        else: miss+=1; print(f"  MISMATCH {nm}: inferred {best}, real {k}  (distribution {dict(cnt)})")
print(f"\n=== ARGUMENT COUNT INFERENCE ACCURACY ===")
print(f"  of {hit+miss} known functions: {hit} right, {miss} wrong = {100.0*hit/max(hit+miss,1):.1f}%")
if STRICT:
    unk=sorted({nm if not nm.startswith('#') else f"{dll} {nm}" for dll,nm,n,sites,a in rows if known(dll,nm) is None})
    if unk: sys.exit(f"argcount: no known stdcall argument count for {', '.join(unk)} (add them to KNOWN)")
with open(IMPOUT,'w') as f:
    f.write('#include <stdint.h>\ntypedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;\n')
    f.write(f'const uint32_t g_{SYMPFX}imp_count = {len(rows)};\nconst ImpEnt g_{SYMPFX}imp[] = {{\n')
    for dll,nm,n,sites,a in sorted(rows,key=lambda r:r[4]):
        k = known(dll, nm); n2 = n if k is None else k
        f.write(f'  {{0x{a:08x}u,"{dll}","{nm}",{n2}}},\n')
    f.write('};\n')
print(f"-> {IMPOUT} ({len(rows)} entries)")
