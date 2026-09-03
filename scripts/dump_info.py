# -*- coding: utf-8 -*-
# ============================================================================
#  scripts/dump_info.py — .dmp 를 열어서 「무엇이 들어 있나」를 읽는다 (L16 검증)
#
#  왜 만들었나 — 이 장비에 WinDbg/cdb 가 없다. Visual Studio 로 열 수는 있지만
#    그건 사람이 클릭하는 일이라 자동 검증이 안 된다.
#    「파일이 0바이트가 아니다」까지만 확인하고 넘어가면 그건 검증이 아니다 —
#    「파일이 있는데 열리지 않는다」가 운영에서 제일 시간을 잡아먹는 상태다.
#
#  그래서 minidump 포맷을 직접 읽는다. 확인하는 것 —
#      · 스트림 목록      : 디버거가 필요로 하는 게 다 들어 있나
#      · 예외 코드/주소   : 무엇으로 죽었나 · 그 주소가 village.exe 안인가
#      · 스레드 목록      : 14개(I/O 4 · 워커 8 · 틱 1 · 로거 1)가 다 잡혔나 —
#        옛 값 13개(I/O 4 · 존 스레드 4 · DB 4 · 로거 1)는 존 스레드/전용 DB 워커 폐기로 낡았다
#      · 콜스택(심볼)     : dbghelp 로 village.pdb 를 붙여 함수 이름을 읽는다
#
#  콜스택은 「진짜 unwind」가 아니라 **스택 메모리 훑기**다.
#    x64 정식 unwind 는 .pdb 의 unwind info 를 따라가야 하는데 그건 디버거의 일이다.
#    여기서는 스택에 남은 8바이트 값 중 village.exe 코드 범위에 드는 것을 모아 심볼로 바꾼다.
#    → **호출 흔적**이지 정확한 프레임 순서가 아니다. 죽은 자리를 찾는 데는 충분하다.
#
#  쓰기 :  python scripts\dump_info.py dumps\server_....dmp
# ============================================================================
import ctypes
import os
import struct
import sys

STREAM_NAMES = {
    3: 'ThreadListStream', 4: 'ModuleListStream', 5: 'MemoryListStream',
    6: 'ExceptionStream', 7: 'SystemInfoStream', 8: 'ThreadExListStream',
    9: 'Memory64ListStream', 15: 'MiscInfoStream', 16: 'MemoryInfoListStream',
    17: 'ThreadInfoListStream', 22: 'SystemMemoryInfoStream',
    23: 'ProcessVmCountersStream',
}

EXC_NAMES = {
    0xC0000005: 'ACCESS_VIOLATION',
    0xC00000FD: 'STACK_OVERFLOW',
    0xC0000094: 'INT_DIVIDE_BY_ZERO',
    0xC0000374: 'HEAP_CORRUPTION',
    0xE06D7363: 'C++ EXCEPTION (MSVC "msc")',
    0x80000003: 'BREAKPOINT',
}


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def u64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def mdstring(b, rva):
    n = u32(b, rva)
    return b[rva + 4: rva + 4 + n].decode('utf-16-le', 'replace')


