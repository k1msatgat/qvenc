#!/usr/bin/env python3
"""
qvenc 장치 검증 도구 — 게스트 OS 없이 실제 QEMU 안에서 돌린다 (HW팀).

QEMU 의 qtest accel 로 띄우고 유닉스 소켓으로 MMIO, PCI config space, 게스트 RAM 을
직접 두드린다. QEMU 자체 장치 테스트(tests/qtest/)가 쓰는 방식이다.

mmio_probe.py / dma_probe.py 와 달리 buildroot 게스트가 필요 없다. 장치만 고쳤을 때는
QEMU 만 재빌드하고 이것부터 돌리는 게 가장 빠르다.

가상 클럭이 clock_step 으로만 흐르므로 지연 타이머가 결정적으로 재현된다.
게스트 안에서는 흉내 내기 어려운 것들 — BAR 를 직접 배치하고, PCI COMMAND 로
bus master 를 껐다 켜고, 타이머 만기 직전/직후를 정확히 겨냥하는 것 — 이 여기서 가능하다.

호스트에서 실행:
    python3 tests/qtest_probe.py
    python3 tests/qtest_probe.py build/qemu/build/qemu-system-x86_64
"""
import os, socket, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = (sys.argv[1] if len(sys.argv) > 1
        else os.path.join(REPO, 'build', 'qemu', 'build', 'qemu-system-x86_64'))
sys.path.insert(0, os.path.join(REPO, 'tests'))
import mmio_probe as mp           # 헤더 파싱을 공유한다

BAR0      = 0xE0000000          # pc 머신의 PCI 구멍. 펌웨어가 없으니 직접 배치한다
RAM_MB    = 256
SRC_PA    = 0x01000000          # 16 MiB
DST_PA    = 0x02000000          # 32 MiB

PCI_COMMAND, PCI_BAR0 = 0x04, 0x10
CMD_MEM, CMD_MASTER = 0x02, 0x04


class QTest:
    def __init__(self, qemu, extra_args=()):
        self.dir = tempfile.mkdtemp(prefix='qvenc-qtest-')
        self.sock_path = os.path.join(self.dir, 'qtest.sock')
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(self.sock_path)
        self.srv.listen(1)
        # -d guest_errors: 장치가 qemu_log_mask(LOG_GUEST_ERROR, ...) 로 남기는
        # 위반 로그를 받아 본다. 로그 경로 자체도 검증 대상이다.
        cmd = [qemu, '-machine', 'pc', '-accel', 'qtest', '-display', 'none',
               '-m', str(RAM_MB), '-qtest', 'unix:%s' % self.sock_path,
               '-d', 'guest_errors', '-nodefaults'] + list(extra_args)
        self.log = open(os.path.join(self.dir, 'qemu.log'), 'w+')
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        self.srv.settimeout(20)
        self.conn, _ = self.srv.accept()
        self.conn.settimeout(20)
        self.buf = b''
        self.irqs = []

    def _line(self):
        while b'\n' not in self.buf:
            chunk = self.conn.recv(4096)
            if not chunk:
                raise RuntimeError('qtest 연결이 끊어졌다')
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b'\n')
        return line.decode().strip()

    def cmd(self, text):
        self.conn.sendall((text + '\n').encode())
        while True:
            line = self._line()
            if line.startswith('IRQ'):
                self.irqs.append(line)
                continue
            if not line.startswith('OK'):
                raise RuntimeError('qtest 실패: %r -> %r' % (text, line))
            parts = line.split()
            return int(parts[1], 0) if len(parts) > 1 else None

    # MMIO
    def readl(self, a):  return self.cmd('readl 0x%x' % a)
    def writel(self, a, v): self.cmd('writel 0x%x 0x%x' % (a, v & 0xffffffff))
    def readq(self, a):  return self.cmd('readq 0x%x' % a)
    def readw(self, a):  return self.cmd('readw 0x%x' % a)

    # 게스트 RAM. qtest 는 줄 단위 프로토콜이라 큰 전송은 끊어서 보낸다.
    CHUNK = 8192

    def _one(self, text, want_data):
        self.conn.sendall((text + '\n').encode())
        while True:
            line = self._line()
            if line.startswith('IRQ'):
                self.irqs.append(line); continue
            if not line.startswith('OK'):
                raise RuntimeError('qtest 실패: %r -> %r' % (text[:60], line))
            return bytes.fromhex(line.split()[1][2:]) if want_data else None

    def mem_write(self, a, data):
        for off in range(0, len(data), self.CHUNK):
            part = data[off:off + self.CHUNK]
            self._one('write 0x%x 0x%x 0x%s' % (a + off, len(part), part.hex()), False)

    def mem_read(self, a, n):
        out = bytearray()
        for off in range(0, n, self.CHUNK):
            m = min(self.CHUNK, n - off)
            out += self._one('read 0x%x 0x%x' % (a + off, m), True)
        return bytes(out)

    def clock_step(self, ns=None):
        return self.cmd('clock_step' + ('' if ns is None else ' %d' % ns))

    # PCI config (0xcf8/0xcfc)
    def _cfg_addr(self, dev, reg, bus=0, fn=0):
        return 0x80000000 | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xfc)
    def cfg_readl(self, dev, reg):
        self.cmd('outl 0xcf8 0x%x' % self._cfg_addr(dev, reg))
        return self.cmd('inl 0xcfc')
    def cfg_writel(self, dev, reg, val):
        self.cmd('outl 0xcf8 0x%x' % self._cfg_addr(dev, reg))
        self.cmd('outl 0xcfc 0x%x' % (val & 0xffffffff))

    def close(self):
        try: self.conn.close()
        except Exception: pass
        self.proc.terminate()
        try: self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired: self.proc.kill()
        self.log.seek(0)
        return self.log.read()


