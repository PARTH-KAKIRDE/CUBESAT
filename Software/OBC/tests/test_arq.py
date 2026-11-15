"""Selective Repeat file downlink (comms/arq.py): codec, sender, receiver, lossy channel."""
import random
import zlib

import pytest

import icd_defs as I  # noqa: E402
from OBC.comms import arq, ground  # noqa: E402


def test_constants_add_up():
    assert I.GL_FILE_SHORT_DATA == I.GL_MAX_LEN - I.GL_FILE_OFS_DATA - I.GL_CRC_LEN
    assert I.GL_FILE_MAX_DATA == I.GL_DOWN_MAX_LEN - I.GL_FILE_OFS_DATA - I.GL_CRC_LEN
    assert I.GL_DOWN_MAX_LEN == I.TTC_TX_MAX_LEN == 255     # LoRa payload limit
    assert I.TTC_TXP_MAX_DATA == I.ICD_MAX_PAYLOAD - I.TTC_TXP_OFS_DATA
    assert I.GL_FILE_MAX_DATA <= 255                        # block size fits the u8 in FILE_START
    assert I.GL_FS_LEN <= I.GL_ACK_MAX_DATA          # FILE_START answer fits one ACK
    assert I.GL_FA_LEN <= I.GL_CMD_MAX_ARGS          # FILE_ACK fits one authenticated CMD
    assert I.GL_FILE_WINDOW_MAX == 32                # u32 bitmap
    assert I.GL_FILE_MAX_BLOCKS == 0xFFFF            # u16 block number


def test_block_codec_and_parse_downlink():
    pkt = arq.build_block(7, 300, b"x" * 17)
    assert len(pkt) == I.GL_MAX_LEN and ground.check(pkt)
    assert arq.parse_block(pkt) == (7, 300, b"x" * 17)
    d = ground.parse_downlink(pkt)
    assert d["type"] == "FILE" and d["xfer"] == 7 and d["block"] == 300
    with pytest.raises(ground.PacketError):
        arq.build_block(1, 0, b"x" * (I.GL_FILE_MAX_DATA + 1))
    with pytest.raises(ground.PacketError):
        arq.build_block(1, 0, b"")
    big = arq.build_block(2, 9, bytes(range(248)))           # v4 long block
    assert len(big) == I.GL_DOWN_MAX_LEN and arq.parse_block(big) == (2, 9, bytes(range(248)))
    assert ground.parse_downlink(big)["block"] == 9
    with pytest.raises(ground.PacketError):                  # only FILE may be long
        ground.parse_downlink(ground._seal(bytes([I.GL_SAT_ID, I.GL_T_ACK, 1]) + bytes(30), 255))
    with pytest.raises(ground.PacketError):
        arq.parse_block(pkt[:-1] + bytes([pkt[-1] ^ 1]))  # CRC


def test_start_info_round_trip():
    s = arq.FileSender(9, bytes(range(256)) * 3, 16, 0.0)
    info = arq.parse_start_info(s.start_info())
    assert info == {"xfer": 9, "size": 768, "blocks": 46, "crc32": zlib.crc32(bytes(range(256)) * 3),
                    "window": 16, "block": 17}
    s = arq.FileSender(9, bytes(768), 16, 0.0, block=200)
    assert arq.parse_start_info(s.start_info())["blocks"] == 4 and len(s.start_info()) == I.GL_FS_LEN
    bad = dict(info, block=200)                              # block count no longer matches
    with pytest.raises(ground.PacketError):
        arq.FileReceiver(bad)


def test_sender_limits():
    with pytest.raises(ValueError):
        arq.FileSender(1, b"", 16, 0)
    with pytest.raises(ValueError):
        arq.FileSender(1, b"x", 0, 0)
    with pytest.raises(ValueError):
        arq.FileSender(1, b"x", 33, 0)
    with pytest.raises(ValueError):
        arq.FileSender(1, bytes(17 * 65535 + 1), 16, 0)
    arq.FileSender(1, bytes(17 * 65535), 16, 0)       # exactly the largest file
    for b in (0, I.GL_FILE_MAX_DATA + 1):
        with pytest.raises(ValueError):
            arq.FileSender(1, b"x", 16, 0, block=b)
    arq.FileSender(1, bytes(248 * 65535), 16, 0, block=248)   # ~16 MB with the longest blocks


