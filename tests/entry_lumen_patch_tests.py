"""Bytecode regressions; optional stock-dump tests never redistribute game data."""
import struct
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/lumen'))
import patch_entry_pc_mode as patch


def string_pool(strings):
    result = bytearray(struct.pack('>I', len(strings)))
    for value in strings:
        raw = value.encode()
        result += struct.pack('>I', len(raw)) + raw + b'\0'
        result += b'\0' * (-(len(raw) + 1) % 4)
    return bytes(result)


class Actions:
    """Small stack evaluator for the actual patched actions, not a Lumen mock.

    MovieClip and native callbacks are supplied by each test. Unsupported
    opcodes fail loudly. This does not claim to emulate timeline animation.
    """
    def __init__(self, pool, registers, globals=None):
        self.pool, self.reg = pool, registers
        self.globals = globals or {}
        self.stack, self.clones, self.removed = [], [], []

    def run(self, code, start, end):
        pc = start
        s = self.stack
        for _ in range(10000):
            if pc >= end:
                return
            op = code[pc]
            size = 3 + struct.unpack_from('<H', code, pc+1)[0] if op >= 128 else 1
            p = code[pc+3:pc+size]
            pc += size
            if op == 0x96:
                q = 0
                while q < len(p):
                    kind = p[q]; q += 1
                    if kind == 9:
                        value = self.pool[struct.unpack_from('<H', p, q)[0]]; q += 2
                    elif kind == 8:
                        value = self.pool[p[q]]; q += 1
                    elif kind == 4:
                        value = self.reg[p[q]]; q += 1
                    elif kind == 5:
                        value = bool(p[q]); q += 1
                    elif kind == 7:
                        value = struct.unpack_from('<i', p, q)[0]; q += 4
                    elif kind in (2, 3):
                        value = None
                    else:
                        raise AssertionError(f'unsupported Push {kind}')
                    s.append(value)
            elif op == 0x87: self.reg[p[0]] = s[-1]
            elif op == 0x17: s.pop()
            elif op == 0x1c: s.append(self.globals[s.pop()])
            elif op == 0x4e:
                key, obj = s.pop(), s.pop(); s.append(obj.get(key))
            elif op == 0x4f:
                value, key, obj = s.pop(), s.pop(), s.pop(); obj[key] = value
            elif op == 0x12: s.append(not s.pop())
            elif op == 0x4c: s.append(s[-1])
            elif op == 0x50: s.append(s.pop() + 1)
            elif op in (0x47, 0x48, 0x49, 0x66, 0x67, 0x0b, 0x0c, 0x0d):
                b, a = s.pop(), s.pop()
                if op == 0x47:
                    value = str(a) + str(b) if isinstance(a, str) or isinstance(b, str) else a+b
                elif op == 0x48: value = a < b
                elif op in (0x49, 0x66): value = type(a) == type(b) and a == b
                elif op == 0x67: value = a > b
                elif op == 0x0b: value = a-b
                elif op == 0x0c: value = a*b
                else: value = a/b
                s.append(value)
            elif op in (0x99, 0x9d):
                if op == 0x99 or s.pop(): pc += struct.unpack('<h', p)[0]
            elif op == 0x52:
                name, receiver, count = s.pop(), s.pop(), s.pop()
                args = [s.pop() for _ in range(count)]
                s.append(receiver[name](*args))
            elif op == 0x24:
                depth, name, source = s.pop(), s.pop(), s.pop()
                clone = dict(source, depth=depth)
                self.reg[4][name] = clone; self.clones.append(clone)
            elif op == 0x25:
                obj = s.pop()
                if obj is not None: self.removed.append(obj)
            elif op == 0x3e: return s.pop()
            elif op == 0: return
            else: raise AssertionError(f'unsupported action {op:#x} at {pc-size:#x}')
        raise AssertionError('action instruction budget exceeded')


