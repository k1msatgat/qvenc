#!/usr/bin/env python3
"""
qvenc BAR0 MMIO 검증 도구 (HW팀).

이것은 드라이버가 아니다. 커널 모듈 없이 게스트에서 sysfs 의 resource0 을 직접
mmap 해서, 장치가 docs/regspec.md 대로 동작하는지 확인한다. SW팀 드라이버가
probe 를 짜기 전에 "칩이 제대로 붙어 있는지" 를 먼저 못 박아 두는 용도다.

게스트에서 root 로 실행:
    python3 /mnt/qvenc/tests/mmio_probe.py
    python3 /mnt/qvenc/tests/mmio_probe.py --bdf 0000:00:04.0 -v

레지스터 상수는 include/qvenc_regs.h 에서 직접 읽어 온다. 테스트가 계약에서
따로 놀지 않게 하기 위해서다. 헤더를 못 찾으면 거기서 멈춘다.
"""

import argparse
import ctypes
import glob
import mmap
import os
import re
import sys

# ---------------------------------------------------------------- #
# include/qvenc_regs.h 파싱                                          #
# ---------------------------------------------------------------- #

_DEFINE_RE = re.compile(r'^\s*#\s*define\s+(QVENC_[A-Z0-9_]+)\s+(.+?)\s*(?:/\*.*)?$')
_SAFE_EXPR_RE = re.compile(r'^[0-9A-Za-z_x\s()|<&+~-]*$')


def load_regs(header_path):
    """헤더의 #define 들을 이름 -> 정수 로 푼다 (함수형 매크로는 건너뛴다)."""
    raw = {}
    with open(header_path) as fh:
        text = fh.read()

    # 줄 끝 백슬래시로 이어진 정의를 한 줄로 합친다
    text = re.sub(r'\\\s*\n\s*', ' ', text)
    # 블록 주석 제거
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)

    for line in text.splitlines():
        m = _DEFINE_RE.match(line)
        if not m:
            continue
        name, expr = m.group(1), m.group(2).strip()
        if '(' in name or not expr:
            continue
        raw[name] = expr

    resolved = {}
    # 이름이 서로를 참조하므로 더 이상 안 풀릴 때까지 반복한다
    for _ in range(8):
        progress = False
        for name, expr in raw.items():
            if name in resolved:
                continue
            py = re.sub(r'\b(0x[0-9a-fA-F]+|\d+)[uU][lL]*\b', r'\1', expr)
            if not _SAFE_EXPR_RE.match(py):
                continue
            try:
                resolved[name] = int(eval(py, {'__builtins__': {}}, dict(resolved)))
                progress = True
            except Exception:
                pass
        if not progress:
            break

    missing = [n for n in raw if n not in resolved]
    return resolved, missing


def find_header(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, '..', 'include', 'qvenc_regs.h'),
                 '/mnt/qvenc/include/qvenc_regs.h'):
        cand = os.path.normpath(cand)
        if os.path.isfile(cand):
            return cand
    return None


# ---------------------------------------------------------------- #
# PCI 장치 찾기 / BAR0 매핑                                          #
# ---------------------------------------------------------------- #

SYSFS_PCI = '/sys/bus/pci/devices'


def _read_sysfs(path):
    with open(path) as fh:
        return fh.read().strip()


def find_device(vendor, device, bdf=None):
    if bdf:
        path = os.path.join(SYSFS_PCI, bdf)
        if not os.path.isdir(path):
            raise RuntimeError('그런 PCI 장치가 없다: %s' % bdf)
        return path

    found = []
    for path in sorted(glob.glob(os.path.join(SYSFS_PCI, '*'))):
        try:
            v = int(_read_sysfs(os.path.join(path, 'vendor')), 16)
            d = int(_read_sysfs(os.path.join(path, 'device')), 16)
        except (OSError, ValueError):
            continue
        if v == vendor and d == device:
            found.append(path)

    if not found:
        raise RuntimeError(
            'qvenc(%04x:%04x) 장치를 못 찾았다.\n'
            '  QEMU 를 -device qvenc 로 띄웠는지 확인할 것.' % (vendor, device))
    if len(found) > 1:
        print('[warn] qvenc 가 %d개 보인다. 첫 번째를 쓴다. --bdf 로 지정 가능.'
              % len(found))
    return found[0]