def test_selective_repeat_resends_only_the_gaps():
    data = bytes(random.Random(1).randrange(256) for _ in range(17 * 40 + 5))   # 41 blocks
    s = arq.FileSender(3, data, 8, 0)
    assert list(s.pending) == list(range(8))
    # Ground got blocks 0..7 except 2 and 5.
    r = arq.FileReceiver(arq.parse_start_info(s.start_info()))
    for k in range(8):
        pkt = s.next_packet()
        s.pop()
        if k not in (2, 5):
            assert r.feed(pkt)
    x, base, bitmap = arq.parse_ack_args(r.ack_args())
    assert (x, base) == (3, 2) and bitmap == 0b110110     # bit i = block 2+i: 3, 4, 6, 7
    n = s.on_ack(base, bitmap, 1)
    # Window is now [2, 10): resend 2 and 5 only, plus new blocks 8 and 9. Not 3, 4, 6, 7.
    assert list(s.pending) == [2, 5, 8, 9] and n == 4
    assert s.base == 2


def test_stale_ack_never_slides_back():
    s = arq.FileSender(1, bytes(17 * 20), 4, 0)
    s.on_ack(8, 0, 0)
    s.on_ack(2, 0, 0)            # older information
    assert s.base == 8 and list(s.pending) == [8, 9, 10, 11]


def test_done_and_window_never_exceeded():
    s = arq.FileSender(1, bytes(17 * 10), 4, 0)
    assert s.on_ack(10, 0, 0) == 0 and s.done
    with pytest.raises(ValueError):
        s.on_ack(11, 0, 0)


def test_receiver_rejects_foreign_and_bad_lengths():
    s = arq.FileSender(5, bytes(17 * 2 + 3), 4, 0)
    r = arq.FileReceiver(arq.parse_start_info(s.start_info()))
    assert not r.feed(arq.build_block(6, 0, bytes(17)))     # other transfer
    assert not r.feed(arq.build_block(5, 3, bytes(3)))      # beyond the end
    assert not r.feed(arq.build_block(5, 2, bytes(17)))     # last block must be 3 bytes
    assert not r.feed(arq.build_block(5, 0, bytes(3)))      # middle block must be full
    assert r.feed(s.packet(0)) and not r.feed(s.packet(0)) and r.duplicates == 1
    with pytest.raises(ground.PacketError):
        r.data()


@pytest.mark.parametrize("loss,window,seed,block", [(0.0, 16, 1, 17), (0.1, 16, 2, 17), (0.3, 16, 3, 17),
                                                    (0.3, 32, 4, 17), (0.5, 8, 5, 17), (0.2, 1, 6, 17),
                                                    (0.0, 16, 7, 200), (0.2, 16, 8, 200), (0.4, 32, 9, 248),
                                                    (0.1, 4, 10, 1)])
def test_lossy_channel_delivers_the_exact_file(loss, window, seed, block):
    """Half-duplex rounds: burst down, one FILE_ACK up. Both directions lose packets."""
    rnd = random.Random(seed)
    data = bytes(rnd.randrange(256) for _ in range(5000 if block > 1 else 600))
    s = arq.FileSender(1, data, window, 0, block=block)
    r = arq.FileReceiver(arq.parse_start_info(s.start_info()))
    rounds = 0
    while not s.done:
        rounds += 1
        assert rounds < 5000
        sent_this_round = set()
        while (pkt := s.next_packet()) is not None:
            k = s.pending[0]
            assert s.base <= k < s.base + window       # never outside the window
            assert k not in sent_this_round             # never twice per request
            sent_this_round.add(k)
            s.pop()
            if rnd.random() >= loss:
                r.feed(pkt)
        if rnd.random() < loss:
            continue                                    # FILE_ACK lost: ground's timer fires, it asks again
        x, base, bitmap = arq.parse_ack_args(r.ack_args())
        s.on_ack(base, bitmap, rounds)
    assert r.done and r.data() == data
    # Selective Repeat: only lost blocks come again, so the overhead tracks the loss.
    overhead = s.sent_total / s.blocks - 1
    if loss == 0:
        assert s.sent_total == s.blocks
    else:
        assert overhead < 2.5 * loss / (1 - loss) + 0.05, overhead
