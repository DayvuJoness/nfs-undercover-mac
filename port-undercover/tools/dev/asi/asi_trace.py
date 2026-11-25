"""Run an ASI plugin's DllMain under Unicorn beside the game exe's image and record every byte it writes into the
exe's image (what it patches). Usage: asi_trace.py <nfs.exe> <plugin.asi> <ini dir> [out.json]"""
import struct, sys, json, os
from unicorn import *
from unicorn.x86_const import *

exe_path, asi_path, ini_dir = sys.argv[1:4]
out_path = sys.argv[4] if len(sys.argv) > 4 else None
mu = Uc(UC_ARCH_X86, UC_MODE_32)
PAGE = 0x1000
def up(x): return (x + PAGE - 1) & ~(PAGE - 1)

def load_pe(path, base_override=None):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    n = struct.unpack_from('<H', b, pe + 6)[0]; optsz = struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from('<I', b, opt + 28)[0]; size = struct.unpack_from('<I', b, opt + 56)[0]
    ep = struct.unpack_from('<I', b, opt + 16)[0]
    hdr = struct.unpack_from('<I', b, opt + 60)[0]
    mu.mem_map(base, up(size))
    mu.mem_write(base, b[:hdr])
    for i in range(n):
        o = opt + optsz + 40 * i
        vs, va, rs, ra = struct.unpack_from('<4I', b, o + 8)
        if rs: mu.mem_write(base + va, b[ra:ra + min(rs, max(vs, rs))][:up(max(vs,rs))])
    dd = opt + 96
    imp_va, imp_sz = struct.unpack_from('<II', b, dd + 8)
    return b, base, size, ep, imp_va

exe, EXE_BASE, EXE_SIZE, _, _ = load_pe(exe_path)
asi, ASI_BASE, ASI_SIZE, ASI_EP, ASI_IMP = load_pe(asi_path)

# stubs
STUB = 0x70000000; mu.mem_map(STUB, 0x10000)
stubs = {}      # addr -> name
def rd(a, n): return bytes(mu.mem_read(a, n))
def u32(a): return struct.unpack('<I', rd(a, 4))[0]
def w32(a, v): mu.mem_write(a, struct.pack('<I', v & 0xffffffff))
def cstr(a, wide=False):
    s = b''
    while True:
        c = rd(a, 2 if wide else 1)
        if c == (b'\0\0' if wide else b'\0'): break
        s += c; a += len(c)
    return s.decode('utf-16le' if wide else 'latin1')
i = 0; nstub = 0
while True:
    d = ASI_BASE + ASI_IMP + 20 * i
    oft, _, _, name_rva, ft = struct.unpack('<5I', rd(d, 20))
    if not name_rva: break
    lib = cstr(ASI_BASE + name_rva)
    j = 0
    while True:
        t = u32(ASI_BASE + (oft or ft) + 4 * j)
        if not t: break
        fn = cstr(ASI_BASE + t + 2) if not t & 0x80000000 else '#%d' % (t & 0xffff)
        a = STUB + 16 * nstub; nstub += 1
        mu.mem_write(a, b'\xc3'); stubs[a] = fn
        w32(ASI_BASE + ft + 4 * j, a); j += 1
    i += 1

# heap, stack, TEB
HEAP = 0x30000000; mu.mem_map(HEAP, 0x4000000); heap_top = [HEAP]
def alloc(n):
    a = heap_top[0]; heap_top[0] = (a + n + 15) & ~15; return a
STACK = 0x08000000; mu.mem_map(STACK, 0x100000); mu.reg_write(UC_X86_REG_ESP, STACK + 0x100000 - 0x100)
TEB = 0x7ffd0000; mu.mem_map(TEB, 0x10000); w32(TEB + 0x18, TEB); w32(TEB + 0x30, TEB + 0x1000); w32(TEB + 0, 0xffffffff)
w32(TEB + 0x2c, TEB + 0x2000)  # TLS array
for k in range(16): w32(TEB + 0x2000 + 4 * k, TEB + 0x4000 + 0x400 * k)
# GDT for fs
GDT = 0x7ff00000; mu.mem_map(GDT, 0x1000)
def seg(base, limit, access, flags):
    return struct.pack('<HHBBBB', limit & 0xffff, base & 0xffff, (base >> 16) & 0xff, access, ((limit >> 16) & 0xf) | (flags << 4), base >> 24)