PCI_COMMAND = 0x04
PCI_COMMAND_MEMORY = 0x02
PCI_COMMAND_MASTER = 0x04


def read_command(devpath):
    with open(os.path.join(devpath, 'config'), 'rb') as fh:
        fh.seek(PCI_COMMAND)
        return int.from_bytes(fh.read(2), 'little')


def write_command(devpath, value):
    with open(os.path.join(devpath, 'config'), 'r+b') as fh:
        fh.seek(PCI_COMMAND)
        fh.write((value & 0xffff).to_bytes(2, 'little'))


def set_bus_master(devpath, enable):
    """
    PCI COMMAND 의 Bus Master Enable 비트를 켜고 끈다.

    드라이버의 pci_set_master() 가 하는 일이다. 이 비트가 꺼져 있으면 장치가
    DMA 를 할 수 없고, qvenc 는 그 상태의 START 를 ERR_NO_BUS_MASTER 로 거절한다.
    """
    cmd = read_command(devpath)
    new = cmd | PCI_COMMAND_MASTER if enable else cmd & ~PCI_COMMAND_MASTER
    if new != cmd:
        write_command(devpath, new)
    return bool(read_command(devpath) & PCI_COMMAND_MASTER)


def ensure_memory_decode(devpath, verbose=False):
    """
    BAR 의 메모리 디코딩이 꺼져 있으면 MMIO 가 전부 0xffffffff 로 보인다.
    드라이버가 안 붙은 장치는 꺼져 있을 수 있으므로 여기서 켠다.
    (드라이버의 pci_enable_device() 가 하는 일의 일부를 손으로 하는 셈이다.)
    """
    cfg = os.path.join(devpath, 'config')
    with open(cfg, 'rb') as fh:
        fh.seek(PCI_COMMAND)
        cmd = int.from_bytes(fh.read(2), 'little')

    if cmd & PCI_COMMAND_MEMORY:
        if verbose:
            print('[info] memory decode 이미 켜져 있음 (COMMAND=0x%04x)' % cmd)
        return

    print('[info] memory decode 가 꺼져 있다 (COMMAND=0x%04x). 켠다.' % cmd)
    try:
        with open(os.path.join(devpath, 'enable'), 'w') as fh:
            fh.write('1')
    except OSError:
        pass

    with open(cfg, 'r+b') as fh:
        fh.seek(PCI_COMMAND)
        cmd = int.from_bytes(fh.read(2), 'little')
        if not (cmd & PCI_COMMAND_MEMORY):
            fh.seek(PCI_COMMAND)
            fh.write((cmd | PCI_COMMAND_MEMORY).to_bytes(2, 'little'))
            fh.seek(PCI_COMMAND)
            cmd = int.from_bytes(fh.read(2), 'little')

    if not (cmd & PCI_COMMAND_MEMORY):
        raise RuntimeError('memory decode 를 못 켰다 (COMMAND=0x%04x)' % cmd)


