#!/usr/bin/env python3
"""
qvenc DMA / 인터럽트 검증 도구 (HW팀).

드라이버 없이 DMA 경로를 시험한다. 방법은 실제 칩 bring-up 에서 쓰는 것과 같다:
커널 커맨드라인의 memmap= 으로 물리 메모리 한 구간을 예약해 두고, 그 구간을
/dev/mem 으로 매핑해서 DMA 버퍼로 쓴다. 커널이 그 메모리를 다른 용도로 쓰지
않으므로 장치가 마음대로 읽고 써도 안전하다.

scripts/run.sh 가 이미 16 MiB 를 0x40000000 에 예약해 둔다.

게스트에서 root 로 실행:
    python3 /mnt/qvenc/tests/dma_probe.py
    python3 /mnt/qvenc/tests/dma_probe.py -v --base 0x40000000

인터럽트는 STATUS.IRQ 비트와 INTx 레벨 동작까지만 본다. 실제 인터럽트 전달
(핸들러 호출)은 커널에 핸들러를 등록해야 확인할 수 있으므로 SW팀 드라이버 몫이다.
"""

import argparse
import ctypes
import mmap
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmio_probe as mp           # 헤더 파싱·장치 탐색·BAR 매핑을 공유한다

SRC_OFF = 0x000000                # 예약 구간 안에서의 오프셋
DST_OFF = 0x400000                # 4 MiB 지점
BUSY_TIMEOUT_S = 5.0


class Ram:
    """예약된 물리 메모리 구간을 /dev/mem 으로 매핑한다."""

    def __init__(self, base, size):
        self.base = base
        self.size = size
        self.fd = os.open('/dev/mem', os.O_RDWR)
        try:
            self.mm = mmap.mmap(self.fd, size, mmap.MAP_SHARED,
                                mmap.PROT_READ | mmap.PROT_WRITE,
                                offset=base)
        except (OSError, ValueError):
            os.close(self.fd)
            raise

    def close(self):
        self.mm.close()
        os.close(self.fd)

    def fill(self, off, data):
        self.mm[off:off + len(data)] = data

    def zero(self, off, n):
        self.mm[off:off + n] = b'\x00' * n

    def read(self, off, n):
        return self.mm[off:off + n]


def region_is_reserved(base, size):
    """/proc/iomem 에서 이 구간이 예약되어 있는지 확인한다 (경고용)."""
    try:
        with open('/proc/iomem') as fh:
            for line in fh:
                rng, _, name = line.strip().partition(' : ')
                start, _, end = rng.partition('-')
                try:
                    start, end = int(start, 16), int(end, 16)
                except ValueError:
                    continue
                if start <= base and base + size - 1 <= end:
                    return name.strip() == 'reserved', name.strip()
    except OSError:
        pass
    return False, '(unknown)'