def main(path):
    with open(path, 'rb') as f:
        b = f.read()

    if b[:4] != b'MDMP':
        print('  [X] MDMP 시그니처가 없다 — 덤프가 아니다')
        return 1

    n_streams = u32(b, 8)
    dir_rva = u32(b, 12)
    print('파일        : %s  (%s bytes)' % (os.path.basename(path), format(len(b), ',')))
    print('스트림      : %d 개' % n_streams)

    streams = {}
    for i in range(n_streams):
        o = dir_rva + i * 12
        t, size, rva = u32(b, o), u32(b, o + 4), u32(b, o + 8)
        streams[t] = (size, rva)
    print('              ' + ', '.join(
        STREAM_NAMES.get(t, str(t)) for t in sorted(streams) if t != 0))

    # ---- 모듈 -------------------------------------------------------------
    exe_base = exe_size = 0
    exe_path = ''
    if 4 in streams:
        _, rva = streams[4]
        n = u32(b, rva)
        for i in range(n):
            o = rva + 4 + i * 108
            base, size, name_rva = u64(b, o), u32(b, o + 8), u32(b, o + 20)
            name = mdstring(b, name_rva)
            if name.lower().endswith('village.exe'):
                exe_base, exe_size, exe_path = base, size, name
        print('모듈        : %d 개   village.exe base=0x%X size=0x%X' %
              (n, exe_base, exe_size))

    # ---- 예외 -------------------------------------------------------------
    fault_tid = 0
    fault_addr = 0
    if 6 in streams:
        _, rva = streams[6]
        fault_tid = u32(b, rva)
        er = rva + 8
        code = u32(b, er)
        addr = u64(b, er + 16)
        fault_addr = addr
        inside = exe_base <= addr < exe_base + exe_size
        print('예외        : 0x%08X  %s' % (code, EXC_NAMES.get(code, '?')))
        print('              주소 0x%X  thread %d   %s' % (
            addr, fault_tid,
            ('village.exe+0x%X  ★ 우리 코드 안이다' % (addr - exe_base))
            if inside else '(village.exe 밖 — 라이브러리/시스템)'))
    else:
        print('예외        : ExceptionStream 없음 '
              '(terminate 경로는 예외 기록이 없다 — 정상)')

    # ---- 스레드 -----------------------------------------------------------
    threads = []
    if 3 in streams:
        _, rva = streams[3]
        n = u32(b, rva)
        # MINIDUMP_THREAD (48B) — 오프셋을 한 번 틀리면 엉뚱한 값이 나온다.
        #   0 ThreadId · 4 SuspendCount · 8 PriorityClass · 12 Priority
        #   16 Teb(8)  ← 여기를 스택 주소로 잘못 읽어서 RIP 가 0x409 로 나왔었다
        #   24 Stack.StartOfMemoryRange(8) · 32 Stack.DataSize(4) · 36 Stack.Rva(4)
        #   40 ThreadContext.DataSize(4)   · 44 ThreadContext.Rva(4)
        for i in range(n):
            o = rva + 4 + i * 48
            tid = u32(b, o)
            stack_start = u64(b, o + 24)
            stack_size = u32(b, o + 32)
            stack_rva = u32(b, o + 36)
            ctx_size = u32(b, o + 40)
            ctx_rva = u32(b, o + 44)
            rip = rsp = 0
            if ctx_size >= 0x100:
                rsp = u64(b, ctx_rva + 0x98)     # CONTEXT_AMD64.Rsp
                rip = u64(b, ctx_rva + 0xF8)     # CONTEXT_AMD64.Rip
            threads.append((tid, rip, rsp, stack_start, stack_size, stack_rva))
        print('스레드      : %d 개  '
              '(우리가 만든 것 14 = I/O 4 + 워커 8 + 틱 1 + 로거 1, '
              '나머지는 main · CRT · 시스템)' % n)

    # ---- 심볼 -------------------------------------------------------------
    pdb = os.path.join(os.path.dirname(os.path.abspath(path)), '..',
                       'build', 'x64', 'Debug', 'village.pdb')
    sym = Symbolizer(exe_path, exe_base, exe_size)
    print('심볼        : %s' % sym.status)

    # ---- 죽은 스레드의 스택 훑기 ------------------------------------------
    target = None
    for t in threads:
        if fault_tid and t[0] == fault_tid:
            target = t
    if target is None and threads:
        target = threads[0]

    if target and sym.ok and exe_base:
        tid, rip, rsp, ss, ssz, srva = target
        print('')
        # 여기가 이 도구의 「본전」이다 — 예외 주소를 함수 이름으로 바꾼다.
        #   스레드의 RIP 는 「지금 어디를 돌고 있나」(핸들러 안)라서 죽은 자리가 아니다.
        #     죽은 자리는 ExceptionRecord 의 ExceptionAddress 다. 둘을 헷갈리면
        #     "크래시가 핸들러에서 났다"는 엉뚱한 결론에 간다.
        if fault_addr and exe_base <= fault_addr < exe_base + exe_size:
            print('★ 죽은 자리 : %s' % sym.name(fault_addr))
        print('죽은 스레드 %d 의 스택 훑기  (⚠️ 정식 unwind 가 아니라 흔적 수집)' % tid)
        if rip:
            print('  현재 RIP (핸들러 실행 중)  %s' % sym.name(rip))
        seen, hits = set(), 0
        stack = b[srva: srva + ssz]
        for off in range(0, len(stack) - 8, 8):
            v = struct.unpack_from('<Q', stack, off)[0]
            if exe_base <= v < exe_base + exe_size:
                nm = sym.name(v)
                key = nm.split('+')[0]
                if key in seen:
                    continue
                seen.add(key)
                hits += 1
                print('  [%2d] %s' % (hits, nm))
                if hits >= 25:
                    print('  ... (25개에서 끊음)')
                    break
        if hits == 0:
            print('  (village.exe 범위의 주소를 스택에서 못 찾았다)')
    return 0