class Checker:
    def __init__(self):
        self.passed = self.failed = 0
        self.failures = []
    def check(self, name, ok, detail=''):
        if ok:
            self.passed += 1
            print('  PASS  %-44s %s' % (name, detail))
        else:
            self.failed += 1
            self.failures.append('%s: %s' % (name, detail))
            print('  FAIL  %-44s %s' % (name, detail))
        return ok
    def eq(self, name, got, want, fmt='0x%x'):
        return self.check(name, got == want,
                          ('got=' + fmt + ' want=' + fmt) % (got, want))


def main():
    R, missing = mp.load_regs(os.path.join(REPO, 'include', 'qvenc_regs.h'))
    assert not missing
    c = Checker()

    print('qvenc qtest probe (실제 QEMU, 게스트 OS 없음)')
    print('  QEMU: %s' % QEMU)
    if not os.path.exists(QEMU):
        print('QEMU 가 없다. scripts/build_qemu.sh 를 먼저 돌릴 것.', file=sys.stderr)
        return 2

    q = QTest(QEMU, ['-device', 'qvenc,latency-ms=10'])
    try:
        # --- PCI 열거 (펌웨어가 없으므로 직접 한다) ---
        print('\n[1] PCI 열거와 BAR 배치')
        slot = None
        for dev in range(32):
            vid_did = q.cfg_readl(dev, 0)
            if vid_did == 0xffffffff:
                continue
            vid, did = vid_did & 0xffff, vid_did >> 16
            if vid == R['QVENC_PCI_VENDOR_ID'] and did == R['QVENC_PCI_DEVICE_ID']:
                slot = dev
                break
        if not c.check('qvenc 를 PCI 버스에서 발견', slot is not None,
                       'slot=%s' % slot):
            raise SystemExit(1)
        c.eq('vendor:device', q.cfg_readl(slot, 0),
             (R['QVENC_PCI_DEVICE_ID'] << 16) | R['QVENC_PCI_VENDOR_ID'])
        classrev = q.cfg_readl(slot, 0x08)
        c.eq('class code', classrev >> 16, 0x0400)
        c.eq('revision', classrev & 0xff, R['QVENC_PCI_REVISION'])

        # BAR0 크기 확인: 전부 1 을 쓰고 되읽는다
        q.cfg_writel(slot, PCI_BAR0, 0xffffffff)
        raw = q.cfg_readl(slot, PCI_BAR0)
        size = (~(raw & 0xfffffff0) & 0xffffffff) + 1
        c.eq('BAR0 크기', size, R['QVENC_BAR0_SIZE'])
        c.check('BAR0 는 32비트 non-prefetchable 메모리', (raw & 0xf) == 0,
                'raw=0x%08x' % raw)

        q.cfg_writel(slot, PCI_BAR0, BAR0)
        q.cfg_writel(slot, PCI_COMMAND, CMD_MEM)      # 메모리 디코딩만, bus master 는 꺼둠
        c.eq('BAR0 배치됨', q.cfg_readl(slot, PCI_BAR0) & 0xfffffff0, BAR0)

        def rd(reg):  return q.readl(BAR0 + R['QVENC_REG_' + reg])
        def wr(reg, v): q.writel(BAR0 + R['QVENC_REG_' + reg], v)
        def st():     return rd('STATUS')
        def ec():     return rd('ERR_CODE')
        def ack():    wr('IRQ_ACK', R['QVENC_IRQ_ACK_ALL'])
        def expect_err(name, code):
            ok = bool(st() & R['QVENC_STATUS_ERROR']) and ec() == R[code]
            c.check(name, ok, 'STATUS=0x%x ERR=0x%02x want %s' % (st(), ec(), code))
            ack()

        # --- 레지스터 ---
        print('\n[2] 레지스터')
        c.eq('ID', rd('ID'), R['QVENC_ID_VALUE'], '0x%08x')
        c.eq('QUALITY 리셋 값', rd('QUALITY'), R['QVENC_RESET_QUALITY'], '%d')
        wr('SRC_LO', 0xdeadbeef); c.eq('SRC_LO 왕복', rd('SRC_LO'), 0xdeadbeef)
        wr('WIDTH', 640); c.eq('WIDTH 왕복', rd('WIDTH'), 640, '%d')
        wr('QUALITY', 9999); c.eq('QUALITY 는 쓰기 시 검사 안 함', rd('QUALITY'), 9999, '%d')
        c.eq('에러 없음', st(), 0)

        print('\n[3] 접근 규칙 위반')
        wr('ID', 0x5a5a5a5a); expect_err('RO 쓰기 -> ERR_RO_WRITE', 'QVENC_ERR_RO_WRITE')
        c.eq('ID 안 바뀜', rd('ID'), R['QVENC_ID_VALUE'], '0x%08x')
        c.eq('IRQ_ACK 읽기 = 0xffffffff', rd('IRQ_ACK'), 0xffffffff, '0x%08x')
        expect_err('WO 읽기 -> ERR_WO_READ', 'QVENC_ERR_WO_READ')
        q.readl(BAR0 + 0x100); expect_err('미정의 오프셋 -> ERR_BAD_OFFSET',
                                          'QVENC_ERR_BAD_OFFSET')
        q.readw(BAR0 + R['QVENC_REG_ID'])
        expect_err('16비트 읽기 -> ERR_ALIGN', 'QVENC_ERR_ALIGN')
        q.readq(BAR0 + R['QVENC_REG_ID'])
        expect_err('64비트 읽기 -> ERR_ALIGN', 'QVENC_ERR_ALIGN')
        wr('CTRL', 1 << 31); expect_err('예약 비트 -> ERR_RESERVED_BIT',
                                        'QVENC_ERR_RESERVED_BIT')
        wr('CTRL', R['QVENC_CTRL_RESET'])

        # --- bus master ---
        print('\n[4] bus master (PCI COMMAND 로 실제 제어)')
        w = h = 64
        n = w * h * 3 // 2
        wr('WIDTH', w); wr('HEIGHT', h); wr('QUALITY', 75)
        wr('SRC_LO', SRC_PA); wr('DST_LO', DST_PA); wr('DST_SIZE', n)
        wr('CTRL', R['QVENC_CTRL_START'])
        c.check('bus master 꺼짐: BUSY 안 됨',
                not (st() & R['QVENC_STATUS_BUSY']), 'STATUS=0x%x' % st())
        expect_err('-> ERR_NO_BUS_MASTER', 'QVENC_ERR_NO_BUS_MASTER')
        q.cfg_writel(slot, PCI_COMMAND, CMD_MEM | CMD_MASTER)
        c.check('COMMAND 에 bus master 반영됨',
                bool(q.cfg_readl(slot, PCI_COMMAND) & CMD_MASTER))

        # --- 실제 DMA ---
        print('\n[5] 실제 DMA 전송 (가상 클럭으로 타이머 구동)')
        data = bytes(((i * 31 + 11) & 0xff) for i in range(n))
        q.mem_write(SRC_PA, data)
        q.mem_write(DST_PA, b'\x00' * n)
        c.check('게스트 RAM 에 입력 기록됨', q.mem_read(SRC_PA, 64) == data[:64])

        wr('CTRL', R['QVENC_CTRL_START'])
        c.check('BUSY 설정됨', bool(st() & R['QVENC_STATUS_BUSY']),
                'STATUS=0x%x' % st())
        c.eq('BUSY 중 OUT_LEN 은 0', rd('OUT_LEN'), 0, '%d')
        c.check('아직 DST 는 비어 있다', q.mem_read(DST_PA, 64) == b'\x00' * 64)

        q.clock_step(5 * 1000 * 1000)          # 5 ms — 아직 만기 전
        c.check('5ms 후에도 BUSY', bool(st() & R['QVENC_STATUS_BUSY']),
                'latency-ms=10 이므로 아직이어야 한다')
        q.clock_step(10 * 1000 * 1000)         # 총 15 ms — 만기 지남
        c.check('15ms 후 BUSY 해제', not (st() & R['QVENC_STATUS_BUSY']),
                'STATUS=0x%x' % st())
        c.check('DONE', bool(st() & R['QVENC_STATUS_DONE']))
        c.eq('OUT_LEN', rd('OUT_LEN'), n, '%d')
        c.check('DST == SRC (%d bytes)' % n, q.mem_read(DST_PA, n) == data)
        ack()

        print('\n[6] 64 KiB chunk 를 넘는 전송')
        w2 = h2 = 512
        n2 = w2 * h2 * 3 // 2
        big = bytes(((i ^ (i >> 8)) & 0xff) for i in range(n2))
        q.mem_write(SRC_PA, big)
        q.mem_write(DST_PA, b'\x00' * n2)
        wr('WIDTH', w2); wr('HEIGHT', h2); wr('DST_SIZE', n2)
        wr('CTRL', R['QVENC_CTRL_START'])
        q.clock_step(20 * 1000 * 1000)
        c.check('DONE', bool(st() & R['QVENC_STATUS_DONE']))
        c.eq('OUT_LEN', rd('OUT_LEN'), n2, '%d')
        c.check('DST == SRC (%d bytes)' % n2, q.mem_read(DST_PA, n2) == big)
        ack()

        print('\n[7] RAM 밖 DMA')
        wr('WIDTH', 64); wr('HEIGHT', 64); wr('DST_SIZE', n)
        wr('SRC_LO', 0x00000000); wr('SRC_HI', 0x7F)      # 508 GiB
        wr('CTRL', R['QVENC_CTRL_START'])
        q.clock_step(20 * 1000 * 1000)
        c.eq('OUT_LEN 은 0', rd('OUT_LEN'), 0, '%d')
        expect_err('-> ERR_DMA_RANGE', 'QVENC_ERR_DMA_RANGE')
        wr('SRC_HI', 0)

        print('\n[8] BUSY 중 START')
        q.mem_write(SRC_PA, data)
        wr('SRC_LO', SRC_PA); wr('DST_LO', DST_PA); wr('DST_SIZE', n)
        wr('WIDTH', 64); wr('HEIGHT', 64)
        wr('CTRL', R['QVENC_CTRL_START'])
        c.check('BUSY', bool(st() & R['QVENC_STATUS_BUSY']))
        wr('CTRL', R['QVENC_CTRL_START'])
        c.eq('두 번째 START -> ERR_BUSY', ec(), R['QVENC_ERR_BUSY'], '0x%02x')
        wr('IRQ_ACK', R['QVENC_IRQ_ACK_ERROR'])
        q.clock_step(20 * 1000 * 1000)
        c.check('원래 작업은 정상 완료', bool(st() & R['QVENC_STATUS_DONE']))
        c.eq('원래 작업 OUT_LEN', rd('OUT_LEN'), n, '%d')
        ack()

        print('\n[9] 파라미터 래칭')
        q.mem_write(DST_PA, b'\x00' * n)
        wr('CTRL', R['QVENC_CTRL_START'])
        wr('WIDTH', 4096)                      # 진행 중 변경
        wr('SRC_LO', 0xdeadb000)
        q.clock_step(20 * 1000 * 1000)
        c.eq('래칭된 길이로 완료', rd('OUT_LEN'), n, '%d')
        c.check('래칭된 SRC 에서 복사됨', q.mem_read(DST_PA, n) == data)
        ack()

        print('\n[10] BUSY 중 RESET')
        wr('WIDTH', 64); wr('SRC_LO', SRC_PA)
        wr('CTRL', R['QVENC_CTRL_START'])
        c.check('BUSY', bool(st() & R['QVENC_STATUS_BUSY']))
        wr('CTRL', R['QVENC_CTRL_RESET'])
        c.eq('RESET 이 STATUS 를 치움', st(), 0)
        q.clock_step(50 * 1000 * 1000)
        c.eq('취소된 타이머는 안 돈다 (STATUS 그대로 0)', st(), 0)
        c.eq('WIDTH 리셋됨', rd('WIDTH'), 0, '%d')
        c.eq('QUALITY 리셋됨', rd('QUALITY'), R['QVENC_RESET_QUALITY'], '%d')

        print('\n[11] MSI capability')
        statreg = q.cfg_readl(slot, 0x04) >> 16
        c.check('capability list 비트 있음', bool(statreg & 0x10),
                'STATUS=0x%04x' % statreg)
        cap = q.cfg_readl(slot, 0x34) & 0xff
        found_msi = False
        while cap and cap != 0xff:
            capreg = q.cfg_readl(slot, cap)
            if (capreg & 0xff) == 0x05:
                found_msi = True
                c.check('MSI capability 발견', True, 'offset=0x%02x' % cap)
                mc = (capreg >> 16) & 0xffff
                c.check('64비트 주소 지원', bool(mc & 0x80), 'MC=0x%04x' % mc)
                c.eq('요청 벡터 수 = 1', 1 << ((mc >> 1) & 0x7), 1, '%d')
                break
            cap = (capreg >> 8) & 0xff
        c.check('MSI capability 존재', found_msi)

    finally:
        log = q.close()

    print('\n[12] guest-error 로그')
    glog = [l for l in log.splitlines() if l.startswith('qvenc:')]
    c.check('장치가 guest-error 로그를 남긴다', len(glog) > 0, '%d줄' % len(glog))
    for l in glog[:4]:
        print('        %s' % l)
    if len(glog) > 4:
        print('        ... 외 %d줄 (전부 의도한 위반)' % (len(glog) - 4))

    print('\n' + '-' * 62)
    print('결과: %d/%d PASS, %d FAIL' % (c.passed, c.passed + c.failed, c.failed))
    for f in c.failures:
        print('  - %s' % f)
    print('qvenc qtest: %s' % ('ALL PASS' if not c.failed else 'FAILED'))
    return 1 if c.failed else 0


if __name__ == '__main__':
    sys.exit(main())