class DmaProbe:
    def __init__(self, bar, ram, R, devpath, verbose):
        self.bar, self.ram, self.R = bar, ram, R
        self.devpath, self.verbose = devpath, verbose
        self.passed = self.failed = 0
        self.failures = []

    # -- 보고 ------------------------------------------------------
    def check(self, name, ok, detail=''):
        if ok:
            self.passed += 1
            print('  PASS  %-38s %s' % (name, detail if self.verbose else ''))
        else:
            self.failed += 1
            self.failures.append('%s: %s' % (name, detail))
            print('  FAIL  %-38s %s' % (name, detail))
        return ok

    def eq(self, name, got, want):
        return self.check(name, got == want, 'got=%d want=%d' % (got, want))

    def info(self, msg):
        print('  INFO  %s' % msg)

    # -- 레지스터 보조 ---------------------------------------------
    def rd(self, reg):
        return self.bar.r32(self.R['QVENC_REG_' + reg])

    def wr(self, reg, val):
        self.bar.w32(self.R['QVENC_REG_' + reg], val)

    def status(self):
        return self.rd('STATUS')

    def errcode(self):
        return self.rd('ERR_CODE')

    def ack(self):
        self.wr('IRQ_ACK', self.R['QVENC_IRQ_ACK_ALL'])

    def start(self, irq_en=False):
        ctrl = self.R['QVENC_CTRL_START']
        if irq_en:
            ctrl |= self.R['QVENC_CTRL_IRQ_EN']
        self.wr('CTRL', ctrl)

    def reset(self):
        self.wr('CTRL', self.R['QVENC_CTRL_RESET'])

    def wait_idle(self, timeout=BUSY_TIMEOUT_S):
        """BUSY 가 내려갈 때까지 기다린다. 타임아웃이면 False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not (self.status() & self.R['QVENC_STATUS_BUSY']):
                return True
            time.sleep(0.001)
        return False

    def program(self, w, h, quality, src_phys, dst_phys, dst_size):
        self.wr('WIDTH', w)
        self.wr('HEIGHT', h)
        self.wr('QUALITY', quality)
        self.wr('SRC_LO', src_phys & 0xffffffff)
        self.wr('SRC_HI', (src_phys >> 32) & 0xffffffff)
        self.wr('DST_LO', dst_phys & 0xffffffff)
        self.wr('DST_HI', (dst_phys >> 32) & 0xffffffff)
        self.wr('DST_SIZE', dst_size)

    def expect_err(self, name, code_name):
        want = self.R[code_name]
        st, ec = self.status(), self.errcode()
        ok = bool(st & self.R['QVENC_STATUS_ERROR']) and ec == want
        self.check(name, ok, 'STATUS=0x%02x ERR_CODE=0x%02x (want %s=0x%02x)'
                   % (st, ec, code_name, want))
        self.ack()
        return ok

    def frame_bytes(self, w, h):
        return w * h * 3 // 2

    def pattern(self, n, seed=0):
        return bytes(((i * 31 + seed * 7 + 11) & 0xff) for i in range(n))

    # -- 테스트 ----------------------------------------------------
    def t_bus_master(self):
        print('\n[1] bus master 검사')
        w, h = 64, 64
        n = self.frame_bytes(w, h)
        mp.set_bus_master(self.devpath, False)
        self.reset()
        self.program(w, h, 75, self.ram.base + SRC_OFF,
                     self.ram.base + DST_OFF, n)
        self.start()
        self.check('bus master 꺼짐: BUSY 로 안 간다',
                   not (self.status() & self.R['QVENC_STATUS_BUSY']),
                   'STATUS=0x%02x' % self.status())
        self.expect_err('-> ERR_NO_BUS_MASTER', 'QVENC_ERR_NO_BUS_MASTER')
        mp.set_bus_master(self.devpath, True)
        self.check('pci_set_master 상당 동작으로 복구',
                   bool(mp.read_command(self.devpath) & mp.PCI_COMMAND_MASTER))

    def t_copy(self, w, h, label):
        n = self.frame_bytes(w, h)
        data = self.pattern(n, seed=w)
        self.ram.fill(SRC_OFF, data)
        self.ram.zero(DST_OFF, n)
        self.reset()
        self.program(w, h, 75, self.ram.base + SRC_OFF,
                     self.ram.base + DST_OFF, n)
        self.start()
        busy_seen = bool(self.status() & self.R['QVENC_STATUS_BUSY'])
        if not self.check('%s: 완료 대기' % label, self.wait_idle(),
                          'BUSY 가 %.1fs 안에 안 내려감' % BUSY_TIMEOUT_S):
            return busy_seen
        st = self.status()
        self.check('%s: DONE' % label, bool(st & self.R['QVENC_STATUS_DONE']),
                   'STATUS=0x%02x ERR_CODE=0x%02x' % (st, self.errcode()))
        self.check('%s: ERROR 없음' % label,
                   not (st & self.R['QVENC_STATUS_ERROR']),
                   'ERR_CODE=0x%02x' % self.errcode())
        self.eq('%s: OUT_LEN' % label, self.rd('OUT_LEN'), n)
        self.check('%s: DST == SRC (%d bytes)' % (label, n),
                   self.ram.read(DST_OFF, n) == data)
        self.ack()
        return busy_seen

    def t_transfers(self):
        print('\n[2] DMA 복사')
        busy_seen = self.t_copy(64, 64, '64x64 (6 KiB)')
        self.check('BUSY 가 관측됨', busy_seen,
                   'latency-ms 가 너무 짧거나 0 이면 못 볼 수 있다')
        print('\n[3] 64 KiB chunk 를 넘는 전송')
        self.t_copy(512, 512, '512x512 (384 KiB)')
        print('\n[4] 최소 크기')
        self.t_copy(16, 16, '16x16 (384 B)')

    def t_param_errors(self):
        print('\n[5] START 거절 경로')
        base = self.ram.base
        n = self.frame_bytes(64, 64)

        self.reset()
        self.program(24, 64, 75, base + SRC_OFF, base + DST_OFF, n)
        self.start()
        self.expect_err('WIDTH=24 (16배수 아님)', 'QVENC_ERR_BAD_PARAM')

        self.program(64, 64, 101, base + SRC_OFF, base + DST_OFF, n)
        self.start()
        self.expect_err('QUALITY=101', 'QVENC_ERR_BAD_PARAM')

        self.program(64, 64, 75, base + SRC_OFF, base + DST_OFF, n - 1)
        self.start()
        self.expect_err('DST_SIZE 부족', 'QVENC_ERR_DST_SMALL')

        self.check('거절된 START 후 BUSY 아님',
                   not (self.status() & self.R['QVENC_STATUS_BUSY']))

    def t_dma_range(self):
        print('\n[6] RAM 밖 DMA')
        n = self.frame_bytes(64, 64)
        bogus = 0x7F00000000            # 508 GiB — 게스트 RAM 밖
        self.reset()
        self.program(64, 64, 75, bogus, self.ram.base + DST_OFF, n)
        self.start()
        # 파라미터는 멀쩡하므로 일단 BUSY 로 들어갔다가 전송에서 실패해야 한다.
        # 다만 관측 시점이 타이밍에 달려 있어 단정하지 않고 알려만 준다.
        if self.status() & self.R['QVENC_STATUS_BUSY']:
            self.info('BUSY 를 거쳐 실패한다 (주소 검사는 전송 시점에 일어난다)')
        self.check('완료 대기', self.wait_idle())
        self.eq('OUT_LEN 은 0', self.rd('OUT_LEN'), 0)
        self.expect_err('-> ERR_DMA_RANGE', 'QVENC_ERR_DMA_RANGE')

    def t_busy(self):
        print('\n[7] BUSY 중 START')
        w, h = 512, 512
        n = self.frame_bytes(w, h)
        self.ram.fill(SRC_OFF, self.pattern(n, seed=3))
        self.reset()
        self.program(w, h, 75, self.ram.base + SRC_OFF,
                     self.ram.base + DST_OFF, n)
        self.start()
        if not (self.status() & self.R['QVENC_STATUS_BUSY']):
            self.info('작업이 너무 빨리 끝나 ERR_BUSY 를 시험할 수 없다 '
                      '(QVENC_LATENCY_MS 를 올려 보라)')
            self.wait_idle()
            self.ack()
            return
        self.start()
        self.eq('두 번째 START -> ERR_BUSY', self.errcode(),
                self.R['QVENC_ERR_BUSY'])
        self.wr('IRQ_ACK', self.R['QVENC_IRQ_ACK_ERROR'])
        self.check('원래 작업은 계속 진행', self.wait_idle())
        self.check('원래 작업이 성공으로 끝남',
                   bool(self.status() & self.R['QVENC_STATUS_DONE']))
        self.eq('원래 작업의 OUT_LEN', self.rd('OUT_LEN'), n)
        self.check('원래 작업의 데이터가 맞음',
                   self.ram.read(DST_OFF, n) == self.pattern(n, seed=3))
        self.ack()

    def t_latching(self):
        print('\n[8] 작업 파라미터 래칭')
        w, h = 512, 512
        n = self.frame_bytes(w, h)
        data = self.pattern(n, seed=5)
        self.ram.fill(SRC_OFF, data)
        self.ram.zero(DST_OFF, n)
        self.reset()
        self.program(w, h, 75, self.ram.base + SRC_OFF,
                     self.ram.base + DST_OFF, n)
        self.start()
        if not (self.status() & self.R['QVENC_STATUS_BUSY']):
            self.info('너무 빨리 끝나 래칭을 시험할 수 없다')
            self.ack()
            return
        self.wr('WIDTH', 16)            # 진행 중 변경 — 영향 없어야 한다
        self.wr('SRC_LO', 0xdeadb000)
        self.check('완료 대기', self.wait_idle())
        self.eq('래칭된 길이로 끝남', self.rd('OUT_LEN'), n)
        self.check('래칭된 SRC 에서 복사됨', self.ram.read(DST_OFF, n) == data)
        self.ack()

    def t_irq(self):
        print('\n[9] 인터럽트 상태 비트')
        w, h = 64, 64
        n = self.frame_bytes(w, h)
        self.ram.fill(SRC_OFF, self.pattern(n, seed=9))
        self.reset()
        self.program(w, h, 75, self.ram.base + SRC_OFF,
                     self.ram.base + DST_OFF, n)

        # IRQ_EN = 0 이면 STATUS.IRQ 가 서지 않는다
        self.start(irq_en=False)
        self.wait_idle()
        self.check('IRQ_EN=0 -> STATUS.IRQ 안 섬',
                   not (self.status() & self.R['QVENC_STATUS_IRQ']),
                   'STATUS=0x%02x' % self.status())
        self.ack()

        # IRQ_EN = 1 이면 완료와 함께 선다
        self.start(irq_en=True)
        self.wait_idle()
        st = self.status()
        self.check('IRQ_EN=1 -> STATUS.IRQ 섬',
                   bool(st & self.R['QVENC_STATUS_IRQ']), 'STATUS=0x%02x' % st)
        self.check('DONE 과 함께 선다', bool(st & self.R['QVENC_STATUS_DONE']))

        # ack 하면 내려간다 (INTx 레벨 트리거의 핵심)
        self.wr('IRQ_ACK', self.R['QVENC_IRQ_ACK_DONE'])
        self.check('ack 후 STATUS.IRQ 내려감',
                   not (self.status() & self.R['QVENC_STATUS_IRQ']),
                   'STATUS=0x%02x' % self.status())

        # IRQ_EN 을 내려도 내려간다
        self.start(irq_en=True)
        self.wait_idle()
        self.check('다시 섬', bool(self.status() & self.R['QVENC_STATUS_IRQ']))
        self.wr('CTRL', 0)
        self.check('IRQ_EN=0 으로 내리면 STATUS.IRQ 내려감',
                   not (self.status() & self.R['QVENC_STATUS_IRQ']))
        self.check('DONE 은 남아 있다',
                   bool(self.status() & self.R['QVENC_STATUS_DONE']))
        self.ack()
        self.info('실제 인터럽트 전달(핸들러 호출)은 드라이버가 있어야 확인된다')

    def run(self):
        self.t_bus_master()
        self.t_transfers()
        self.t_param_errors()
        self.t_dma_range()
        self.t_busy()
        self.t_latching()
        self.t_irq()
        self.reset()


def main():
    ap = argparse.ArgumentParser(description='qvenc DMA/IRQ 검증 도구')
    ap.add_argument('--bdf', help='PCI 주소 지정 (예: 0000:00:04.0)')
    ap.add_argument('--header', help='qvenc_regs.h 경로')
    ap.add_argument('--base', default='0x40000000',
                    help='예약된 DMA 영역의 물리주소 (기본 0x40000000)')
    ap.add_argument('--size-mb', type=int, default=16,
                    help='예약 영역 크기 MiB (기본 16)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if os.geteuid() != 0:
        print('root 로 실행해야 한다.', file=sys.stderr)
        return 2

    header = mp.find_header(args.header)
    if not header:
        print('include/qvenc_regs.h 를 못 찾았다. --header 로 지정할 것.',
              file=sys.stderr)
        return 2

    R, _ = mp.load_regs(header)
    base = int(args.base, 0)
    size = args.size_mb << 20

    print('qvenc DMA probe')
    print('  헤더: %s' % header)
    print('  DMA 영역: 0x%x + %d MiB' % (base, args.size_mb))

    reserved, owner = region_is_reserved(base, size)
    if not reserved:
        print('  [warn] /proc/iomem 에서 이 구간이 "reserved" 가 아니다 (%s).' % owner)
        print('         scripts/run.sh 의 memmap= 설정을 확인할 것. '
              '커널이 쓰는 메모리를 덮어쓸 수 있다.')

    try:
        devpath = mp.find_device(R['QVENC_PCI_VENDOR_ID'],
                                 R['QVENC_PCI_DEVICE_ID'], args.bdf)
        mp.ensure_memory_decode(devpath, args.verbose)
    except (OSError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        return 2
    print('  장치: %s' % os.path.basename(devpath))

    saved_cmd = mp.read_command(devpath)
    try:
        bar = mp.Bar(devpath, R['QVENC_BAR0_SIZE'])
    except OSError as exc:
        print('BAR0 mmap 실패: %s' % exc, file=sys.stderr)
        return 2

    try:
        ram = Ram(base, size)
    except (OSError, ValueError) as exc:
        bar.close()
        print('/dev/mem 매핑 실패: %s' % exc, file=sys.stderr)
        print('CONFIG_STRICT_DEVMEM 이 꺼져 있는지, memmap= 이 걸렸는지 확인할 것.',
              file=sys.stderr)
        return 2

    probe = DmaProbe(bar, ram, R, devpath, args.verbose)
    try:
        probe.run()
    finally:
        ram.close()
        bar.close()
        try:
            mp.write_command(devpath, saved_cmd)
        except OSError:
            pass

    total = probe.passed + probe.failed
    print('\n' + '-' * 60)
    print('결과: %d/%d PASS, %d FAIL' % (probe.passed, total, probe.failed))
    if probe.failed:
        for f in probe.failures:
            print('  - %s' % f)
        print('qvenc DMA probe: FAILED')
        return 1
    print('qvenc DMA probe: ALL PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