class Symbolizer(object):
    """dbghelp 로 village.exe + village.pdb 를 붙여 주소 → 함수 이름."""

    def __init__(self, exe_path, base, size):
        self.ok = False
        self.status = '안 붙음'
        if not base or not exe_path or not os.path.exists(exe_path):
            self.status = 'village.exe 원본을 못 찾음 (%s)' % exe_path
            return
        try:
            self.dbg = ctypes.WinDLL('dbghelp.dll')
            self.h = ctypes.c_void_p(0x1234)          # 가짜 핸들 — 오프라인 모드
            self.dbg.SymSetOptions(0x00000200 | 0x00000004)   # UNDNAME | DEFERRED
            if not self.dbg.SymInitialize(self.h, None, False):
                self.status = 'SymInitialize 실패'
                return
            self.dbg.SymLoadModuleExW.restype = ctypes.c_uint64
            got = self.dbg.SymLoadModuleExW(
                self.h, None, ctypes.c_wchar_p(exe_path), None,
                ctypes.c_uint64(base), ctypes.c_uint32(size), None, 0)
            if not got:
                self.status = 'SymLoadModuleEx 실패 (err=%d)' % ctypes.GetLastError()
                return
            self.ok = True
            self.status = 'village.exe + pdb 로드됨 (base 0x%X)' % base
        except Exception as e:                      # noqa
            self.status = 'dbghelp 사용 불가: %s' % e

    def name(self, addr):
        if not self.ok:
            return '0x%X' % addr

        class SYMBOL_INFO(ctypes.Structure):
            _fields_ = [('SizeOfStruct', ctypes.c_uint32),
                        ('TypeIndex', ctypes.c_uint32),
                        ('Reserved', ctypes.c_uint64 * 2),
                        ('Index', ctypes.c_uint32),
                        ('Size', ctypes.c_uint32),
                        ('ModBase', ctypes.c_uint64),
                        ('Flags', ctypes.c_uint32),
                        ('Value', ctypes.c_uint64),
                        ('Address', ctypes.c_uint64),
                        ('Register', ctypes.c_uint32),
                        ('Scope', ctypes.c_uint32),
                        ('Tag', ctypes.c_uint32),
                        ('NameLen', ctypes.c_uint32),
                        ('MaxNameLen', ctypes.c_uint32),
                        ('Name', ctypes.c_char * 1024)]

        s = SYMBOL_INFO()
        s.SizeOfStruct = 88
        s.MaxNameLen = 1024
        disp = ctypes.c_uint64(0)
        if self.dbg.SymFromAddr(self.h, ctypes.c_uint64(addr),
                                ctypes.byref(disp), ctypes.byref(s)):
            nm = s.Name.decode('utf-8', 'replace')
            return '%s + 0x%X' % (nm, disp.value) if disp.value else nm
        return '0x%X' % addr


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('usage: python scripts/dump_info.py <file.dmp>')
        sys.exit(2)
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:                                # noqa
        pass
    sys.exit(main(sys.argv[1]))