class FormatTests(unittest.TestCase):
    def test_empty_indices_and_terminator_padding(self):
        values = ['', 'lmf', 'four', '', '日本', 'NotifyModeSelectEnd']
        self.assertEqual(patch.parse_strings(string_pool(values)), values)

    def test_corrupt_string_pool(self):
        good = string_pool(['', 'four'])
        for bad in (b'', good[:-1], good+b'\0'*4, good[:-4]+b'xxxx'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                patch.parse_strings(bad)

    def test_insertion_before_loop_does_not_retarget_backedge(self):
        # induction setup, loop header, body, jump back to header
        code = bytes.fromhex('96050007000000008701000317960200040317990200f5ff')
        # Build the displacement from boundaries rather than assuming it.
        code = code[:-2] + struct.pack('<h', 13-len(code))
        changed = patch.insert_action_code(code, 0, b'\x17')
        offset, op, size = patch.action_instructions(changed)[-1]
        self.assertEqual(op, 0x99)
        self.assertEqual(offset+size+struct.unpack_from('<h',changed,offset+3)[0],14)


class NativeBindingTests(unittest.TestCase):
    def test_real_callback_table_layout_and_hook_address(self):
        source = ROOT / 'game/EBOOT.elf'
        if not source.exists(): self.skipTest('stock Green ELF unavailable')
        data = source.read_bytes()
        phoff = struct.unpack_from('>Q',data,32)[0]
        size,count = struct.unpack_from('>HH',data,54)
        segments = [struct.unpack_from('>IIQQQQQQ',data,phoff+i*size)
                    for i in range(count)]
        def read(address,n):
            for kind,_,offset,virtual,_,length,_,_ in segments:
                if kind == 1 and virtual <= address and address+n <= virtual+length:
                    return data[offset+address-virtual:offset+address-virtual+n]
            raise AssertionError(f'unmapped ELF address {address:#x}')
        bindings = {}
        for address in range(0xf93b5c,0xf93d44,8):
            name,opd = struct.unpack('>II',read(address,8))
            name = read(name,80).split(b'\0')[0].decode()
            entry,toc = struct.unpack('>II',read(opd,8))
            self.assertEqual(toc,0x1037a88)
            bindings[name] = entry
        self.assertEqual(bindings['NotifyModeSelectEnd'],0x22426c)
        self.assertEqual(bindings['NotifyBnCoinUseResult'],0x226888)
        self.assertEqual(bindings['SetNextScene'],0x2287bc)
        self.assertEqual(bindings['SetPlayerData'],0x223f74)
        self.assertEqual(bindings['StartTimer'],0x224b04)
        header = (ROOT/'src/taiko_entry_callback.h').read_text()
        hook = re.search(r'kSetNextScene\s*=\s*(0x[0-9a-fA-F]+)',header)
        self.assertEqual(int(hook[1],16),bindings['SetNextScene'])


class StockDumpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / 'game/vfs/data/lumendata/packed/entry/packeddata.orig.ddp'
        if not source.exists(): raise unittest.SkipTest('stock Green dump unavailable')
        d = source.read_bytes()
        entries, _, _ = patch.parse_header(d)
        entry = next(e for e in entries if e['name'].endswith('entry.lm'))
        cls.lm = d[entry['offset']:entry['offset']+entry['size']]
        cls.pool, cls.records, cls.labels = cls.read_lm(cls.lm)
        cls.patched = patch.patch_entry_lm(cls.lm)
        cls.new_pool, cls.new_records, cls.new_labels = cls.read_lm(cls.patched)

    @staticmethod
    def read_lm(lm):
        offset, sprite, labels = 64, None, {}
        while offset < len(lm):
            tag, words = struct.unpack_from('>II', lm, offset)
            payload = lm[offset+8:offset+8+words*4]; offset += 8+words*4
            if tag == 0xf001: pool = patch.parse_strings(payload)
            elif tag == 0xf005: records = patch.parse_action_records(payload)
            elif tag == 0x27: sprite = struct.unpack_from('>I',payload)[0]
            elif tag == 0x2b:
                label, frame = struct.unpack_from('>II',payload)
                labels[sprite,label] = frame
        return pool, records, labels

    def test_unchanged_records_and_training_shop_labels(self):
        self.assertEqual(self.pool, self.new_pool)
        for i, (old, new) in enumerate(zip(self.records, self.new_records)):
            if i not in (65,97): self.assertEqual(old, new, i)
        expected = dict(self.labels)
        for sprite in patch.PC_MODE_BOARD_SPRITES:
            self.assertEqual(expected.pop((sprite,289)),30)
            expected[sprite,691] = 30
        self.assertEqual(expected,self.new_labels)

    def test_stock_get_mode_and_dispatches_unchanged(self):
        old, new = self.records[97], self.new_records[97]
        shift = len(new)-len(old)
        self.assertEqual(old[0x1e14:0x257b],new[0x1e14+shift:0x257b+shift])
        for offset, opcode, size in patch.action_instructions(self.records[98]):
            # Enclosing function lengths and jumps across the insertion move.
            if offset < 0x8af and opcode not in (0x8e,0x9b,0x99,0x9d):
                self.assertEqual(self.records[98][offset:offset+size],
                                 self.new_records[98][offset:offset+size])
        mode = dict(MODE_GAME=0,MODE_DRESS=1,MODE_SHOP=2,MODE_BURST=3,
                    MODE_CAMPAIGN=4,MODE_BATTLE=5,MODE_BATTLE_GRAY=6,
                    LABEL_NAME_SRC=dict(enumerate([self.pool[i] for i in (286,287,290,288,289,291,292)])))
        for label, expected in [*[(v,k) for k,v in mode['LABEL_NAME_SRC'].items()],('SetEntryPCMode',0)]:
            vm=Actions(self.pool,{1:dict(mcBoard={0:dict(label=label)},currentIndex=0)}, {'ModeSelect':mode})
            self.assertEqual(vm.run(new,0x1e20+shift,0x1fc2+shift),expected)

    def test_final_callback_once_with_correct_receiver_and_argument(self):
        r=self.new_records[65]
        for selected in (None,False,True):
            for scene,campaign,identity in ((1,0,0),(4,7,42)):
                calls=[]
                native={'SetNextScene':lambda *args: calls.append(args) or True}
                connection=dict(cppMode=True,resource=dict(nextScene=scene,
                    selectCampaign=campaign,decidedCampaignID=identity))
                if selected is not None:connection['SetEntryPCMode']=selected
                vm=Actions(self.pool,{1:{'Lumen':native}}, {'CppConnection':connection})
                self.assertTrue(vm.run(r,0x5b19,0x5ba7+len(r)-len(self.records[65])))
                expected=(scene,campaign,identity)+( (99,) if selected else () )
                self.assertEqual(calls,[expected])
                self.assertEqual(vm.stack,[])

    def test_carousel_initialization_clears_previous_marker(self):
        connection={'SetEntryPCMode':True}
        vm=Actions(self.pool,{}, {'CppConnection':connection})
        end=self.new_records[97].index(self.records[97][0x1fc:0x20a])
        vm.run(self.new_records[97],0x1fc,end)
        self.assertIs(connection['SetEntryPCMode'],False)
        self.assertEqual(vm.stack,[])

    def test_confirmation_completion_installs_stock_fade(self):
        old, new = self.records[97], self.new_records[97]
        start = new.index(old[0xd24:0xd42])
        end = new.index(old[0x12d5:0x12e9]) - 1
        mode = dict(MODE_GAME=0,MODE_DRESS=1,MODE_SHOP=2,MODE_BURST=3,
                    MODE_CAMPAIGN=4,MODE_BATTLE=5,MODE_BATTLE_GRAY=6,
                    LABEL_NAME_SRC=dict(enumerate([self.pool[i] for i in (286,287,290,288,289,291,292)])))
        states = []
        for label in (self.pool[286], 'SetEntryPCMode', 'unknown-label'):
            voice_calls = []
            fade = object()
            controller = dict(mcBoard={0:dict(label=label)},currentIndex=0,
                              EnterFrame_Fadeout=fade,onEnterFrame=None,
                              isWait=True,isFinish=False,_alpha=100)
            connection={}
            vm = Actions(self.pool,{1:controller,2:{'cpp':{
                'NotifyPlayLoopVO':lambda *args: voice_calls.append(args)}}},
                {'ModeSelect':mode,'CppConnection':connection})
            vm.run(new,start,end)
            self.assertEqual(connection['SetEntryPCMode'],label=='SetEntryPCMode')
            self.assertEqual(controller['mcBoard'][0]['label'],label)
            self.assertEqual(vm.stack,[])
            if label == 'unknown-label':
                self.assertIsNone(controller['onEnterFrame'])
                self.assertEqual(voice_calls,[])
            else:
                self.assertIs(controller['onEnterFrame'],fade)
                self.assertEqual(voice_calls,[(False,)])
                fade_start = new.index(old[0x17c5:0x17e1])
                fade_end = new.index(old[0x1818:0x182f]) - 1
                for tick in range(10):
                    vm.run(new,fade_start,fade_end)
                    self.assertEqual(controller['isFinish'],tick == 9)
                self.assertEqual(controller['_alpha'],0)
                self.assertIsNone(controller['onEnterFrame'])
                states.append({k:v for k,v in controller.items()
                               if k not in ('mcBoard','onEnterFrame','EnterFrame_Fadeout')})
        self.assertEqual(states[0],states[1])

    def test_clone_once_and_live_reference_after_repeated_init(self):
        old,new=self.records[97],self.new_records[97]
        # Locate unchanged induction setup and loop header in the patched code.
        header=new.index(old[0x592:0x5a3])
        clone=new.index(b'\x24',new.index(old[0x570:0x585])+len(old[0x570:0x585]))
        start=new.index(old[0x570:0x585])+len(old[0x570:0x585])
        end=new.index(old[0x726:0x743])+len(old[0x726:0x743])
        self.assertLess(clone,header)
        for count in (4,5):
            boards={f'board{i}':{'SetDisplay':lambda *a:None,'SetEnable':lambda *a:None} for i in range(4)}
            controller=dict(mcBoard={},boardNum=count,Interval=100,BoardsWidth=500,currentIndex=0,Tween_Move=lambda:None)
            mode=dict(BOARD_MAX=5,LABEL_NAME=dict(enumerate(['a','b','c','SetEntryPCMode','d'])),LABEL_NAME_SRC={6:'gray'},MODE_BATTLE_GRAY=6)
            vm=Actions(self.pool,{1:controller,4:boards},{'ModeSelect':mode})
            for iteration in range(3):
                vm.run(new,start,end)
                self.assertEqual(len(vm.clones),iteration+1)
                self.assertEqual(len(vm.removed),iteration)
                self.assertIs(controller['mcBoard'][4],boards['board4'])
                self.assertFalse(any(boards['board4'] is x for x in vm.removed))
                self.assertEqual(vm.stack,[])

    def test_relocated_branches_and_function_bounds(self):
        for code in self.new_records:
            instructions=patch.action_instructions(code)
            starts={o for o,_,_ in instructions}|{len(code)}
            for o,op,n in instructions:
                if op in (0x99,0x9d):
                    self.assertIn(o+n+struct.unpack_from('<h',code,o+3)[0],starts)
                elif op in (0x8e,0x9b):
                    params=struct.unpack_from('<H',code,o+5)[0]
                    field=o+3+(7+3*params if op==0x8e else 4+2*params)
                    self.assertIn(o+n+struct.unpack_from('<H',code,field)[0],starts)


if __name__ == '__main__': unittest.main()
