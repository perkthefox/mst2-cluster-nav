#!/usr/bin/env python3
"""Host checks of the actual C decoder with sanitizers, no phone or vendor code."""
import ctypes as C
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/check'
OUT.mkdir(parents=True, exist_ok=True)
LIB = OUT / 'route.dylib'
subprocess.run(['clang', '-shared', '-fPIC', '-DMCN_HOST', '-std=c99', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'include'), str(ROOT / 'src/common/route.c'),
                str(ROOT / 'src/common/media.c'), str(ROOT / 'src/common/aa.c'),
                str(ROOT / 'src/common/transcode.c'), str(ROOT / 'src/common/aa_sdr.c'),
                str(ROOT / 'src/common/scale.c'),
                '-o', str(LIB)], check=True)
lib = C.CDLL(str(LIB))
lib.mcn_route_reset.argtypes = [C.c_void_p]
lib.mcn_route_log_reset.argtypes = [C.c_void_p]
lib.mcn_route_message.argtypes = [C.c_void_p, C.c_char_p, C.c_uint, C.c_uint]
lib.mcn_route_text.argtypes = [C.c_void_p, C.c_uint, C.c_void_p, C.c_uint]
lib.mcn_route_log_feed.argtypes = [C.c_void_p, C.c_void_p, C.c_char_p, C.c_uint, C.c_uint]
lib.mcn_route_config.argtypes = [C.c_char_p, C.c_uint, C.c_void_p, C.c_uint]
lib.mcn_route_component.argtypes = [C.c_void_p]
lib.mcn_route_start.argtypes = [C.c_void_p]
lib.mcn_media_text.argtypes = [C.c_char_p, C.c_uint, C.c_void_p, C.c_uint]
lib.mcn_aa_reset.argtypes = [C.c_void_p]
lib.mcn_aa_status.argtypes = [C.c_void_p, C.c_int]
lib.mcn_aa_distance.argtypes = [C.c_void_p, C.c_int]
lib.mcn_aa_text.argtypes = [C.c_void_p, C.c_void_p, C.c_uint]


class Geom(C.Structure):
    _fields_ = [(n, C.c_int) for n in ('mode', 'req_w', 'req_h', 'margin_w', 'margin_h',
                                       'src_x', 'src_y', 'src_w', 'src_h', 'scale')]


lib.mcn_geom.argtypes = [C.c_int, C.c_int, C.POINTER(Geom)]
lib.mcn_geom.restype = C.c_int
lib.mcn_aa_sdr.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_int, C.POINTER(Geom)]
lib.mcn_aa_sdr.restype = C.c_int
lib.mcn_scale_nv12.argtypes = [C.c_char_p, C.c_char_p, C.c_int, C.c_int, C.c_int,
                               C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_void_p]


def tlv(key, data=b''):
    return struct.pack('>HH', len(data) + 4, key) + data


def num(key, value, size):
    return tlv(key, value.to_bytes(size, 'big', signed=value < 0))


MD = b'metadata=Title,Artist,AlbumTitle,ArtworkFileTransferIdentifier\n'


def msg(kind, *fields):
    payload = b''.join(fields)
    return b'@@' + struct.pack('>HH', len(payload) + 6, kind) + payload


def params(data):
    result = {}
    while data:
        n, key = struct.unpack('>HH', data[:4])
        assert 4 <= n <= len(data)
        result[key] = data[4:n]
        data = data[n:]
    return result


