#!/usr/bin/env python3
"""Exercise a running Mac companion without a headset; does not verify XR rendering.

Run: python3 android/tests/wire_smoke.py [--host 127.0.0.1] [--seconds 5]
The Mac companion permits one client. Run when no headset/test client is connected.
"""
import argparse
import json
import math
import re
import socket
import struct
import threading
import time

MAX_PAYLOAD = 8 << 20


def packet(kind, body):
    return struct.pack('<BI', kind, len(body)) + body


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size-len(data))
        if not part:
            raise EOFError('Companion closed connection')
        data.extend(part)
    return bytes(data)


def receive(sock):
    kind, size = struct.unpack('<BI', exact(sock, 5))
    if size > MAX_PAYLOAD:
        raise ValueError(f'Oversized payload: {size}')
    return kind, exact(sock, size)


def pose(x=0, y=1.6, z=0):
    return struct.pack('<7f', x, y, z, 0, 0, 0, 1)


def tracking(timestamp, x):
    head = pose(x=x)
    eyes = b''.join(pose(x=x+offset) + struct.pack('<4f', -.8, .8, .8, -.8)
                    for offset in (-.032, .032))
    # Inactive controllers: valid identity poses and neutral inputs.
    hand = struct.pack('<II', 0, 0) + pose(y=0) * 2 + struct.pack('<4f', 0, 0, 0, 0)
    body = struct.pack('<Q', timestamp) + head + eyes + hand * 2
    assert len(body) == 284
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9945)
    parser.add_argument('--seconds', type=float, default=5)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    hello = json.dumps({'device':'Wire smoke test', 'eye_w':512, 'eye_h':512,
                        'refresh_rates':[72], 'codecs':['h264'], 'reference_space':'stage'}).encode()
    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # Explicitly fragment header/body; a correct TCP reader must reassemble them.
        framed = packet(1, hello)
        for segment in (framed[:2], framed[2:5], framed[5:11], framed[11:]):
            sock.sendall(segment)
            time.sleep(.02)
        kind, body = receive(sock)
        assert kind == 2, f'Expected CONFIG, received packet {kind}'
        config = json.loads(body)
        assert config['codec'] == 'h264', config
        assert 0 < config['eye_w'] <= 2048 and 0 < config['eye_h'] <= 2048, config
        print('CONFIG', config)
        sock.sendall(packet(7, b''))
        stop = threading.Event()
        sent = set()
        lock = threading.Lock()
        sender_errors = []

        def send_samples():
            try:
                start = time.monotonic()
                while not stop.is_set():
                    timestamp = time.monotonic_ns() + 20_000_000
                    with lock:
                        sent.add(timestamp)
                    sock.sendall(packet(3, tracking(timestamp, .1*math.sin(time.monotonic()-start))))
                    stop.wait(1/72)
            except OSError as error:
                if not stop.is_set():
                    sender_errors.append(error)

        sender = threading.Thread(target=send_samples, daemon=True)
        sender.start()
        frames, idrs, requested_at, recovery_verified = 0, 0, None, False
        deadline = time.monotonic() + args.seconds
        try:
            while time.monotonic() < deadline:
                kind, body = receive(sock)
                if kind == 5:
                    assert len(body) == 13
                    continue
                assert kind == 4, f'Unexpected packet type {kind}'
                assert len(body) > 17, 'Missing encoded frame'
                frame_id, timestamp, flags = struct.unpack('<QQB', body[:17])
                with lock:
                    assert timestamp in sent, f'Unknown render timestamp {timestamp}'
                nals = [part[0] & 31 for part in re.split(b'\x00\x00\x00?\x01', body[17:])[1:] if part]
                assert nals, 'Missing Annex-B start codes'
                if flags & 1:
                    assert nals[0] == 7 and 8 in nals and 5 in nals, f'IDR lacks ordered SPS/PPS: {nals}'
                    idrs += 1
                    if requested_at is not None and frame_id > requested_at:
                        recovery_verified = True
                frames += 1
                if frames == 3:
                    sock.sendall(packet(7, b''))
                    requested_at = frame_id
            if sender_errors:
                raise sender_errors[0]
            assert frames >= 4, f'Only {frames} frames received'
            assert idrs >= 1, 'No IDR received'
            assert recovery_verified, 'REQUEST_IDR did not produce a later IDR'
            print(f'PASS: {frames} frames, {idrs} IDRs; exact timestamps, Annex-B SPS/PPS, recovery, fragmented HELLO')
        finally:
            stop.set()
            sock.shutdown(socket.SHUT_RDWR)
            sender.join(timeout=2)


if __name__ == '__main__':
    main()
