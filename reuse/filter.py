import binascii
import hashlib
import os
import struct

import zstandard as zstd


class DiskFilter:
    def __init__(self, cache_dir):
        self.cache_dir = cache_dir
        self.bin_path  = f"{cache_dir}/cache.bin"
        self.blacklist_path = f"{cache_dir}/blacklist.txt"


    def cache_records(self):
        if not os.path.exists(self.bin_path):
            return
        with open(self.bin_path, "rb") as f:
            b = f.read(1)
            if not b:
                return
            ver = b[0]
            if ver in (1, 2):
                def _read_one_rec(first_ver):
                    if first_ver == 1:
                        rest = f.read(1 + 32)
                        if len(rest) != 33:
                            return None
                        vcode = struct.unpack("b", rest[0:1])[0]
                        h = rest[1:33]
                        return (vcode, h, None, 0)
                    elif first_ver == 2:
                        rest = f.read(1 + 1 + 32)
                        if len(rest) != 34:
                            return None
                        vcode = struct.unpack("b", rest[0:1])[0]
                        flags = rest[1]
                        h = rest[2:34]
                        return (vcode, h, None, flags)
                    else:
                        return None
                first = _read_one_rec(ver)
                if first is not None:
                    yield first
                while True:
                    vb = f.read(1)
                    if not vb:
                        break
                    v = vb[0]
                    rec = _read_one_rec(v)
                    if rec is None:
                        break
                    yield rec
            else:
                f.seek(0, os.SEEK_SET)
                for raw in f:
                    try:
                        line = raw.decode("utf-8", errors="replace")
                    except Exception:
                        line = str(raw)
                    line = line.rstrip("\n")
                    if not line or "\t" not in line:
                        continue
                    val, keytext = line.split("\t", 1)
                    try:
                        vcode = int(val.strip())
                    except Exception:
                        continue
                    h = hashlib.sha256(keytext.encode("utf-8", errors="replace")).digest()
                    yield (vcode, h, keytext, None)


    def count(self, data_path, variable):
        if os.path.exists(data_path):
            with open(data_path, "r") as f:
                lines = [line.strip() for line in f.readlines()]
                for line in lines:
                    cache, cost = line.split()
                    if cache in variable.keys():
                        variable[cache] += int(cost)
        return variable


    def normalize(self, d):
        values = list(d.values())
        v_min = min(values)
        v_max = max(values)
        if v_max == v_min:
            return {k: 0.0 for k in d}
        return {k: (v - v_min) / (v_max - v_min) for k, v in d.items()}


    def prune_cache_bin(self, drop_hex_list):
        if not os.path.exists(self.bin_path):
            return

        drop_set = {binascii.unhexlify(h) for h in drop_hex_list}
        tmp_path = self.bin_path + ".tmp"

        with open(self.bin_path, "rb") as fin, open(tmp_path, "wb") as fout:
            b = fin.read(1)
            if not b:
                pass
            else:
                ver0 = b[0]
                if ver0 in (1, 2):
                    def read_one_record(first_ver):
                        if first_ver == 1:
                            rest = fin.read(1 + 32)
                            if len(rest) != 33:
                                return None
                            vcode = struct.unpack("b", rest[0:1])[0]
                            h = rest[1:33]
                            return (vcode, 0, h)
                        elif first_ver == 2:
                            rest = fin.read(1 + 1 + 32)
                            if len(rest) != 34:
                                return None
                            vcode = struct.unpack("b", rest[0:1])[0]
                            flags = rest[1]
                            h = rest[2:34]
                            return (vcode, flags, h)
                        else:
                            return None

                    rec = read_one_record(ver0)
                    if rec is not None:
                        vcode, flags, h = rec
                        if h not in drop_set:
                            fout.write(bytes([2]))
                            fout.write(struct.pack("b", vcode))
                            fout.write(bytes([flags & 0xFF]))
                            fout.write(h)
                    while True:
                        vb = fin.read(1)
                        if not vb:
                            break
                        v = vb[0]
                        rec = read_one_record(v)
                        if rec is None:
                            break
                        vcode, flags, h = rec
                        if h in drop_set:
                            continue
                        fout.write(bytes([2]))
                        fout.write(struct.pack("b", vcode))
                        fout.write(bytes([flags & 0xFF]))
                        fout.write(h)
                else:
                    fin.seek(0, os.SEEK_SET)
                    for raw in fin:
                        try:
                            line = raw.decode("utf-8", errors="replace")
                        except Exception:
                            line = str(raw)
                        line = line.rstrip("\n")
                        if not line or "\t" not in line:
                            continue
                        val, keytext = line.split("\t", 1)
                        try:
                            _ = int(val.strip())
                        except Exception:
                            continue
                        h = hashlib.sha256(keytext.encode("utf-8", errors="replace")).digest()
                        if h in drop_set:
                            continue
                        fout.write((line + "\n").encode("utf-8"))
        os.replace(tmp_path, self.bin_path)


    def prune_dict_files(self, drop_hex_list):
        blob_path = os.path.join(self.cache_dir, "cache.dict")
        idx_path  = os.path.join(self.cache_dir, "cache.idx")

        if not (os.path.exists(blob_path) and os.path.exists(idx_path)):
            return

        drop_set = {binascii.unhexlify(h) for h in drop_hex_list}
        entries = []
        with open(idx_path, "rb") as fx:
            while True:
                h = fx.read(32)
                if not h or len(h) < 32:
                    break
                off_le = fx.read(8)
                len_le = fx.read(4)
                fl_b   = fx.read(1)
                if len(off_le) < 8 or len(len_le) < 4 or len(fl_b) < 1:
                    break
                old_off = struct.unpack("<Q", off_le)[0]
                old_len = struct.unpack("<I", len_le)[0]
                flags   = fl_b[0]
                if h in drop_set:
                    continue
                entries.append((h, old_off, old_len, flags))

        tmp_blob = blob_path + ".tmp"
        tmp_idx  = idx_path + ".tmp"

        with open(blob_path, "rb") as fblob, \
            open(tmp_blob, "wb") as nblob, \
            open(tmp_idx,  "wb") as nidx:

            curr_off = 0
            for h, old_off, old_len, flags in entries:
                fblob.seek(old_off)
                payload = fblob.read(old_len)
                if len(payload) != old_len:
                    continue
                nblob.write(payload)
                nidx.write(h)
                nidx.write(struct.pack("<Q", curr_off))
                nidx.write(struct.pack("<I", old_len))
                nidx.write(bytes([flags & 0xFF]))
                curr_off += old_len
        os.replace(tmp_blob, blob_path)
        os.replace(tmp_idx,  idx_path)


    def make_blacklist(self, filtered):
        blacklist = filtered
        if os.path.exists(self.blacklist_path):
            with open(self.blacklist_path, "r") as r_f:
                blacklist = blacklist + [l.strip() for l in r_f.readlines()]
        blacklist = list(set(blacklist))
        with open(self.blacklist_path, "w") as w_f:
            for bl in blacklist:
                w_f.write(f"{bl}\n")


    def filtering(self, iteration_dir):
        def _hex32(b):
            return binascii.hexlify(b).decode("ascii") if b is not None else None

        iter_path = f"{os.getcwd()}/{iteration_dir}"
        hashed_caches = [_hex32(rec[1]) for rec in self.cache_records()]

        time_cost = {cache : 0 for cache in hashed_caches}
        hit_count = {cache : 0 for cache in hashed_caches}
        
        time_log_path =  f"{iter_path}/disk_cache_solve_time.log"
        hit_log_path =  f"{iter_path}/disk_cache_hit.count"
        last_query_log_path = f"{iter_path}/disk_cache_last_hit.log"

        if os.path.exists(time_log_path) and os.path.exists(hit_log_path) and os.path.exists(last_query_log_path):
            time_cost = self.count(time_log_path, time_cost)
            hit_count = self.count(hit_log_path, hit_count)
            for key in time_cost.keys():
                if key not in hit_count.keys():
                    hit_count[key] = 0
            last_count = self.count(last_query_log_path, hit_count)
            for key in time_cost.keys():
                if key not in last_count.keys():
                    last_count[key] = 0

            time_cost["default"] = sum(time_cost.values()) / len(time_cost.values())
            hit_count = {key : value + 1 for key, value in hit_count.items()}
            hit_count["default"] = 0  
            last_count = {key : value + 1 for key, value in last_count.items()}
            last_count["default"] = 0

            time_cost = self.normalize(time_cost)
            hit_count = self.normalize(hit_count)
            last_count = self.normalize(last_count)

            scores = {cache : time_cost[cache] + hit_count[cache] + last_count[cache] for cache in hashed_caches}
            threshold = time_cost["default"] + hit_count["default"] + last_count["default"]
            # scores = {cache : time_cost[cache] + hit_count[cache] for cache in hashed_caches}
            # threshold = time_cost["default"] + hit_count["default"]
            filtered = [cache for cache, value in scores.items() if value <= threshold]
            # self.make_blacklist(filtered)
            self.prune_cache_bin(filtered)
            self.prune_dict_files(filtered)


