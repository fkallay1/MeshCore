import sys, struct, hashlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_sender as S

def test_bridge_plaintext_equals_companion_wrapping():
    # data čo companion CMD_SEND_CHANNEL_DATA posiela = [ts4][ota_payload]
    ota_payload = bytes([S.OTA_PKT_CHUNK]) + b"\x00"*12 + b"X"*144   # plný chunk
    ts = 0x11223344
    bridge_plain = S.grpdata_plaintext(ota_payload, ts)
    data = struct.pack('<I', ts) + ota_payload
    companion_plain = S.companion_grpdata_plaintext(S.OTA_MAGIC, data)
    assert bridge_plain == companion_plain          # bajt-identické
    assert len(data) <= 165                          # firmware limit

def test_all_types_fit_165():
    ps = ns = os_ = b"\x00"*32
    meta = S.build_meta_payload(1, 100, ps, ns, os_)
    sig  = S.build_sig_payload(meta, None, 1)
    chunk = bytes([S.OTA_PKT_CHUNK]) + b"\x00"*12 + b"X"*144
    for p in (meta, sig, chunk):
        assert 4 + len(p) <= 165