mu.mem_write(GDT + 8 * 1, seg(0, 0xfffff, 0x9b, 0xc)); mu.mem_write(GDT + 8 * 2, seg(0, 0xfffff, 0x93, 0xc))
mu.mem_write(GDT + 8 * 3, seg(TEB, 0xfff, 0x93, 0x4))
mu.reg_write(UC_X86_REG_GDTR, (0, GDT, 0x1000 - 1, 0))
mu.reg_write(UC_X86_REG_FS, 3 << 3)
for r in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_GS): mu.reg_write(r, 2 << 3)
mu.reg_write(UC_X86_REG_ESP, STACK + 0x100000 - 0x100)

# files
files = {}; nexth = [0x100]
def host_path(p):
    base = os.path.basename(p.replace('\\', '/'))
    if base.lower() == 'nfs.exe': return exe_path
    cand = os.path.join(ini_dir, base)
    return cand if os.path.exists(cand) else None
tls = {}; ntls = [0]; last_err = [0]
writes = []
def on_write(uc, access, addr, size, value, user):
    if EXE_BASE + 0x1000 <= addr < EXE_BASE + EXE_SIZE:
        old = rd(addr, size)
        writes.append((addr, size, value & ((1 << (8 * size)) - 1), old.hex(), uc.reg_read(UC_X86_REG_EIP)))
mu.hook_add(UC_HOOK_MEM_WRITE, on_write)
calls = []
def ret(uc, val, nargs):
    esp = uc.reg_read(UC_X86_REG_ESP); r = u32(esp)
    uc.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * nargs); uc.reg_write(UC_X86_REG_EIP, r); uc.reg_write(UC_X86_REG_EAX, val & 0xffffffff)
def arg(uc, k): return u32(uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * k)
DYN = {'InitializeCriticalSectionEx':3,'FlsAlloc':1,'FlsGetValue':1,'FlsSetValue':2,'FlsFree':1,'SleepConditionVariableCS':3,'WakeAllConditionVariable':1,'InitializeConditionVariable':1,'CreateEventExW':4,'CreateSemaphoreExW':6,'InitOnceExecuteOnce':4,'GetTickCount64':0,'GetSystemTimePreciseAsFileTime':1,'LCMapStringEx':9,'GetLocaleInfoEx':4,'CompareStringEx':9,'GetUserDefaultLocaleName':2,'IsValidLocaleName':1,'GetCurrentPackageId':2,'AreFileApisANSI':0,'SetThreadStackGuarantee':1}
NARGS = {'CloseHandle':1,'CreateEventW':4,'CreateFileW':7,'DecodePointer':1,'EncodePointer':1,'DeleteCriticalSection':1,
 'EnterCriticalSection':1,'LeaveCriticalSection':1,'ExitProcess':1,'FindClose':1,'FlushFileBuffers':1,'FreeLibrary':1,
 'GetACP':0,'GetCPInfo':2,'GetCommandLineA':0,'GetCommandLineW':0,'GetConsoleMode':2,'GetConsoleOutputCP':0,
 'GetCurrentProcess':0,'GetCurrentProcessId':0,'GetCurrentThreadId':0,'GetEnvironmentStringsW':0,'FreeEnvironmentStringsW':1,
 'GetFileSizeEx':2,'GetFileType':1,'GetLastError':0,'GetModuleFileNameA':3,'GetModuleFileNameW':3,'GetModuleHandleA':1,
 'GetModuleHandleW':1,'GetModuleHandleExA':3,'GetModuleHandleExW':3,'GetOEMCP':0,'GetProcAddress':2,'GetProcessHeap':0,
 'GetStartupInfoW':1,'GetStdHandle':1,'GetStringTypeW':4,'GetSystemTimeAsFileTime':1,'GetUserDefaultLCID':0,
 'HeapAlloc':3,'HeapFree':3,'HeapReAlloc':4,'HeapSize':3,'InitializeCriticalSectionAndSpinCount':2,'InitializeCriticalSectionEx':3,
 'InitializeSListHead':1,'InterlockedFlushSList':1,'IsDebuggerPresent':0,'IsProcessorFeaturePresent':1,'IsValidCodePage':1,
 'LCMapStringW':6,'LCMapStringEx':9,'LoadLibraryExW':3,'MultiByteToWideChar':6,'WideCharToMultiByte':8,'QueryPerformanceCounter':1,
 'ReadFile':5,'WriteFile':5,'ResetEvent':1,'SetEvent':1,'SetLastError':1,'SetFilePointerEx':4,'SetEndOfFile':1,'SetStdHandle':2,
 'SetUnhandledExceptionFilter':1,'TlsAlloc':0,'TlsFree':1,'TlsGetValue':1,'TlsSetValue':2,'VirtualProtect':4,
 'WaitForSingleObjectEx':3,'WritePrivateProfileStringA':4,'GetAsyncKeyState':1,'MessageBoxA':4,'RaiseException':4,
 'GetLocaleInfoW':4,'IsValidLocale':2,'EnumSystemLocalesW':2,'FindFirstFileExW':6,'FindNextFileW':2,'ReadConsoleW':5,
 'WriteConsoleW':5,'RtlUnwind':4,'TerminateProcess':2,'UnhandledExceptionFilter':1,'GetTickCount':0}