class CexFilter:
    def __init__(self, cache_dir):
        self.cache_dir = cache_dir
        self.idx_path  = f"{cache_dir}/cex_cache.idx"
        self.dict_path = f"{cache_dir}/cex_cache.dict"
        self.bin_path  = f"{cache_dir}/cex_cache.bin"
        self.blacklist_path = f"{cache_dir}/blacklist.txt"

    def _hex32(self, b):
        return binascii.hexlify(b).decode("ascii") if b is not None else None


    def _dedup_keep_order(self, xs):
        seen = set()
        out = []
        for x in xs:
            if x in seen:
                continue
            seen.add(x)
            out.append(x)
        return out


    def _is_unsat(self, sat):
        return sat == 0


    def read_idx(self):
        idx = dict()
        with open(self.idx_path, "rb") as f:
            while True:
                b = f.read(32 + 8 + 4 + 1)
                if not b or len(b) < 45:
                    break
                h   = b[:32]
                off = struct.unpack("<Q", b[32:40])[0]
                ln  = struct.unpack("<I", b[40:44])[0]
                flg = b[44]
                idx[h] = (off, ln, flg)
        return idx


    def _maybe_decompress_zstd(self, blob, flags):
        if flags & 0x01:
            try:
                return zstd.ZstdDecompressor().decompress(blob)
            except Exception:
                return blob
        return blob


    def load_dict(self, idx):
        mapping = dict()
        with open(self.dict_path, "rb") as f:
            for h, (off, ln, flg) in idx.items():
                f.seek(off)
                blob = f.read(ln)
                txt = self._maybe_decompress_zstd(blob, flg).decode("utf-8", errors="ignore")
                mapping[h] = txt
        return mapping


    def _read_one_v3(self, fin):
        sat_b = fin.read(1)
        if not sat_b or len(sat_b) < 1:
            return None
        sat = struct.unpack("b", sat_b)[0]

        h = fin.read(32)
        if not h or len(h) < 32:
            return None

        def _read_u32():
            b = fin.read(4)
            if not b or len(b) < 4:
                return None
            return struct.unpack("<I", b)[0]

        cLen = _read_u32()
        if cLen is None:
            return None
        cBytes = fin.read(cLen) if cLen else b""
        constraints = cBytes.decode("utf-8", errors="ignore") if cLen else ""

        sLen = _read_u32()
        if sLen is None:
            return None
        sBytes = fin.read(sLen) if sLen else b""
        symbolsCSV = sBytes.decode("utf-8", errors="ignore") if sLen else ""

        vFlag_b = fin.read(1)
        if not vFlag_b or len(vFlag_b) < 1:
            return None
        vFlags = vFlag_b[0]

        vLen = _read_u32()
        if vLen is None:
            return None
        vBytes = fin.read(vLen) if vLen else b""

        value = ""
        if sat == 1:
            vPayload = self._maybe_decompress_zstd(vBytes, vFlags) if vLen else b""
            value = vPayload.decode("utf-8", errors="ignore")

        return (h, sat, constraints, symbolsCSV, value)


    def _read_one_v1_v2(self, fin):
        head = fin.read(1)
        if not head or len(head) < 1:
            return None
        ver = head[0]
        if ver not in (1, 2):
            return None

        if ver == 2:
            rest = fin.read(1 + 32 + 1 + 4)
            if len(rest) < 38:
                return None
            sat = struct.unpack("b", rest[0:1])[0]
            h   = rest[1:33]
            flg = rest[33]
            ln  = struct.unpack("<I", rest[34:38])[0]
            payload = fin.read(ln) if ln else b""
            return (2, h, payload, sat, flg)

        rest = fin.read(1 + 32 + 4)
        if len(rest) < 37:
            return None
        sat = struct.unpack("b", rest[0:1])[0]
        h   = rest[1:33]
        ln  = struct.unpack("<I", rest[33:37])[0]
        payload = fin.read(ln) if ln else b""
        return (1, h, payload, sat, 0)


    def iter_cex_records(self, dictmap=None):
        if not os.path.exists(self.bin_path):
            return

        with open(self.bin_path, "rb") as f:
            while True:
                head = f.read(1)
                if not head:
                    break
                ver = head[0]

                if ver == 3:
                    rec = self._read_one_v3(f)
                    if rec is None:
                        break
                    h, sat, constraints, symbolsCSV, value = rec
                    yield (h, sat, constraints, symbolsCSV, value)
                    continue

                f.seek(-1, 1)
                rec_legacy = self._read_one_v1_v2(f)
                if rec_legacy is None:
                    break
                ver_l, h, payload, sat, flg = rec_legacy
                constraints = dictmap.get(h, "") if dictmap else ""
                if sat != 1:
                    value = ""
                else:
                    data = payload
                    if ver_l == 2 and (flg & 0x01):
                        data = self._maybe_decompress_zstd(payload, flg)
                    value = data.decode("utf-8", errors="ignore")

                symbolsCSV = ""
                yield (h, sat, constraints, symbolsCSV, value)


    def collect_unsat_keys(self, dictmap=None):
        unsat_hex = []
        for h, sat, _, _, _ in self.iter_cex_records(dictmap):
            if self._is_unsat(sat):
                unsat_hex.append(self._hex32(h))
        return self._dedup_keep_order(unsat_hex)

    def remove_all_unsat(self, dictmap=None, also_blacklist=False, dry_run=False):
        drop_hex = self.collect_unsat_keys(dictmap=dictmap)
        if dry_run:
            return
        if not drop_hex:
            return
        if also_blacklist:
            self.make_blacklist(drop_hex)
        self.prune_cex_cache_bin(drop_hex, dictmap or {})
        self.prune_cex_dict_files(drop_hex)
        return len(drop_hex)


    def count(self, data_path, variable):
        total = list()
        if os.path.exists(data_path):
            with open(data_path, "r") as f:
                lines = [line.strip() for line in f.readlines()]
                for line in lines:
                    cache, cost = line.split()
                    if cache in variable.keys():
                        variable[cache] += int(cost)
                        total.append(int(cost))
        return variable


    def normalize(self, d):
        values = list(d.values())
        v_min = min(values)
        v_max = max(values)
        if v_max == v_min:
            return {k: 0.0 for k in d}
        return {k: (v - v_min) / (v_max - v_min) for k, v in d.items()}


    def make_blacklist(self, filtered):
        blacklist = filtered
        if os.path.exists(self.blacklist_path):
            with open(self.blacklist_path, "r") as r_f:
                blacklist = blacklist + [l.strip() for l in r_f.readlines()]
        blacklist = list(set(blacklist))
        with open(self.blacklist_path, "w") as w_f:
            for bl in blacklist:
                w_f.write(f"{bl}\n")


    def prune_cex_cache_bin(self, drop_hex_list, dictmap):
        if not os.path.exists(self.bin_path):
            return

        drop_set_hex = set(drop_hex_list)
        tmp_path = self.bin_path + ".tmp"

        with open(tmp_path, "wb") as fout:
            kept = 0
            dropped = 0
            for h, sat, constraints, symbolsCSV, value in self.iter_cex_records(dictmap):
                key_hex = self._hex32(h)
                if key_hex in drop_set_hex:
                    dropped += 1
                    continue

                kept += 1
                ver = 3
                fout.write(struct.pack("B", ver))
                fout.write(struct.pack("b", sat))
                fout.write(h)

                c_bytes = (constraints or "").encode("utf-8")
                fout.write(struct.pack("<I", len(c_bytes)))
                if c_bytes:
                    fout.write(c_bytes)

                s_bytes = (symbolsCSV or "").encode("utf-8")
                fout.write(struct.pack("<I", len(s_bytes)))
                if s_bytes:
                    fout.write(s_bytes)

                if sat == 1 and value:
                    v_raw = value.encode("utf-8")
                    v_flags = 0
                    v_payload = v_raw
                    if v_raw:
                        cctx = zstd.ZstdCompressor(level=3)
                        v_z = cctx.compress(v_raw)
                        if len(v_z) < len(v_raw):
                            v_flags = 0x01
                            v_payload = v_z
                    fout.write(struct.pack("B", v_flags))
                    fout.write(struct.pack("<I", len(v_payload)))
                    if v_payload:
                        fout.write(v_payload)
                else:
                    fout.write(struct.pack("B", 0))
                    fout.write(struct.pack("<I", 0))

        os.replace(tmp_path, self.bin_path)


    def prune_cex_dict_files(self, drop_hex_list):
        base_dir = os.path.dirname(self.bin_path)
        blob_path = os.path.join(base_dir, "cex_cache.dict")

        if not (os.path.exists(blob_path) and os.path.exists(self.idx_path)):
            return

        drop_set = set()
        for h in drop_hex_list:
            if not h or len(h) != 64:
                continue
            try:
                drop_set.add(binascii.unhexlify(h))
            except Exception:
                continue

        entries = []
        with open(self.idx_path, "rb") as fx:
            while True:
                h = fx.read(32)
                if not h or len(h) < 32:
                    break
                off_le = fx.read(8)
                len_le = fx.read(4)
                fl_b   = fx.read(1)
                if len(off_le) < 8 or len(len_le) < 4 or len(fl_b) < 1:
                    break
                old_off = struct.unpack("<Q", off_le)[0]
                old_len = struct.unpack("<I", len_le)[0]
                flags   = fl_b[0]
                if h in drop_set:
                    continue
                entries.append((h, old_off, old_len, flags))

        tmp_blob = blob_path + ".tmp"
        tmp_idx  = self.idx_path + ".tmp"

        with open(blob_path, "rb") as fblob, \
            open(tmp_blob, "wb") as nblob, \
            open(tmp_idx,  "wb") as nidx:

            curr_off = 0
            for h, old_off, old_len, flags in entries:
                fblob.seek(old_off)
                payload = fblob.read(old_len)
                if len(payload) != old_len:
                    continue
                nblob.write(payload)
                nidx.write(h)
                nidx.write(struct.pack("<Q", curr_off))
                nidx.write(struct.pack("<I", old_len))
                nidx.write(bytes([flags & 0xFF]))
                curr_off += old_len

        os.replace(tmp_blob, blob_path)
        os.replace(tmp_idx,  self.idx_path)


    def filtering(self, iteration_dir):
        iter_path = f"{os.getcwd()}/{iteration_dir}"
        dictmap = {}
        try:
            idx = self.read_idx()
            dictmap = self.load_dict(idx)
        except FileNotFoundError:
            dictmap = {}
        num_unsat = self.remove_all_unsat(dictmap=dictmap, also_blacklist=False, dry_run=False)
        return num_unsat
        