class Bar:
    """
    BAR0 에 대한 32/64비트 접근기.

    mmap 슬라이싱(mm[0:4]) 대신 ctypes 를 쓴다. 슬라이싱은 memcpy 를 타기 때문에
    바이트 단위 접근으로 쪼개질 수 있고, 그러면 장치가 ERR_ALIGN 을 올려 버린다.
    ctypes 는 지정한 폭 그대로 한 번에 읽고 쓴다.
    """

    def __init__(self, devpath, size):
        self.path = os.path.join(devpath, 'resource0')
        self.size = size
        self.fd = os.open(self.path, os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(self.fd, size, mmap.MAP_SHARED,
                            mmap.PROT_READ | mmap.PROT_WRITE)

    def close(self):
        self.mm.close()
        os.close(self.fd)

    def r32(self, off):
        return ctypes.c_uint32.from_buffer(self.mm, off).value

    def w32(self, off, val):
        ctypes.c_uint32.from_buffer(self.mm, off).value = val & 0xffffffff

    def r64(self, off):
        return ctypes.c_uint64.from_buffer(self.mm, off).value


# ---------------------------------------------------------------- #
# 테스트                                                             #
# ---------------------------------------------------------------- #

class Probe:
    def __init__(self, bar, R, verbose, devpath=None):
        self.bar = bar
        self.R = R
        self.verbose = verbose
        self.devpath = devpath
        self.passed = 0
        self.failed = 0
        self.failures = []

    # -- 보고 ------------------------------------------------------
    def check(self, name, ok, detail=''):
        if ok:
            self.passed += 1
            print('  PASS  %-34s %s' % (name, detail if self.verbose else ''))
        else:
            self.failed += 1
            self.failures.append('%s: %s' % (name, detail))
            print('  FAIL  %-34s %s' % (name, detail))
        return ok

    def eq(self, name, got, want):
        return self.check(name, got == want,
                          'got=0x%08x want=0x%08x' % (got, want))

    # -- 상태 보조 -------------------------------------------------
    def status(self):
        return self.bar.r32(self.R['QVENC_REG_STATUS'])

    def errcode(self):
        return self.bar.r32(self.R['QVENC_REG_ERR_CODE'])

    def clear(self):
        """에러/완료 플래그를 지워 다음 테스트를 깨끗한 상태에서 시작한다."""
        self.bar.w32(self.R['QVENC_REG_IRQ_ACK'],
                     self.R['QVENC_IRQ_ACK_DONE'] | self.R['QVENC_IRQ_ACK_ERROR'])

    def expect_err(self, name, code_name):
        """STATUS.ERROR 가 섰고 ERR_CODE 가 기대값인지 확인한 뒤 지운다."""
        want = self.R[code_name]
        st, ec = self.status(), self.errcode()
        ok = bool(st & self.R['QVENC_STATUS_ERROR']) and ec == want
        self.check(name, ok, 'STATUS=0x%08x ERR_CODE=0x%02x (want %s=0x%02x)'
                   % (st, ec, code_name, want))
        self.clear()
        return ok

    def expect_clean(self, name):
        st, ec = self.status(), self.errcode()
        return self.check(name, st == 0 and ec == 0,
                          'STATUS=0x%08x ERR_CODE=0x%02x' % (st, ec))

    # -- 개별 테스트 -----------------------------------------------
    def t_id(self):
        print('\n[1] ID 레지스터')
        v = self.bar.r32(self.R['QVENC_REG_ID'])
        if v == 0xffffffff:
            self.check('ID readable', False,
                       '0xffffffff — BAR 가 디코딩되지 않는다')
            return
        self.eq('ID == QVENC_ID_VALUE', v, self.R['QVENC_ID_VALUE'])
        magic = (v >> self.R['QVENC_ID_MAGIC_SHIFT']) & 0xffff
        self.eq('ID.MAGIC', magic, self.R['QVENC_ID_MAGIC'])
        major = (v >> self.R['QVENC_ID_VER_MAJOR_SHIFT']) & 0xff
        minor = (v >> self.R['QVENC_ID_VER_MINOR_SHIFT']) & 0xff
        self.check('ID.VERSION', (major, minor) ==
                   (self.R['QVENC_ID_VER_MAJOR'], self.R['QVENC_ID_VER_MINOR']),
                   'v%d.%d' % (major, minor))

    def t_reset_values(self):
        print('\n[2] CTRL.RESET 후 리셋 값')
        self.bar.w32(self.R['QVENC_REG_CTRL'], self.R['QVENC_CTRL_RESET'])
        self.expect_clean('RESET 후 STATUS/ERR_CODE 가 0')
        for reg, rst in (('CTRL', 'CTRL'), ('SRC_LO', 'SRC_LO'), ('SRC_HI', 'SRC_HI'),
                         ('DST_LO', 'DST_LO'), ('DST_HI', 'DST_HI'),
                         ('DST_SIZE', 'DST_SIZE'), ('OUT_LEN', 'OUT_LEN'),
                         ('WIDTH', 'WIDTH'), ('HEIGHT', 'HEIGHT'),
                         ('QUALITY', 'QUALITY')):
            self.eq('%s 리셋 값' % reg,
                    self.bar.r32(self.R['QVENC_REG_' + reg]),
                    self.R['QVENC_RESET_' + rst])

    def t_roundtrip(self):
        print('\n[3] RW 레지스터 왕복')
        patterns = {
            'SRC_LO':   0xdeadbeef,
            'SRC_HI':   0x00000001,
            'DST_LO':   0xcafe0000,
            'DST_HI':   0x00000002,
            'DST_SIZE': 0x00100000,
            'WIDTH':    640,
            'HEIGHT':   480,
            'QUALITY':  90,
        }
        for reg, val in patterns.items():
            off = self.R['QVENC_REG_' + reg]
            self.bar.w32(off, val)
            self.eq('%s 왕복' % reg, self.bar.r32(off), val)
        self.expect_clean('왕복 중 에러 없음')

        # 전부 1 / 전부 0 패턴 (비트 고착 확인)
        for reg in ('SRC_LO', 'DST_LO', 'DST_SIZE'):
            off = self.R['QVENC_REG_' + reg]
            for val in (0xffffffff, 0x00000000, 0x55555555, 0xaaaaaaaa):
                self.bar.w32(off, val)
                if self.bar.r32(off) != val:
                    self.check('%s 비트 패턴 0x%08x' % (reg, val), False, '')
                    break
            else:
                self.check('%s 비트 패턴 4종' % reg, True, '')
        self.clear()

    def t_ctrl_bits(self):
        print('\n[4] CTRL 비트 동작')
        ctrl = self.R['QVENC_REG_CTRL']
        self.bar.w32(ctrl, self.R['QVENC_CTRL_IRQ_EN'])
        self.eq('IRQ_EN 래치됨', self.bar.r32(ctrl), self.R['QVENC_CTRL_IRQ_EN'])
        self.expect_clean('IRQ_EN 쓰기에 에러 없음')

        # START 는 self-clearing 이라 항상 0 으로 읽힌다
        self.bar.w32(ctrl, self.R['QVENC_CTRL_IRQ_EN'] | self.R['QVENC_CTRL_START'])
        self.eq('START 는 0 으로 읽힘', self.bar.r32(ctrl),
                self.R['QVENC_CTRL_IRQ_EN'])
        self.clear()

        self.bar.w32(ctrl, self.R['QVENC_CTRL_RESET'])
        self.eq('RESET 후 CTRL 은 0', self.bar.r32(ctrl), 0)

    def t_start_rejects(self):
        """
        START 의 거절 경로만 본다. 실제 DMA 는 tests/dma_probe.py 가 맡는다.
        검사 순서는 regspec 3.8: BUSY -> BUS_MASTER -> BAD_PARAM -> ...
        """
        print('\n[5] START 거절 경로')
        ctrl = self.R['QVENC_REG_CTRL']
        start = self.R['QVENC_CTRL_START']

        if self.devpath is None:
            print('  SKIP  PCI config 접근 불가')
            return

        # bus master 가 꺼진 상태: 파라미터를 보기도 전에 거절해야 한다
        set_bus_master(self.devpath, False)
        self.bar.w32(ctrl, self.R['QVENC_CTRL_RESET'])
        self.bar.w32(ctrl, start)
        self.expect_err('bus master 꺼짐 -> ERR_NO_BUS_MASTER',
                        'QVENC_ERR_NO_BUS_MASTER')

        # bus master 를 켜면 이제 파라미터 검사까지 간다. RESET 직후 WIDTH=0 이다.
        set_bus_master(self.devpath, True)
        self.bar.w32(ctrl, start)
        self.expect_err('WIDTH=0 -> ERR_BAD_PARAM', 'QVENC_ERR_BAD_PARAM')

        # 16 의 배수가 아닌 크기
        self.bar.w32(self.R['QVENC_REG_WIDTH'], 24)
        self.bar.w32(self.R['QVENC_REG_HEIGHT'], 64)
        self.bar.w32(ctrl, start)
        self.expect_err('WIDTH=24 (16배수 아님) -> ERR_BAD_PARAM',
                        'QVENC_ERR_BAD_PARAM')

        # 크기는 맞고 DST_SIZE 가 모자람
        self.bar.w32(self.R['QVENC_REG_WIDTH'], 64)
        self.bar.w32(self.R['QVENC_REG_DST_SIZE'], 1)
        self.bar.w32(ctrl, start)
        self.expect_err('DST_SIZE 부족 -> ERR_DST_SMALL', 'QVENC_ERR_DST_SMALL')

        self.bar.w32(ctrl, self.R['QVENC_CTRL_RESET'])
        self.expect_clean('RESET 으로 정리됨')

    def t_ro_write(self):
        print('\n[6] 읽기 전용 위반')
        for reg in ('ID', 'STATUS', 'OUT_LEN'):
            off = self.R['QVENC_REG_' + reg]
            before = self.bar.r32(off)
            self.bar.w32(off, 0x5a5a5a5a)
            # 에러를 먼저 지우고 나서 되읽는다. STATUS 는 거부된 쓰기 때문이 아니라
            # ERROR 플래그 때문에 값이 달라지므로, 지우기 전에 비교하면 안 된다.
            self.expect_err('%s 쓰기 -> ERR_RO_WRITE' % reg, 'QVENC_ERR_RO_WRITE')
            self.eq('%s 값이 안 바뀜' % reg, self.bar.r32(off), before)

    def t_wo_read(self):
        print('\n[7] 쓰기 전용 위반')
        v = self.bar.r32(self.R['QVENC_REG_IRQ_ACK'])
        self.eq('IRQ_ACK 읽기 -> 0xffffffff', v, 0xffffffff)
        self.expect_err('IRQ_ACK 읽기 -> ERR_WO_READ', 'QVENC_ERR_WO_READ')

    def t_bad_offset(self):
        print('\n[8] 정의되지 않은 오프셋')
        for off in (self.R['QVENC_REG_END'], 0x100, 0xffc):
            v = self.bar.r32(off)
            self.eq('0x%03x 읽기 -> 0xffffffff' % off, v, 0xffffffff)
            self.expect_err('0x%03x 읽기 -> ERR_BAD_OFFSET' % off,
                            'QVENC_ERR_BAD_OFFSET')
            self.bar.w32(off, 0x12345678)
            self.expect_err('0x%03x 쓰기 -> ERR_BAD_OFFSET' % off,
                            'QVENC_ERR_BAD_OFFSET')

    def t_alignment(self):
        print('\n[9] 정렬 / 접근 폭')
        self.bar.r32(0x02)      # 비정렬 32비트 읽기
        self.expect_err('비정렬 주소 0x02 -> ERR_ALIGN', 'QVENC_ERR_ALIGN')

        # 64비트 접근: CPU 가 32비트 두 번으로 쪼개면 장치는 위반을 보지 못한다.
        # 장치가 아니라 CPU/QEMU 쪽 사정이므로 참고 정보로만 출력한다.
        self.bar.r64(self.R['QVENC_REG_ID'])
        st, ec = self.status(), self.errcode()
        if st & self.R['QVENC_STATUS_ERROR']:
            self.eq('64비트 읽기 -> ERR_ALIGN', ec, self.R['QVENC_ERR_ALIGN'])
        else:
            print('  INFO  64비트 읽기가 32비트 2회로 쪼개짐 (장치는 위반을 못 봄)')
        self.clear()

    def t_reserved_bits(self):
        print('\n[10] 예약 비트')
        ctrl = self.R['QVENC_REG_CTRL']
        self.bar.w32(ctrl, self.R['QVENC_CTRL_IRQ_EN'])
        self.clear()
        self.bar.w32(ctrl, 1 << 31)
        self.expect_err('CTRL 예약 비트 -> ERR_RESERVED_BIT',
                        'QVENC_ERR_RESERVED_BIT')
        self.eq('거부된 쓰기는 CTRL 을 안 바꿈', self.bar.r32(ctrl),
                self.R['QVENC_CTRL_IRQ_EN'])

        self.bar.w32(self.R['QVENC_REG_IRQ_ACK'], 1 << 7)
        self.expect_err('IRQ_ACK 예약 비트 -> ERR_RESERVED_BIT',
                        'QVENC_ERR_RESERVED_BIT')
        self.bar.w32(ctrl, self.R['QVENC_CTRL_RESET'])

    def t_error_sticky(self):
        print('\n[11] ERR_CODE 는 첫 원인을 보존한다')
        self.bar.r32(self.R['QVENC_REG_IRQ_ACK'])       # 1차: ERR_WO_READ
        first = self.errcode()
        self.bar.w32(self.R['QVENC_REG_ID'], 0)         # 2차: ERR_RO_WRITE
        second = self.errcode()
        self.eq('1차 에러는 ERR_WO_READ', first, self.R['QVENC_ERR_WO_READ'])
        self.eq('2차 에러가 덮어쓰지 않음', second, first)
        self.clear()
        self.expect_clean('ACK_ERROR 로 에러가 지워짐')

    def t_reset_recovers(self):
        print('\n[12] RESET 이 더럽힌 상태를 되돌린다')
        self.bar.w32(self.R['QVENC_REG_WIDTH'], 1920)
        self.bar.w32(self.R['QVENC_REG_HEIGHT'], 1080)
        self.bar.w32(self.R['QVENC_REG_QUALITY'], 10)
        self.bar.r32(self.R['QVENC_REG_IRQ_ACK'])       # 에러를 일부러 남긴다
        self.check('RESET 전에 에러가 걸려 있음',
                   bool(self.status() & self.R['QVENC_STATUS_ERROR']),
                   'STATUS=0x%08x' % self.status())
        self.bar.w32(self.R['QVENC_REG_CTRL'], self.R['QVENC_CTRL_RESET'])
        self.expect_clean('RESET 이 STATUS/ERR_CODE 를 치움')
        self.eq('WIDTH 복구', self.bar.r32(self.R['QVENC_REG_WIDTH']),
                self.R['QVENC_RESET_WIDTH'])
        self.eq('QUALITY 복구', self.bar.r32(self.R['QVENC_REG_QUALITY']),
                self.R['QVENC_RESET_QUALITY'])

    def run(self):
        self.t_id()
        self.t_reset_values()
        self.t_roundtrip()
        self.t_ctrl_bits()
        self.t_start_rejects()
        self.t_ro_write()
        self.t_wo_read()
        self.t_bad_offset()
        self.t_alignment()
        self.t_reserved_bits()
        self.t_error_sticky()
        self.t_reset_recovers()
        # 장치를 깨끗한 상태로 두고 끝낸다
        self.bar.w32(self.R['QVENC_REG_CTRL'], self.R['QVENC_CTRL_RESET'])


def main():
    ap = argparse.ArgumentParser(description='qvenc BAR0 MMIO 검증 도구')
    ap.add_argument('--bdf', help='PCI 주소 지정 (예: 0000:00:04.0)')
    ap.add_argument('--header', help='qvenc_regs.h 경로')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if os.geteuid() != 0:
        print('root 로 실행해야 한다 (sysfs resource0 mmap).', file=sys.stderr)
        return 2

    header = find_header(args.header)
    if not header:
        print('include/qvenc_regs.h 를 못 찾았다. --header 로 지정할 것.',
              file=sys.stderr)
        return 2

    R, missing = load_regs(header)
    print('qvenc MMIO probe')
    print('  헤더: %s (%d개 상수)' % (header, len(R)))
    if args.verbose and missing:
        print('  (해석 안 된 매크로: %s)' % ', '.join(sorted(missing)))

    required = ['QVENC_PCI_VENDOR_ID', 'QVENC_PCI_DEVICE_ID', 'QVENC_BAR0_SIZE',
                'QVENC_ID_VALUE', 'QVENC_REG_END']
    absent = [k for k in required if k not in R]
    if absent:
        print('헤더에서 필수 상수를 못 읽었다: %s' % ', '.join(absent),
              file=sys.stderr)
        return 2

    try:
        devpath = find_device(R['QVENC_PCI_VENDOR_ID'], R['QVENC_PCI_DEVICE_ID'],
                              args.bdf)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return 2

    print('  장치: %s' % os.path.basename(devpath))

    try:
        ensure_memory_decode(devpath, args.verbose)
    except (OSError, RuntimeError) as exc:
        print('memory decode 설정 실패: %s' % exc, file=sys.stderr)
        return 2

    try:
        bar = Bar(devpath, R['QVENC_BAR0_SIZE'])
    except OSError as exc:
        print('BAR0 mmap 실패: %s' % exc, file=sys.stderr)
        return 2

    saved_cmd = read_command(devpath)
    probe = Probe(bar, R, args.verbose, devpath)
    try:
        probe.run()
    finally:
        bar.close()
        # PCI COMMAND 를 원래대로 돌려 둔다 (bus master 를 건드렸으므로)
        try:
            write_command(devpath, saved_cmd)
        except OSError:
            pass

    total = probe.passed + probe.failed
    print('\n' + '-' * 58)
    print('결과: %d/%d PASS, %d FAIL' % (probe.passed, total, probe.failed))
    if probe.failed:
        for f in probe.failures:
            print('  - %s' % f)
        print('qvenc MMIO probe: FAILED')
        return 1
    print('qvenc MMIO probe: ALL PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