def on_code(uc, addr, size, user):
    if addr not in stubs: return
    n = stubs[addr]; calls.append(n); na = NARGS.get(n, DYN.get(n))
    if na is None: print('unhandled import', n); uc.emu_stop(); return
    a = [arg(uc, k) for k in range(na)]; v = 0
    if n in ('GetModuleHandleA', 'GetModuleHandleW'):
        nm = cstr(a[0], n.endswith('W')).lower() if a[0] else ''
        v = EXE_BASE if a[0] == 0 or nm.startswith('nfs') else (ASI_BASE if 'xtra' in nm else 0x7c800000)
    elif n in ('GetModuleHandleExA','GetModuleHandleExW'): w32(a[2], ASI_BASE); v = 1
    elif n in ('GetModuleFileNameA', 'GetModuleFileNameW'):
        p = 'C:\\Game\\nfs.exe' if a[0] in (0, EXE_BASE) else 'C:\\Game\\scripts\\NFSUCExtraOptions.asi'
        e = p.encode('utf-16le' if n.endswith('W') else 'latin1') + (b'\0\0' if n.endswith('W') else b'\0')
        mu.mem_write(a[1], e[:a[2] * (2 if n.endswith('W') else 1)]); v = len(p)
    elif n == 'GetProcAddress':
        nm = cstr(a[1]) if a[1] > 0xffff else '#%d' % a[1]
        v = 0
        if nm in NARGS or nm in DYN:
            v = STUB + 16 * (0x800 + len(stubs)); mu.mem_write(v, b'\xc3'); stubs[v] = nm
        else: print('GetProcAddress (none)', nm)
    elif n == 'GetProcessHeap': v = 0x5000
    elif n == 'HeapAlloc':
        v = alloc(a[2]); mu.mem_write(v, b'\0' * a[2])
    elif n == 'HeapReAlloc':
        v = alloc(a[3]); mu.mem_write(v, rd(a[2], a[3]))
    elif n == 'HeapSize': v = 0x10000
    elif n in ('HeapFree','InitializeCriticalSectionAndSpinCount','InitializeCriticalSectionEx','IsValidCodePage','FlushFileBuffers',
               'VirtualProtect','SetEvent','ResetEvent','SetEndOfFile','CloseHandle','TlsFree','TlsSetValue','IsValidLocale','FreeEnvironmentStringsW','SetStdHandle'):
        v = 1
        if n == 'VirtualProtect' and a[3]: w32(a[3], 0x20)
        if n == 'TlsSetValue': tls[a[0]] = a[1]
    elif n in ('TlsAlloc','FlsAlloc'): ntls[0] += 1; v = ntls[0]
    elif n == 'FlsGetValue': v = tls.get(a[0], 0)
    elif n == 'FlsSetValue': tls[a[0]] = a[1]; v = 1
    elif n in ('FlsFree','SetThreadStackGuarantee','InitOnceExecuteOnce'): v = 1
    elif n == 'GetTickCount64': v = 1000
    elif n in ('InitializeConditionVariable','WakeAllConditionVariable'): v = 0
    elif n == 'AreFileApisANSI': v = 1
    elif n == 'TlsGetValue': v = tls.get(a[0], 0)
    elif n in ('EncodePointer','DecodePointer'): v = a[0]
    elif n == 'GetLastError': v = last_err[0]
    elif n == 'SetLastError': last_err[0] = a[0]
    elif n == 'GetACP': v = 1252
    elif n == 'GetOEMCP': v = 437
    elif n == 'GetCPInfo': mu.mem_write(a[1], struct.pack('<I', 1) + b'?\0' + b'\0' * 12); v = 1
    elif n == 'GetCommandLineA':
        v = alloc(32); mu.mem_write(v, b'nfs.exe\0')
    elif n == 'GetCommandLineW':
        v = alloc(32); mu.mem_write(v, 'nfs.exe\0'.encode('utf-16le'))
    elif n == 'GetEnvironmentStringsW':
        v = alloc(16); mu.mem_write(v, b'\0' * 8)
    elif n == 'GetStdHandle': v = 0
    elif n == 'GetFileType': v = 1
    elif n in ('GetCurrentProcessId','GetCurrentThreadId'): v = 4
    elif n == 'GetCurrentProcess': v = 0xffffffff
    elif n == 'GetSystemTimeAsFileTime': w32(a[0], 0); w32(a[0] + 4, 0x01d00000)
    elif n == 'QueryPerformanceCounter': w32(a[0], 1000); w32(a[0] + 4, 0); v = 1
    elif n == 'GetStartupInfoW': mu.mem_write(a[0], struct.pack('<I', 68) + b'\0' * 64)
    elif n == 'IsProcessorFeaturePresent': v = 1 if a[0] in (10, 6, 23) else 0
    elif n == 'GetUserDefaultLCID': v = 0x409
    elif n == 'MultiByteToWideChar':
        cp, fl, src, cb, dst, cch = a
        s = rd(src, cb) if cb != 0xffffffff else (cstr(src).encode('latin1') + b'\0')
        w = s.decode('latin1').encode('utf-16le'); v = len(w) // 2
        if cch: mu.mem_write(dst, w[:2 * cch])
    elif n == 'WideCharToMultiByte':
        cp, fl, src, cch, dst, cb = a[:6]
        s = rd(src, 2 * cch) if cch != 0xffffffff else (cstr(src, True).encode('utf-16le') + b'\0\0')
        m = s.decode('utf-16le').encode('latin1', 'replace'); v = len(m)
        if cb: mu.mem_write(dst, m[:cb])
    elif n == 'GetStringTypeW':
        for k in range(a[2] if a[2] != 0xffffffff else 0): mu.mem_write(a[3] + 2 * k, struct.pack('<H', 0x100 | 0x8)); 
        v = 1
    elif n == 'LCMapStringW':
        src, cch, dst, cchd = a[2], a[3], a[4], a[5]; v = cch
        if cchd: mu.mem_write(dst, rd(src, 2 * cch))
    elif n == 'LCMapStringEx':
        src, cch, dst, cchd = a[2], a[3], a[4], a[5]; v = cch
        if cchd: mu.mem_write(dst, rd(src, 2 * cch))
    elif n == 'CreateFileW':
        p = cstr(a[0], True); hp = host_path(p)
        if hp and not (a[1] & 0x40000000):
            h = nexth[0]; nexth[0] += 4; files[h] = [open(hp, 'rb').read(), 0]; v = h
        else:
            v = 0xffffffff; last_err[0] = 2
        print('CreateFileW', p, '->', hp, hex(a[1]))
    elif n == 'ReadFile':
        f = files.get(a[0])
        if f:
            d = f[0][f[1]:f[1] + a[2]]; f[1] += len(d); mu.mem_write(a[1], d)
            if a[3]: w32(a[3], len(d))
            v = 1
    elif n == 'WriteFile':
        if a[3]: w32(a[3], a[2])
        v = 1
    elif n == 'GetFileSizeEx':
        f = files.get(a[0]); w32(a[1], len(f[0]) if f else 0); w32(a[1] + 4, 0); v = 1
    elif n == 'SetFilePointerEx':
        f = files.get(a[0]); lo = a[1]; mv = a[3]
        if f:
            f[1] = lo if mv == 0 else (f[1] + lo if mv == 1 else len(f[0]) + lo)
            if a[3 - 1]: pass
            v = 1
        if a[2]: w32(a[2], f[1] if f else 0); w32(a[2] + 4, 0)
    elif n == 'GetAsyncKeyState': v = 0
    elif n == 'MessageBoxA': print('MessageBox:', cstr(a[1]), '/', cstr(a[2])); v = 1
    elif n == 'CreateEventW': v = 0x900
    elif n == 'WaitForSingleObjectEx': v = 0
    elif n in ('ExitProcess', 'TerminateProcess', 'RaiseException'):
        print(n, [hex(x) for x in a]); uc.emu_stop(); return
    elif n == 'GetConsoleMode': v = 0
    elif n == 'WritePrivateProfileStringA': v = 1
    elif n == 'LoadLibraryExW': v = 0
    elif n == 'FindFirstFileExW': v = 0xffffffff
    ret(uc, v, na)
