import hashlib, hmac, subprocess, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
d = bytes((i*31+7) & 0xff for i in range(70000))
out = subprocess.run([sys.argv[1]], capture_output=True, text=True).stdout.split("\n")
got = {}
for l in out:
    if l:
        k, v = l.rsplit(" ", 1); got[k] = v
exp = {}
for n in [0,1,55,56,63,64,135,136,137,1000,70000]:
    exp[f"sha256 {n}"] = hashlib.sha256(d[:n]).hexdigest()
    exp[f"sha3 {n}"] = hashlib.sha3_256(d[:n]).hexdigest()
exp["hmac32"] = hmac.new(d[:32], d[100:120], "sha256").hexdigest()
exp["hmac100"] = hmac.new(d[:100], d[100:600], "sha256").hexdigest()
e = Cipher(algorithms.AES(d[5:21]), modes.ECB())
exp["aesenc"] = e.encryptor().update(d[40:56]).hex()
exp["aesdec"] = e.decryptor().update(d[40:56]).hex()
key = d[7:23] + d[23:39]
def xts(sector, enc):
    c = Cipher(algorithms.AES(key), modes.XTS(sector.to_bytes(16, "little")))
    f = c.encryptor() if enc else c.decryptor()
    return hashlib.sha256(f.update(d[300:300+0x1000])).hexdigest()
exp["xtsdec"] = xts(0x800000000069, False)
exp["xtsenc"] = xts(5, True)
bad = [k for k in exp if exp[k] != got.get(k)]
print("ALL OK" if not bad else "MISMATCH: " + ", ".join(bad))