class RouteCheck(unittest.TestCase):
    def setUp(self):
        self.route = C.create_string_buffer(65536)
        self.log = C.create_string_buffer(16384)
        lib.mcn_route_reset(self.route)
        lib.mcn_route_log_reset(self.log)

    def accept(self, packet, now=100):
        return lib.mcn_route_message(self.route, packet, len(packet), now)

    def snapshot(self, now=101):
        out = C.create_string_buffer(4096)
        n = lib.mcn_route_text(self.route, now, out, len(out))
        self.assertLess(n, len(out))
        return dict(line.split('=', 1) for line in out.value.decode().splitlines())

    def maneuver(self, index=7, *extra):
        return msg(0x5202, num(0, 16, 2), num(1, index, 2), num(3, 2, 1),
                   tlv(4, 'Седьмая\nactive=0'.encode()), num(5, 400, 4),
                   num(8, 1, 1), num(10, -90, 2), num(10, 90, 2), num(11, 90, 2), *extra)

    def status(self, now=100, index=7, *extra):
        return self.accept(msg(0x5201, num(1, 1, 1), num(10, 250, 4),
                               num(6, 900, 8), num(7, 12000, 4),
                               tlv(13, struct.pack('>H', index)), *extra), now)

    def test_actual_snapshot_and_cached_maneuver(self):
        self.assertEqual(self.accept(self.maneuver()), 1)
        self.assertEqual(self.status(), 1)
        s = self.snapshot()
        self.assertEqual([s[k] for k in ('active', 'type', 'angle', 'lht', 'dist', 'step',
                                        'rtt', 'dest', 'roads')],
                         ['1', '2', '90', '1', '250', '400', '900', '12000', '-90,90'])
        # Nothing passed yet and no current road: the next maneuver's road is not shown.
        self.assertEqual(s['street'], '')
        self.status(now=2000)
        self.assertEqual(self.snapshot(2001)['type'], '2')
        # Past maneuver 7 the car is on the road it led onto, line breaks removed; known once
        # maneuver 8 is (until then 8 may be maneuver 7 again under a new index).
        self.accept(msg(0x5201, num(10, 20, 4)), 2050)
        self.status(now=2100, index=8)
        self.assertEqual(self.snapshot(2101)['street'], '')
        self.accept(msg(0x5202, num(1, 8, 2), num(3, 1, 1)), 2150)
        self.assertEqual(self.snapshot(2151)['street'], 'Седьмая active=0')

    def test_empty_list_holds_then_hides_and_recovers_cache(self):
        self.accept(self.maneuver())
        self.status()
        self.accept(msg(0x5201, tlv(13)), 1000)
        self.assertEqual(self.snapshot(3999)['type'], '2')
        self.assertEqual(self.snapshot(4000)['type'], '-1')
        self.accept(msg(0x5201, tlv(13)), 4100)
        self.assertEqual(self.snapshot(4101)['type'], '-1')
        self.status(now=4200)
        self.assertEqual(self.snapshot(4201)['type'], '2')

    def test_silence_keeps_guidance(self):
        # The phone sends only changes: Yandex Maps, parked, every ~6 s.
        self.accept(self.maneuver())
        self.status()
        self.assertEqual(self.snapshot(60000)['type'], '2')

    def current(self, index, dist=None, *extra):
        if dist is not None:
            extra = (num(10, dist, 4),) + extra
        self.accept(msg(0x5201, num(1, 1, 1), tlv(13, struct.pack('>HH', index, 0xffff)), *extra))
        return self.snapshot()['street']

    def yandex(self, index, kind, road):
        self.accept(msg(0x5202, num(0, 16, 2), num(1, index, 2), num(3, kind, 1),
                        tlv(2, road.encode())))

    def test_street_is_the_road_the_car_is_on(self):
        # Yandex Maps (car, 2026-10-26): no current road, the road a maneuver leads onto only in
        # its description, and a new index on every update, the same maneuver included.
        self.yandex(19, 2, 'Первая ул.')
        self.assertEqual(self.current(19, 890), '')
        self.yandex(20, 2, 'Первая ул.')
        self.assertEqual(self.current(20, 240), '')
        self.yandex(21, 2, 'Первая ул.')
        self.assertEqual(self.current(21, 80), '')
        self.current(21, 10)
        # Turned: a different maneuver replaces it within 50 m.
        self.yandex(22, 14, 'Вторая пл.')
        self.assertEqual(self.current(22, 1400), 'Первая ул.')
        # A reroute (state 5) that replaces the maneuver near it is not a pass.
        self.current(22, 30)
        self.accept(msg(0x5201, num(1, 5, 1)))
        self.yandex(23, 47, 'Третья ул.')
        self.assertEqual(self.current(23, 100), 'Первая ул.')
        # A stop-and-restart reroute (state 0, 3, 1) keeps the road too.
        self.accept(msg(0x5201, num(1, 0, 1)))
        self.accept(msg(0x5201, num(1, 3, 1)))
        self.yandex(24, 2, 'Четвёртый пер.')
        self.assertEqual(self.current(24, 90), 'Первая ул.')
        # Past an unnamed turn the road is not known.
        self.current(24, 10)
        self.yandex(25, 2, '')
        self.assertEqual(self.current(25, 600), 'Четвёртый пер.')
        self.current(25, 10)
        self.yandex(26, 2, 'Вторая пл.')
        self.assertEqual(self.current(26, 970), '')
        # The app's current road wins when it sends one, and an empty one keeps it.
        self.assertEqual(self.current(26, None, tlv(3, 'Пятый б-р'.encode())),
                         'Пятый б-р')
        self.assertEqual(self.current(26, None, tlv(3, b'')), 'Пятый б-р')

    def test_instructions_are_never_a_road(self):
        # Apple Maps: roads in field 4, instructions in field 2 are never shown.
        self.accept(msg(0x5202, num(0, 16, 2), num(1, 1, 2), num(3, 2, 1),
                        tlv(4, 'ул. Шестая'.encode())))
        self.accept(msg(0x5202, num(0, 16, 2), num(1, 2, 2), num(3, 2, 1),
                        tlv(2, 'Поверните направо'.encode())))
        self.assertEqual(self.current(1, 300), '')
        self.current(1, 20)
        self.assertEqual(self.current(2, 500), 'ул. Шестая')
        self.accept(msg(0x5202, num(0, 16, 2), num(1, 3, 2), num(3, 2, 1)))
        self.current(2, 20)
        self.assertEqual(self.current(3, 400), '')

    def test_arrival_stop_timeout_and_new_route(self):
        self.accept(self.maneuver())
        self.status()
        self.accept(msg(0x5201, num(1, 2, 1)), 1000)
        self.assertEqual(self.snapshot(1001)['type'], '10')
        self.assertEqual(self.snapshot(6000), {'active': '0'})
        self.accept(msg(0x5201, num(1, 0, 1)), 6500)
        self.assertEqual(self.snapshot(6501), {'active': '0'})
        self.status(now=6600)
        self.assertEqual(self.snapshot(6601)['type'], '-1')

    def test_partial_fields_and_status_before_maneuver(self):
        self.status()
        self.assertEqual(self.snapshot()['type'], '-1')
        self.accept(self.maneuver())
        self.assertEqual(self.snapshot()['type'], '2')
        self.accept(msg(0x5202, num(1, 7, 2), num(3, 1, 1)))
        self.assertEqual(self.snapshot()['type'], '1')
        self.assertEqual(self.snapshot()['step'], '400')

    def test_malformed_and_foreign_component_do_not_mutate(self):
        self.accept(self.maneuver())
        self.status()
        before = bytes(self.route)
        for packet in (b'', b'@@\x00\x06R', msg(0x5201, b'\x00\x03\x00\x01'),
                       msg(0x5201, num(1, 0, 1), tlv(13, b'x')),
                       msg(0x5202, num(0, 17, 2), num(1, 7, 2), num(3, 1, 1)),
                       msg(0x5204, num(1, 1, 2), tlv(2, b'x'))):
            self.assertEqual(self.accept(packet), 0)
            self.assertEqual(bytes(self.route), before)

    def test_all_hex_read_splits_and_noise(self):
        packet = self.maneuver()
        data = ('--?-- [msgid=5202]\n' + ' '.join('%02X' % b for b in packet)
                + '\n(checksum) FF\n' + ' '.join('%02x' % b for b in
                  msg(0x5201, num(1, 1, 1), num(13, 7, 2))) + '\n').encode()
        for split in range(len(data) + 1):
            self.setUp()
            for part in (data[:split], data[split:]):
                lib.mcn_route_log_feed(self.log, self.route, part, len(part), 100)
            self.assertEqual(self.snapshot()['type'], '2', split)

    def test_stock_print_packet_framing(self):
        # ipod-drvr-iap2.so print_packet: "--?-- [msgid=%02x%02x]", " CTRL[" (or " CTRL data["),
        # ipod_print_bytes without a trailing space, "] ", "CKSUM[", one byte, "] ", ")".
        def dump(packet, label=' CTRL['):
            return ('--?-- [msgid=%02x%02x]' % (packet[4], packet[5]) + label
                    + ' '.join('%02X' % b for b in packet) + '] CKSUM[40] )\n')
        data = (dump(self.maneuver()) + dump(msg(0x5201, num(1, 1, 1), num(13, 7, 2)),
                                             ' CTRL data[')).encode()
        for split in range(len(data) + 1):
            self.setUp()
            count = 0
            for part in (data[:split], data[split:]):
                count += lib.mcn_route_log_feed(self.log, self.route, part, len(part), 100)
            self.assertEqual(count, 2, split)
            self.assertEqual(self.snapshot()['type'], '2', split)

    def test_oversize_and_broken_dump_recover(self):
        data = b'40 40 FF FF 52 02\n' + b'A' * 10000 + b' --broken--\n'
        data += b' '.join(('%02X' % b).encode() for b in self.maneuver()) + b'\n'
        lib.mcn_route_log_feed(self.log, self.route, data, len(data), 100)
        self.status()
        self.assertEqual(self.snapshot()['type'], '2')

    def test_reidentification_name_resets_cache_at_every_split(self):
        data = b'Accessory Identification StartIdentification (1d00)\n'
        for split in range(len(data) + 1):
            self.setUp()
            self.accept(self.maneuver())
            self.status()
            events = 0
            for part in (data[:split], data[split:]):
                events |= lib.mcn_route_log_feed(self.log, self.route, part, len(part), 200)
            self.assertTrue(events & 0x10000)
            self.assertEqual(self.snapshot(201), {'active': '0'})
            self.status(now=300)
            self.assertEqual(self.snapshot(301)['type'], '-1')

    def test_lanes_and_saturating_numbers(self):
        lanes = msg(0x5204, num(0, 16, 2), num(1, 5, 2),
                    tlv(2, num(0, 0, 2) + num(1, 2, 1) + num(2, -45, 2)))
        self.assertEqual(self.accept(lanes), 1)
        self.status(100, 7, num(16, 5, 2), num(18, 1, 1))
        self.accept(msg(0x5201, num(6, 0xffffffffffffffff, 8), num(7, 0xffffffff, 4)))
        self.assertEqual(self.snapshot()['rtt'], '2147483647')
        self.assertEqual(self.snapshot()['dest'], '2147483647')

    def lane(self, position, status, *angles, highlight=None):
        return tlv(2, num(0, position, 2) + num(1, status, 1)
                   + b''.join(num(2, a, 2) for a in angles)
                   + (num(3, highlight, 2) if highlight is not None else b''))

    def test_lane_snapshot_for_hmi(self):
        self.accept(msg(0x5204, num(1, 5, 2), self.lane(2, 2, 0, highlight=90),
                        self.lane(0, 0, -90), self.lane(1, 1, 0)))
        self.status(100, 7, num(16, 5, 2), num(18, 1, 1))
        out = C.create_string_buffer(4096)
        n = lib.mcn_route_text(self.route, 101, out, len(out))
        self.assertGreater(n, 0)
        self.assertIn(b'lane0=2,2,90,0\n', out.raw[:n])
        (OUT / 'lanes-message.txt').write_bytes(out.raw[:n])

    def test_lane_visibility_current_index_lifecycle_and_repeated_angles(self):
        self.accept(msg(0x5204, num(1, 5, 2), self.lane(0, 2, -90, 0, highlight=90)))
        self.status(100, 7, num(16, 5, 2), num(18, 1, 1))
        self.assertEqual(self.snapshot()['lanes'], '1')
        self.assertEqual(self.snapshot()['lane0'], '0,2,90,-90,0')
        self.accept(msg(0x5201, tlv(13)), 1000)
        self.assertEqual(self.snapshot(4001)['lanes'], '1')
        self.accept(msg(0x5201, num(18, 0, 1)), 4100)
        self.assertEqual(self.snapshot(4101)['lanes'], '0')
        self.accept(msg(0x5201, num(18, 1, 1), num(16, 6, 2)), 4200)
        self.assertEqual(self.snapshot(4201)['lanes'], '0')
        self.accept(msg(0x5204, num(1, 6, 2), self.lane(0, 1, 0)), 4300)
        self.assertEqual(self.snapshot(4301)['lane0'], '0,1,1000,0')
        self.accept(msg(0x5201, num(1, 2, 1)), 4400)
        self.assertEqual(self.snapshot(4401)['lanes'], '0')
        self.assertEqual(self.snapshot(9400), {'active': '0'})
        self.accept(msg(0x5201, num(1, 0, 1)), 9500)
        self.status(9600, 7, num(16, 6, 2), num(18, 1, 1))
        self.assertEqual(self.snapshot(9601)['lanes'], '0')

    def test_lane_capacity_and_output_bounds(self):
        packet = msg(0x5204, num(1, 5, 2), *[self.lane(i, 2, *range(-8, 8)) for i in range(16)])
        self.accept(packet)
        self.status(100, 7, num(16, 5, 2), num(18, 1, 1))
        self.assertEqual(self.snapshot()['lanes'], '16')
        small = C.create_string_buffer(128)
        self.assertLess(lib.mcn_route_text(self.route, 101, small, len(small)), 0)
        self.accept(msg(0x5204, num(1, 5, 2), *[self.lane(i, 2, 0) for i in range(17)]))
        self.assertEqual(self.snapshot()['lanes'], '0')
        self.accept(msg(0x5204, num(1, 5, 2), self.lane(0, 2, *range(17))))
        self.assertEqual(self.snapshot()['lanes'], '0')

    def test_group_and_start_wire_format(self):
        buf = C.create_string_buffer(128)
        n = lib.mcn_route_component(buf)
        fields = params(buf.raw[:n])
        self.assertEqual(fields[0], b'\x00\x10')
        self.assertEqual(fields[1], b'Instrument Cluster\0')
        self.assertEqual(fields[6], b'\x00\x06')
        self.assertEqual(fields[8], b'\x00\x06')
        n = lib.mcn_route_start(buf)
        self.assertEqual(buf.raw[:n], msg(0x5200, num(0, 16, 2)))

    def test_cfg_keeps_stock_values_and_is_idempotent(self):
        for data in (b'[accessory]\nserial=123\n',
                     b'[ident]\nsendmsgs=0x5000,20992\nrecvmsgs=0x5001\n[location]\nenable=1',
                     b'[ident]\nsendmsgs=\nrecvmsgs=\n',
                     b'[ident]\nsendmsgs=0x5200,0x5203\nrecvmsgs=0x5201,0x5202,0x5204\n'):
            out = C.create_string_buffer(4096)
            n = lib.mcn_route_config(data, len(data), out, len(out))
            self.assertGreater(n, 0)
            first = out.raw[:n]
            n = lib.mcn_route_config(first, len(first), out, len(out))
            self.assertEqual(out.raw[:n], first)
            self.assertEqual(first.count(b'sendmsgs='), 1)
            self.assertEqual(first.count(b'recvmsgs='), 1)
            if b'location' in data:
                self.assertIn(b'[location]\nenable=1\n[nowplaying]\n', first)
                self.assertIn(b'sendmsgs=0x5000,20992,0x5203\n', first)
        self.assertEqual(lib.mcn_route_config(b'', 0, out, 4), -1)

    def test_cfg_enables_now_playing(self):
        out = C.create_string_buffer(4096)
        data = (b'[nowplaying]\nenable=no\nmetadata=Title,ArtworkFileTransferIdentifier\n'
                b'playback = Status,AppName\nother=1\n[location]\nenable=yes\n')
        n = lib.mcn_route_config(data, len(data), out, len(out))
        text = out.raw[:n]
        self.assertIn(b'[nowplaying]\nenable=yes\n' + MD +
                      b'playback=Status\nother=1\n[location]\nenable=yes\n', text)
        self.assertEqual(text.count(b'[nowplaying]'), 1)
        # a section with only enable= must still get metadata and playback added before the
        # next section and only once (code review: partial section).
        data = b'[nowplaying]\nenable=no\n[eap]\nenable=yes\n'
        n = lib.mcn_route_config(data, len(data), out, len(out))
        text = out.raw[:n]
        self.assertIn(b'[nowplaying]\nenable=yes\n' + MD +
                      b'playback=Status\n[eap]\nenable=yes\n', text)
        self.assertEqual(text.count(MD), 1)
        self.assertEqual(text.count(b'playback=Status'), 1)
        # a partial section at end of file
        data = b'[accessory]\nx=1\n[nowplaying]\nmetadata=Title\n'
        n = lib.mcn_route_config(data, len(data), out, len(out))
        text = out.raw[:n]
        self.assertIn(b'[nowplaying]\n' + MD + b'enable=yes\n'
                      b'playback=Status\n', text)
        self.assertEqual(text.count(b'enable=yes'), 1)
        data = b'[accessory]\nname=x\n'
        n = lib.mcn_route_config(data, len(data), out, len(out))
        self.assertTrue(out.raw[:n].endswith(b'[nowplaying]\nenable=yes\n' +
                                             MD + b'playback=Status\n'))

    def media(self, pps, size=4096):
        out = C.create_string_buffer(size)
        n = lib.mcn_media_text(pps, len(pps), out, size)
        return n if n <= 0 else out.raw[:n].decode()

    @staticmethod
    def varints(blob):
        out, v, sh = [], 0, 0
        for b in blob:
            v |= (b & 0x7f) << sh
            if b & 0x80:
                sh += 7
            else:
                out.append(v)
                v = sh = 0
        return out

    @staticmethod
    def pb(blob):
        """field number -> list of values (int for varint, bytes for length-delimited)."""
        fields, at = {}, 0
        while at < len(blob):
            tag, at = RouteCheck._take(blob, at)
            field, wire = tag >> 3, tag & 7
            if wire == 0:
                val, at = RouteCheck._take(blob, at)
            elif wire == 2:
                n, at = RouteCheck._take(blob, at)
                val, at = blob[at:at + n], at + n
            else:
                raise AssertionError('wire %d' % wire)
            fields.setdefault(field, []).append(val)
        return fields

    @staticmethod
    def _take(blob, at):
        v, sh = 0, 0
        while True:
            b = blob[at]
            at += 1
            v |= (b & 0x7f) << sh
            if not b & 0x80:
                return v, at
            sh += 7

    @staticmethod
    def v(values):
        assert len(values) == 1
        return values[0]

    def geom(self, mode, factor=1000):
        g = Geom()
        self.assertEqual(lib.mcn_geom(mode, factor, C.byref(g)), 0)
        return g

    def test_transcode_geometry(self):
        self.assertEqual(lib.mcn_geom(0, 0, C.byref(Geom())), -1)  # OFF has no geometry
        aa = self.geom(1)  # MCN_TC_AA
        self.assertEqual((aa.req_w, aa.req_h), (1280, 720))
        self.assertEqual((aa.src_x, aa.src_y, aa.src_w, aa.src_h), (128, 168, 1024, 384))
        self.assertEqual(aa.scale, 0)
        # content rectangle fits inside the frame and is VPU-aligned
        self.assertEqual(aa.src_x % 16, 0)
        self.assertEqual(aa.src_y % 2, 0)
        self.assertLessEqual(aa.src_x + aa.src_w, aa.req_w)
        self.assertLessEqual(aa.src_y + aa.src_h, aa.req_h)
        for factor, want in ((1000, (1024, 376)), (1300, (1328, 488)), (1500, (1520, 564)),
                             (1900, (1920, 714)), (3000, (1920, 714))):
            z = self.geom(2, factor)  # clamped to 1900 at 3000
            self.assertEqual((z.req_w, z.req_h), want)
            self.assertEqual(z.req_w % 16, 0)
            self.assertEqual(z.req_h % 2, 0)
            self.assertLessEqual(z.req_w, 1920)
            self.assertEqual((z.src_x, z.src_y, z.src_w, z.src_h),
                             (0, 0, z.req_w, z.req_h))
            self.assertEqual(z.scale, 1)

    @staticmethod
    def scale(y, c, sstride, sw, sh, dw, dh):
        """Runs mcn_scale_nv12 into dw-stride planes; checks it stays inside its work buffer."""
        work_n = ((sw + 8 + 3) & ~3) + dw
        work = C.create_string_buffer(work_n + 16)
        C.memset(C.addressof(work) + work_n, 0xA5, 16)
        dy = C.create_string_buffer(dw * dh)
        dc = C.create_string_buffer(dw * dh // 2)
        lib.mcn_scale_nv12(bytes(y), bytes(c), sstride, sw, sh, dy, dc, dw, dw, dh, work)
        assert work.raw[work_n:work_n + 16] == b'\xa5' * 16, 'work buffer overrun'
        return dy.raw[:dw * dh], dc.raw[:dw * dh // 2]

    @staticmethod
    def ref_plane(src, stride, step, sw, sh, dw, dh):
        """Integer model of the C: 16.16 steps, 8-bit fractions, vertical then horizontal."""
        def ratio(a, b):
            return ((a - 1) << 16) // (b - 1)
        rx, ry = ratio(sw, dw), ratio(sh, dh)
        out = []
        for oy in range(dh):
            sy = oy * ry
            y, fy = sy >> 16, (sy >> 8) & 0xff
            y1 = y + 1 if y + 1 < sh else y
            t = [(src[y * stride + i * step] * (256 - fy) + src[y1 * stride + i * step] * fy
                  + 128) >> 8 for i in range(sw)] + [None]
            t[sw] = t[sw - 1]
            row = []
            for ox in range(dw):
                sx = ox * rx
                x, fx = sx >> 16, (sx >> 8) & 0xff
                row.append((t[x] * (256 - fx) + t[x + 1] * fx + 128) >> 8)
            out.append(row)
        return out

    def test_scale_nv12_flat_and_reference(self):
        # A flat frame stays flat (taps sum to 1); random data equals the integer model exactly,
        # which proves the four-bytes-per-multiply blend keeps its lanes apart.
        import random
        rnd = random.Random(7)
        for sw, sh, dw, dh in ((80, 20, 64, 16), (96, 24, 64, 16), (1264, 8, 1010, 6)):
            y, c = self.scale(bytes([130]) * (sw * sh), bytes([90, 200]) * (sw // 2 * sh // 2),
                              sw, sw, sh, dw, dh)
            self.assertEqual(set(y), {130})
            self.assertEqual(set(c[0::2]), {90})
            self.assertEqual(set(c[1::2]), {200})
            ys = bytes(rnd.randrange(256) for _ in range(sw * sh))
            cs = bytes(rnd.randrange(256) for _ in range(sw * sh // 2))
            y, c = self.scale(ys, cs, sw, sw, sh, dw, dh)
            want = self.ref_plane(ys, sw, 1, sw, sh, dw, dh)
            self.assertEqual([list(y[r * dw:(r + 1) * dw]) for r in range(dh)], want)
            for off in (0, 1):  # Cb, Cr
                want = self.ref_plane(cs[off:], sw, 2, sw // 2, sh // 2, dw // 2, dh // 2)
                got = [list(c[r * dw + off:(r + 1) * dw:2]) for r in range(dh // 2)]
                self.assertEqual(got, want)

    def test_scale_nv12_keeps_thin_lines(self):
        # The zoom-out ratios (1.25, 1.5): a one-pixel line at any position survives at a quarter
        # of its brightness or more. The GC320's 1x1-kernel point sampling dropped such lines
        # (letters losing strokes in the car), so this is the property that matters.
        for sw, dw in ((80, 64), (96, 64)):
            sh, dh = sw // 4, dw // 4
            cflat = bytes([128]) * (sw * sh // 2)
            for x in range(sw):
                src = bytearray(sw * sh)
                for r in range(sh):
                    src[r * sw + x] = 255
                y, _ = self.scale(src, cflat, sw, sw, sh, dw, dh)
                self.assertGreaterEqual(max(y[:dw]), 63, (sw, x))
            for r in range(sh):
                src = bytearray(sw * sh)
                src[r * sw:(r + 1) * sw] = bytes([255]) * sw
                y, _ = self.scale(src, cflat, sw, sw, sh, dw, dh)
                self.assertGreaterEqual(max(y[0::dw]), 63, (sh, r))

    def test_aa_cluster_discovery_bytes(self):
        aa = self.geom(1)
        out = C.create_string_buffer(256)
        n = lib.mcn_aa_sdr(out, len(out), 5, 6, C.byref(aa))
        self.assertGreater(n, 0)
        services = self.pb(out.raw[:n])
        self.assertEqual(len(services[1]), 2)  # two Service entries, both field 1
        video, inp = (self.pb(s) for s in services[1])
        self.assertEqual(self.v(video[1]), 5)         # video channel id
        ms = self.pb(video[3][0])
        self.assertEqual(self.v(ms[1]), 3)            # available_type = VIDEO
        self.assertEqual(self.v(ms[6]), 1)            # display_id
        self.assertEqual(self.v(ms[7]), 1)            # display_type = CLUSTER
        vc = self.pb(ms[4][0])
        self.assertEqual([self.v(vc[k]) for k in (1, 2, 3, 4)], [2, 2, 256, 336])  # res,fps,margins
        self.assertEqual(self.v(inp[1]), 6)           # input channel id
        iss = self.pb(inp[4][0])
        self.assertEqual(self.v(iss[5]), 1)           # input display_id
        keys = self.varints(iss[1][0])
        self.assertEqual(keys, [23, 65536])
        self.assertEqual(lib.mcn_aa_sdr(out, len(out), 5, 6, C.byref(self.geom(2, 1500))), 0)

    def test_now_playing_from_pps(self):
        pps = ('@nowplaying\nPlaybackStatus::Playing\nMediaItemTitle::Песня: часть 1\r\n'
               'MediaItemArtist::Группа\nMediaItemTitleX::no\n-MediaItemAlbumTitle\n').encode()
        self.assertEqual(self.media(pps),
                         'media=1\ntitle=Песня: часть 1\nartist=Группа\nalbum=\n')
        self.assertEqual(self.media(b'@nowplaying\nMediaItemArtist::A\n'), 0)
        self.assertEqual(self.media(b'MediaItemTitle::T\nMediaItemAlbumTitle::B'),
                         'media=1\ntitle=T\nartist=\nalbum=B\n')
        self.assertEqual(self.media(b'MediaItemTitle::T\n', 12), -1)
        title = self.media(('MediaItemTitle::' + 'я' * 150 + '\n').encode()).split('\n')[1]
        self.assertEqual(title, 'title=' + 'я' * 99)

    def test_now_playing_pause_stop_and_deleted_attributes(self):
        # The driver writes "PlaybackStatus::<name>" (Stopped, Playing, Paused, SeekForward,
        # SeekBackward, Unknown); only a paused or stopped track leaves the cluster.
        track = b'@nowplaying\nMediaItemTitle::T\nMediaItemArtist::A\n'
        for status, shown in ((b'Playing', True), (b'Paused', False), (b'Stopped', False),
                              (b'SeekForward', True), (b'Unknown', True)):
            text = self.media(track + b'PlaybackStatus::' + status + b'\n')
            self.assertEqual(text != 0, shown, status)
        self.assertNotEqual(self.media(track), 0)  # no status yet: the title is shown
        # PPS deletions: "-name" removes the attribute, an unrelated "-x" is ignored.
        self.assertEqual(self.media(track + b'-MediaItemTitle\n'), 0)
        self.assertEqual(self.media(track + b'-MediaItemArtist\n-x\n-\n'),
                         'media=1\ntitle=T\nartist=\nalbum=\n')
        self.assertNotEqual(self.media(track + b'PlaybackStatus::Paused\n-PlaybackStatus\n'), 0)
        self.assertEqual(self.media(track + b'-MediaItemTitleX\n'),
                         'media=1\ntitle=T\nartist=A\nalbum=\n')

    def test_silence_without_teardown_ends_guidance_and_resumes(self):
        # Phone gone without a TEARDOWN (or mm-ipod dead): no status for MCN_RG_SILENT_MS.
        self.accept(self.maneuver())
        self.status(now=1000)
        self.assertEqual(self.snapshot(1000 + 119999)['type'], '2')
        self.assertEqual(self.snapshot(1000 + 120000), {'active': '0'})
        # A real status brings it back with the cached maneuver: silence never clears the route.
        self.status(now=200000)
        self.assertEqual(self.snapshot(200001)['type'], '2')

    def test_passed_waits_for_the_new_maneuver(self):
        # A status may name an index whose 0x5202 is not cached yet; the street line must not
        # change until it is known whether that index is the same maneuver (Yandex Maps style).
        self.yandex(31, 2, 'Первая ул.')
        self.current(31, 10)
        self.assertEqual(self.current(32, 900), '')
        self.yandex(32, 2, 'Первая ул.')                # the same turn under a new index
        self.assertEqual(self.current(32, 800), '')
        self.current(32, 10)
        self.assertEqual(self.current(33, 700), '')      # 33 unknown: no decision yet
        self.yandex(33, 14, 'Вторая пл.')                # a different maneuver: 31/32 passed
        self.assertEqual(self.snapshot()['street'], 'Первая ул.')
        # A reroute before the new maneuver arrives is no pass.
        self.current(33, 10)
        self.assertEqual(self.current(34, 500), 'Первая ул.')
        self.accept(msg(0x5201, num(1, 5, 1)))
        self.yandex(34, 47, 'Третья ул.')
        self.assertEqual(self.snapshot()['street'], 'Первая ул.')

    @staticmethod
    def stock_like_cfg():
        # Synthetic stand-in for the unit's iap2.cfg (no vendor content): the same layout as the
        # P0480T template and the runtime copy SAL writes from it. Comment lines before each
        # section, sections commented out with ';[...]', a non-ASCII byte in a comment,
        # [nowplaying] disabled with long lists, no [ident], [commands] last.
        return ('# accessory\n[accessory]\nname=A\nmodel=B\nserial=S1\n\n'
                '# transport\n[transport]\nname=T\nid=1\n\n'
                '[applaunch]\nenable=no\n\n'
                '# bt: mac\n;[bluetooth]\n;enable=yes\n;id=2\n\n'
                ';[devaudio]\n;enable=yes\n;rates=32000\n\n'
                '# media\n[medialib]\nenable=no\nsyncbufsize=1\n\n'
                '# metadata: Track attributes to be published.\n'
                '# playback: Playback state attributes to be published.\n'
                '[nowplaying]\nenable=no\nmetadata=Title,PlaybackDurationInMilliseconds,'
                'AlbumTitle,Artist,ArtworkFileTransferIdentifier\n'
                'playback=Status,ElapsedTimeInMilliseconds,AppName\n\n'
                '# location\n[location]\ntypes=GPGGA\nname=L\nid=3\n\n'
                '[eap]\nprotocol=x.y,match=0\nenable=yes\nmaxbufsize=1\n\n'
                '[power]\nenable=yes\n\n[communications]\nenable=yes\nlists=no\n\n'
                ';[vehicle]\n;enable=yes\n\n;[hid]\n;component=name::K,function::0\n\n'
                '# set the API(s) to \u2019no\u2019.\n[commands]\n'
                'iap2_nowplay_set=no\niap2_x=no\n').encode()

    def test_cfg_on_stock_like_template(self):
        data = self.stock_like_cfg()
        out = C.create_string_buffer(8192)
        n = lib.mcn_route_config(data, len(data), out, len(out))
        self.assertGreater(n, 0)
        text = out.raw[:n]
        # Only [nowplaying]'s three keys change; [ident] is added once, at the end.
        np = (b'[nowplaying]\nenable=yes\n' + MD +
              b'playback=Status\n\n')
        self.assertEqual(text, data.replace(data[data.index(b'[nowplaying]'):
                                                 data.index(b'# location')], np)
                         + b'[ident]\nsendmsgs=0x5200,0x5203\nrecvmsgs=0x5201,0x5202,0x5204\n')
        # Commented sections and the [commands] API switches stay as they were.
        self.assertIn(b';[bluetooth]\n;enable=yes\n', text)
        self.assertIn(b'iap2_nowplay_set=no\n', text)
        # Idempotent: SAL's next copy of an already rewritten file gives the same result.
        n2 = lib.mcn_route_config(text, len(text), out, len(out))
        self.assertEqual(out.raw[:n2], text)
        # SAL may reorder keys and switch sections (runtime copy): only values move.
        moved = data.replace(b'[applaunch]\nenable=no', b'[applaunch]\nenable=yes')
        n = lib.mcn_route_config(moved, len(moved), out, len(out))
        self.assertIn(b'[applaunch]\nenable=yes\n', out.raw[:n])
        self.assertEqual(out.raw[:n].count(b'[ident]'), 1)


lib.mcn_aa_turn.argtypes = [C.c_void_p, C.c_char_p, C.c_uint, C.c_int, C.c_int, C.c_int, C.c_int]
lib.mcn_aa_copy.argtypes = [C.c_void_p, C.c_uint, C.c_char_p, C.c_uint]
lib.mcn_aa_media_text.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_void_p,
                                  C.c_uint]
lib.mcn_cover_save.argtypes = [C.c_char_p, C.c_char_p, C.c_uint, C.c_uint, C.c_void_p, C.c_uint]
lib.mcn_art_reset.argtypes = [C.c_void_p]
lib.mcn_art_poll.argtypes = [C.c_void_p, C.c_char_p, C.c_uint, C.c_uint]
lib.mcn_art_poll.restype = C.c_uint
lib.mcn_art_fetched.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
lib.mcn_cover_clean.argtypes = [C.c_char_p]
lib.mcn_art_current.argtypes = [C.c_void_p]
lib.mcn_art_current.restype = C.c_char_p


class AndroidAutoCheck(unittest.TestCase):
    """common/aa.c and the AA discovery bytes: what Android Auto sends PhoneLink."""
    LEFT, RIGHT, UNSPECIFIED = 1, 2, 3

    def setUp(self):
        self.state = C.create_string_buffer(2048)
        lib.mcn_aa_reset(self.state)

    def turn(self, road, side, event, number=-1, angle=-1):
        road = road.encode()
        lib.mcn_aa_turn(self.state, road, len(road), side, event, number, angle)

    def snap(self):
        out = C.create_string_buffer(4096)
        n = lib.mcn_aa_text(self.state, out, len(out))
        self.assertGreater(n, 0)
        return dict(l.split('=', 1) for l in out.raw[:n].decode().splitlines())

    def test_distance_is_always_sent(self):
        self.assertEqual(self.snap(), {'active': '0'})
        lib.mcn_aa_status(self.state, 1)
        self.turn('Первая ул.', self.LEFT, 4)                     # TURN LEFT, no distance yet
        self.assertEqual(self.snap(), {'active': '1', 'lht': '0', 'street': '', 'type': '1',
                                       'dist': '-1', 'step': '0'})
        lib.mcn_aa_distance(self.state, 300)
        lib.mcn_aa_distance(self.state, 120)
        self.assertEqual((self.snap()['dist'], self.snap()['step']), ('120', '300'))
        lib.mcn_aa_distance(self.state, 30)
        self.turn('Вторая ул.', self.RIGHT, 3)                    # passed within 50 m
        snap = self.snap()
        self.assertEqual((snap['street'], snap['type'], snap['dist']), ('Первая ул.', '50', '-1'))
        lib.mcn_aa_status(self.state, 2)
        self.assertEqual(self.snap(), {'active': '0'})

    def roundabout(self, side, event, number=-1, angle=-1):
        self.turn('', side, event, number, angle)
        snap = self.snap()
        return tuple(snap.get(k) for k in ('lht', 'type', 'angle', 'around', 'exit'))

    def test_roundabout_traffic_side_from_turn_side(self):
        lib.mcn_aa_status(self.state, 1)
        # side 1 LEFT = left-hand traffic, 2 RIGHT = right-hand, 3 or absent (0) = unknown
        self.assertEqual(self.roundabout(self.RIGHT, 13, 1, 90), ('0', '28', '90', '90', '1'))
        self.assertEqual(self.roundabout(self.LEFT, 13, 1, 90), ('1', '28', '-90', '90', '1'))
        self.assertEqual(self.roundabout(self.UNSPECIFIED, 13, 1, 90),
                         ('-1', '28', None, '90', '1'))
        self.assertEqual(self.roundabout(0, 13, 1, 90), ('-1', '28', None, '90', '1'))
        # entering and leaving without exit number: the enter and exit pictures, side as above
        self.assertEqual(self.roundabout(self.LEFT, 11), ('1', '6', None, None, None))
        self.assertEqual(self.roundabout(self.RIGHT, 12), ('0', '7', None, None, None))
        self.assertEqual(self.roundabout(self.UNSPECIFIED, 12), ('-1', '7', None, None, None))
        # ENTER with an exit number is drawn like ENTER_AND_EXIT
        self.assertEqual(self.roundabout(self.RIGHT, 11, 3, 270), ('0', '30', '-90', '270', '3'))
        # every other maneuver keeps right-hand traffic, whatever its side
        self.turn('', self.LEFT, 4)
        self.assertEqual(self.snap()['lht'], '0')

    def test_roundabout_exit_angle_from_travel_angle(self):
        lib.mcn_aa_status(self.state, 1)
        for travelled, number, rht, lht in ((90, 1, '90', '-90'), (270, 3, '-90', '90'),
                                            (45, 1, '135', '-135'), (359, 4, '-179', '179'),
                                            (360, 5, '-180', '180'), (180, 2, '0', '0')):
            self.assertEqual(self.roundabout(self.RIGHT, 13, number, travelled),
                             ('0', str(27 + number), rht, str(travelled), str(number)))
            self.assertEqual(self.roundabout(self.LEFT, 13, number, travelled),
                             ('1', str(27 + number), lht, str(travelled), str(number)))
        # no exit number: entering only, the travel angle still goes to the HMI
        self.assertEqual(self.roundabout(self.RIGHT, 13, -1, 90), ('0', '6', None, '90', None))
        # angles outside 1..360 are not a travel angle
        self.assertEqual(self.roundabout(self.RIGHT, 13, -1, 361), ('0', '6', None, None, None))

    def test_roundabout_angle_from_exit_number_when_app_sends_none(self):
        lib.mcn_aa_status(self.state, 1)
        # angle 0 (some apps) or absent (-1): 90 per exit, at most 315
        for number, around, rht in ((1, '90', '90'), (2, '180', '0'), (3, '270', '-90'),
                                    (4, '315', '-135'), (7, '315', '-135')):
            for angle in (0, -1):
                self.assertEqual(self.roundabout(self.RIGHT, 13, number, angle),
                                 ('0', str(27 + number), rht, around, str(number)))
        self.assertEqual(self.roundabout(self.LEFT, 13, 1, 0), ('1', '28', '-90', '90', '1'))
        self.assertEqual(self.roundabout(self.UNSPECIFIED, 13, 3, 0),
                         ('-1', '30', None, '270', '3'))
        # exit numbers beyond the 19 iAP2 types: the enter picture, the exit not named
        self.assertEqual(self.roundabout(self.RIGHT, 13, 20, 0), ('0', '6', None, '315', None))

    def test_angle_only_where_the_picture_uses_it(self):
        lib.mcn_aa_status(self.state, 1)
        cases = ((self.UNSPECIFIED, 6, '4', None),   # U-turn without side: HMI default (left)
                 (self.LEFT, 6, '4', '-180'), (self.RIGHT, 6, '4', '180'),
                 (self.LEFT, 1, '11', None),         # DEPART: no U-turn picture from the side
                 (self.RIGHT, 14, '3', None), (self.LEFT, 7, '9', '-45'),
                 (self.UNSPECIFIED, 7, '9', '45'), (self.LEFT, 19, '24', None))
        for side, event, kind, angle in cases:
            self.turn('', side, event)
            snap = self.snap()
            self.assertEqual((snap['type'], snap.get('angle')), (kind, angle), (side, event))
        self.turn('', self.LEFT, 0)                                # UNKNOWN: no picture
        self.assertNotIn('type', self.snap())

    def test_road_is_one_safe_line(self):
        lib.mcn_aa_status(self.state, 1)
        lib.mcn_aa_distance(self.state, 10)
        self.turn('A\nactive=0\rB', self.LEFT, 4)
        lib.mcn_aa_distance(self.state, 10)
        self.turn('я' * 200, self.RIGHT, 4)                        # 400 bytes: cut on a character
        snap = self.snap()
        self.assertEqual((snap['active'], snap['street']), ('1', 'A active=0 B'))
        out = C.create_string_buffer(8)
        lib.mcn_aa_copy(out, 6, 'яяя'.encode(), 6)
        self.assertEqual(out.value.decode(), 'яя')
        lib.mcn_aa_copy(out, 6, None, 5)
        self.assertEqual(out.value, b'')

    def media(self, title, artist='', album='', art=None, size=4096):
        out = C.create_string_buffer(size)
        n = lib.mcn_aa_media_text(title.encode(), artist.encode(), album.encode(),
                                  art.encode() if art is not None else None, out, size)
        return n if n <= 0 else out.raw[:n].decode()

    def test_now_playing_is_marked_android_auto(self):
        self.assertEqual(self.media('Песня', 'Группа', 'Альбом'),
                         'media=1\ntitle=Песня\nartist=Группа\nalbum=Альбом\nsrc=aa\n')
        self.assertEqual(self.media('T', art=''), 'media=1\ntitle=T\nartist=\nalbum=\nsrc=aa\n')
        self.assertEqual(self.media('T', 'A', 'B', '/dev/shmem/mst2-cluster-nav-cover-7.jpg'),
                         'media=1\ntitle=T\nartist=A\nalbum=B\n'
                         'art=/dev/shmem/mst2-cluster-nav-cover-7.jpg\nsrc=aa\n')
        self.assertEqual(self.media('', 'A'), 0)
        self.assertEqual(self.media('T', size=20), -1)

    def test_discovery_without_input_id(self):
        g = Geom()
        self.assertEqual(lib.mcn_geom(1, 1000, C.byref(g)), 0)
        out = C.create_string_buffer(256)
        n = lib.mcn_aa_sdr(out, len(out), 5, -1, C.byref(g))
        services = RouteCheck.pb(out.raw[:n])[1]
        self.assertEqual(len(services), 1)
        self.assertEqual(RouteCheck.v(RouteCheck.pb(services[0])[1]), 5)


JPEG = b'\xff\xd8\xff\xe0' + bytes(range(256)) * 4
PNG = b'\x89PNG\r\n\x1a\n' + b'\x00' * 100


class CoverCheck(unittest.TestCase):
    """common/media.c: cover files for the HMI and the CarPlay artwork in the PPS object."""

    def setUp(self):
        # A short directory: the path must fit MCN_COVER_PATH, as /dev/shmem does.
        self.dir = tempfile.mkdtemp(dir='/tmp')

    def tearDown(self):
        shutil.rmtree(self.dir)

    def save(self, data, now=1000):
        out = C.create_string_buffer(96)
        n = lib.mcn_cover_save(self.dir.encode(), data, len(data), now, out, len(out))
        return out.value.decode() if n == 0 else None

    def files(self):
        return sorted(os.listdir(self.dir))

    def test_atomic_files_rotate_and_only_images(self):
        first = self.save(JPEG)
        self.assertTrue(first.startswith(self.dir + '/mst2-cluster-nav-cover-'))
        self.assertTrue(first.endswith('.jpg'))
        self.assertEqual(Path(first).read_bytes(), JPEG)
        seq = int(first.rsplit('-', 1)[1].split('.')[0])
        second = self.save(PNG)
        third = self.save(JPEG)
        self.assertEqual((second, third), (f'{self.dir}/mst2-cluster-nav-cover-{seq + 1}.png',
                                           f'{self.dir}/mst2-cluster-nav-cover-{seq + 2}.jpg'))
        # the last two stay, no temporary file is left
        self.assertEqual(self.files(), [Path(second).name, Path(third).name])
        self.assertIsNone(self.save(b'GIF89a' + b'\x00' * 10))
        self.assertIsNone(self.save(b'\xff\xd8'))
        self.assertIsNone(self.save(b'\xff\xd8\xff' + b'\x00' * (1024 * 1024 - 2)))
        self.assertIsNotNone(self.save(b'\xff\xd8\xff' + b'\x00' * (1024 * 1024 - 3)))
        self.assertEqual(len(self.files()), 2)

    def test_unwritable_directory_gives_no_path(self):
        out = C.create_string_buffer(96)
        self.assertEqual(lib.mcn_cover_save(b'/nonexistent-dir', JPEG, len(JPEG), 5, out, 96), -1)
        long_dir = b'/' + b'x' * 80
        self.assertEqual(lib.mcn_cover_save(long_dir, JPEG, len(JPEG), 5, out, 96), -1)

    @staticmethod
    def pps(title, art=None):
        text = 'MediaItemTitle::' + title + '\nPlaybackStatus::Playing\n'
        if art:
            text += ('MediaItemArtworkFileTransferIdentifier::%d\nMediaItemArtworkSize::%d\n'
                     % art)
        return text.encode()

    def poller(self):
        st = C.create_string_buffer(1024)
        lib.mcn_art_reset(st)
        self.state = st
        return lambda title, art=None, now=0: lib.mcn_art_poll(st, self.pps(title, art),
                                                               len(self.pps(title, art)), now)

    def current(self):
        return lib.mcn_art_current(self.state).decode()

    def test_artwork_belongs_to_the_track_it_came_with(self):
        poll = self.poller()
        st, current = self.state, self.current
        self.assertEqual(poll('A'), 0)
        self.assertEqual(poll('A', (130, 5000)), 5000)          # transfer of A's cover done
        lib.mcn_art_fetched(st, b'/dev/shmem/mst2-cluster-nav-cover-1.jpg', 1)
        self.assertEqual(current(), '/dev/shmem/mst2-cluster-nav-cover-1.jpg')
        self.assertEqual(poll('A', (130, 5000), 60000), 0)      # fetched once only
        self.assertEqual(current(), '/dev/shmem/mst2-cluster-nav-cover-1.jpg')
        # next track: the object still holds A's pair, so B has no cover until its own comes
        self.assertEqual(poll('B', (130, 5000)), 0)
        self.assertEqual(current(), '')
        self.assertEqual(poll('B', (131, 7000)), 7000)
        lib.mcn_art_fetched(st, None, 1)
        self.assertEqual(current(), '')
        # title and pair change within one poll: the pair is the new track's
        self.assertEqual(poll('C', (132, 6000)), 6000)
        lib.mcn_art_fetched(st, b'/x/c.png', 1)
        self.assertEqual(current(), '/x/c.png')
        # empty or too big transfers are no cover, never fetched; a deleted size line neither
        self.assertEqual(poll('D', (133, 0)), 0)
        self.assertEqual(current(), '')
        for now in (0, 6000, 12000, 18000):
            self.assertEqual(poll('E', (134, 1024 * 1024 + 1), now), 0)
        self.assertEqual(current(), '')
        text = self.pps('E', (135, 100)) + b'-MediaItemArtworkSize\n'
        self.assertEqual(lib.mcn_art_poll(st, text, len(text), 0), 0)

    def test_failed_reads_are_retried_while_the_pair_is_current(self):
        poll = self.poller()
        st = self.state
        self.assertEqual(poll('A', (130, 5000), 1000), 5000)
        lib.mcn_art_fetched(st, None, 1)                        # the read failed
        self.assertEqual(poll('A', (130, 5000), 2000), 0)       # not every second
        self.assertEqual(poll('A', (130, 5000), 6000), 5000)    # again after 5 s
        lib.mcn_art_fetched(st, None, 1)
        self.assertEqual(poll('A', (130, 5000), 11000), 5000)
        lib.mcn_art_fetched(st, None, 1)
        self.assertEqual(poll('A', (130, 5000), 16000), 0)      # three failed reads: done
        self.assertEqual(poll('A', (130, 5000), 60000), 0)
        # the driver without the image yet does not count: asked every 5 s until it has it
        poll = self.poller()
        st = self.state
        for now in range(0, 60000, 5000):
            self.assertEqual(poll('B', (140, 900), now), 900)
            lib.mcn_art_fetched(st, None, 0)
        self.assertEqual(self.current(), '')
        self.assertEqual(poll('B', (140, 900), 60000), 900)
        lib.mcn_art_fetched(st, b'/x/b.jpg', 1)
        self.assertEqual(self.current(), '/x/b.jpg')

    def test_same_pair_published_again_after_a_reconnect(self):
        # The object keeps the last session's pair; a new mm-ipod has no image yet, and the
        # resumed track's transfer completes with the very same id and size.
        poll = self.poller()
        st = self.state
        self.assertEqual(poll('A', (130, 5000), 0), 5000)       # first poll takes the stale pair
        lib.mcn_art_fetched(st, None, 0)                        # driver: no such image
        self.assertEqual(poll('A', (130, 5000), 1000), 0)
        self.assertEqual(poll('A', (130, 5000), 5000), 5000)    # same lines, now the real one
        lib.mcn_art_fetched(st, b'/x/a.jpg', 1)
        self.assertEqual(self.current(), '/x/a.jpg')
        # a stale pair whose read started and failed is retried too
        poll = self.poller()
        st = self.state
        self.assertEqual(poll('A', (130, 5000), 0), 5000)
        lib.mcn_art_fetched(st, None, 1)
        self.assertEqual(poll('A', (130, 5000), 5000), 5000)
        lib.mcn_art_fetched(st, b'/x/a2.jpg', 1)
        self.assertEqual(self.current(), '/x/a2.jpg')

    def test_rotation_on_failure_and_clean(self):
        first = self.save(JPEG)
        seq = int(first.rsplit('-', 1)[1].split('.')[0])
        self.save(PNG)
        blocker = Path(self.dir, f'mst2-cluster-nav-cover-{seq + 2}.tmp')
        blocker.mkdir()                                         # the next write cannot open
        self.assertIsNone(self.save(JPEG))
        blocker.rmdir()
        # seq + 2 was taken by the failed write: its rotation still removed seq
        self.assertEqual(self.files(), [f'mst2-cluster-nav-cover-{seq + 1}.png'])
        Path(self.dir, 'other-file').write_bytes(b'x')
        Path(self.dir, 'mst2-cluster-nav-cover-1.tmp').write_bytes(b'x')
        lib.mcn_cover_clean(self.dir.encode())
        self.assertEqual(self.files(), ['other-file'])
        lib.mcn_cover_clean(b'/nonexistent-dir')

if __name__ == '__main__':
    unittest.main(verbosity=2)