mu.hook_add(UC_HOOK_CODE, on_code, begin=STUB, end=STUB + 0x10000)
def on_bad(uc, access, addr, size, value, user):
    print('bad access', access, hex(addr), 'eip', hex(uc.reg_read(UC_X86_REG_EIP))); return False
mu.hook_add(UC_HOOK_MEM_UNMAPPED, on_bad)
mu.reg_write(UC_X86_REG_FPCW, 0x27f)
try: mu.reg_write(UC_X86_REG_MXCSR, 0x1f80)
except Exception as e: print('mxcsr', e)
# call DllMain(ASI_BASE, 1, 0) returning to a sentinel
END = STUB + 0xff00; mu.mem_write(END, b'\xf4')
esp = mu.reg_read(UC_X86_REG_ESP) - 16
mu.mem_write(esp, struct.pack('<4I', END, ASI_BASE, 1, 0)); mu.reg_write(UC_X86_REG_ESP, esp)
try:
    mu.emu_start(ASI_BASE + ASI_EP, END, count=200_000_000)
except UcError as e:
    print('stopped:', e, hex(mu.reg_read(UC_X86_REG_EIP)))
print('last imports', calls[-15:]); ebp=mu.reg_read(UC_X86_REG_EBP)
for k in range(8):
    try: print('frame ret', hex(u32(ebp+4))); ebp=u32(ebp)
    except Exception: break
print('eax', hex(mu.reg_read(UC_X86_REG_EAX)), 'imports called', len(calls), 'writes into the exe', len(writes))
# merge into runs
runs = {}
for addr, size, val, old, eip in writes:
    for k in range(size):
        runs[addr + k] = ((val >> (8 * k)) & 0xff, int(old[2 * k:2 * k + 2], 16), eip)
rows = []
for a in sorted(runs):
    if rows and rows[-1]['end'] == a:
        rows[-1]['end'] += 1; rows[-1]['new'] += '%02x' % runs[a][0]; rows[-1]['old'] += '%02x' % runs[a][1]
    else:
        rows.append({'addr': a, 'end': a + 1, 'new': '%02x' % runs[a][0], 'old': '%02x' % runs[a][1], 'by': hex(runs[a][2])})
for r in rows: r['addr'] = hex(r['addr']); r['end'] = hex(r['end'])
for da in filter(None, os.environ.get('DUMP','').split(',')): print('dump', da, rd(int(da,16), 16).hex())
if out_path: json.dump(rows, open(out_path, 'w'), indent=0)
for r in rows: print(r['addr'], r['old'], '->', r['new'], 'by', r['by'])
