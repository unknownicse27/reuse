import os
import re
import json
import hashlib
import subprocess as sp

from z3 import Solver, parse_smt2_string, is_bool, is_and, is_bv, is_bv_value, BoolVal, unsat


class CoreExtractor:
    def __init__(self, kleaver_bin, cache_path):
        self.kleaver_bin = kleaver_bin
        self.cache_path = cache_path
        self.enable_formula_dict = False
        self.use_compact_cache = True
        self.append_cores = True
        self._seen_mem = None
        self._core_set_index_mem = None
        self._core_sets_mem = None
        self.formula_dict_path = os.path.join(self.cache_path, "unsat_core_dict.txt")
        self.seen_path = os.path.join(self.cache_path, "unsat_seen.jsonl")
        

    
    def extract_unsat_queries(self, iteration_path):
        unsat_queries = list()
        if os.path.exists(f"{iteration_path}/solver-queries.kquery"):
            with open(f"{iteration_path}/solver-queries.kquery", "r") as f:
                content = f.read().strip()
                queries = [query for query in content.split('\n\n') if "Solvable: false" in query]
                for query in queries:
                    lines = [l for l in query.split("\n") if l and l[0] != "#"]
                    valid_query = "\n".join(lines)
                    unsat_queries.append(valid_query)
        return unsat_queries


    def kquery_to_smt(self, kquery):
        with open(f"{self.cache_path}/test.kquery", "w") as f:
            f.write(kquery)
        kleaver_cmd = f"{self.kleaver_bin} --print-smtlib --solver-backend=z3 {self.cache_path}/test.kquery"
        result = sp.run(kleaver_cmd, shell=True, capture_output=True, text=True)
        return result.stdout


    def kquery_file_to_smt(self, kquery_path: str) -> str:
        kleaver_cmd = f"{self.kleaver_bin} --print-smtlib --solver-backend=z3 {kquery_path}"
        result = sp.run(kleaver_cmd, shell=True, capture_output=True, text=True)
        return result.stdout


    def _split_kquery_blocks(self, kquery_text: str):
        return [q for q in kquery_text.split("\n\n") if q.strip()]


    def _split_smt2_by_checksat(self, smt2_all: str):
        blocks = []
        cur = []
        for line in smt2_all.splitlines(True):
            cur.append(line)
            if line.strip().startswith("(check-sat"):
                blocks.append("".join(cur))
                cur = []
        return blocks


    def _smt2_cache_key(self, smt2_block: str) -> str:
        keep = []
        for line in smt2_block.splitlines():
            t = line.strip()
            if t.startswith("(check-sat"):
                continue
            if t.startswith("(get-value"):
                continue
            if t.startswith("(get-model"):
                continue
            if t.startswith("(exit"):
                continue
            keep.append(t)
        norm = " ".join(" ".join(keep).split())
        return hashlib.blake2b(norm.encode("utf-8"), digest_size=16).hexdigest()

    def _load_seen(self):
        if self._seen_mem is not None:
            return self._seen_mem
        if not os.path.exists(self.seen_path):
            self._seen_mem = set()
            return self._seen_mem
        seen = set()
        with open(self.seen_path, "r") as f:
            for line in f:
                s = line.strip()
                if not s:
                    continue
                try:
                    v = json.loads(s)
                    if isinstance(v, str):
                        seen.add(v)
                except Exception:
                    seen.add(s)
        self._seen_mem = seen
        return self._seen_mem


    def _append_seen(self, new_keys):
        if not new_keys:
            return
        with open(self.seen_path, "a") as f:
            for k in new_keys:
                f.write(json.dumps(k, separators=(",", ":")) + "\n")
        if self._seen_mem is not None:
            self._seen_mem.update(new_keys)


    def get_unsat_core(self, smt2_string):
        def flatten_and(expr):
            if is_and(expr):
                res = []
                for ch in expr.children():
                    res.extend(flatten_and(ch))
                return res
            else:
                return [expr]

        def smt2_for_z3(smt_format):
            keep = []
            for line in smt_format.splitlines():
                t = line.strip()
                if t.startswith("(check-sat"):
                    continue
                if t.startswith("(get-value"):
                    continue
                if t.startswith("(get-model"):
                    continue
                if t.startswith("(exit"):
                    continue
                keep.append(line)
            return "\n".join(keep)

        clean = smt2_for_z3(smt2_string)
        formulas = parse_smt2_string(clean)

        bool_forms = [f for f in formulas if is_bool(f)]
        conjuncts = []
        for f in bool_forms:
            conjuncts.extend(flatten_and(f))

        solver = Solver()
        solver.set(unsat_core=True)

        res = solver.check(*conjuncts)
        if res != unsat:
            return res, [], conjuncts

        core = solver.unsat_core()
        core_set = set(core)

        core_formulas = [c for c in conjuncts if c in core_set]
        return res, core_formulas, conjuncts


    def canonicalize_array_symbol_sexpr(self, s):
        pattern = r"\(select\s+([^\s\)]+)"
        names = set(re.findall(pattern, s))
        if not names:
            return s
        res = s
        for name in names:
            res = re.sub(r"\b" + re.escape(name) + r"\b", "SYMBOL", res)
        return res


    def normalize_select_indices_relative(self, s):
        pattern = r"\(select\s+SYMBOL\s+(#x[0-9a-fA-F]+)\)"
        indices_raw = []

        def collect(m):
            h = m.group(1)
            try:
                v = int(h[2:], 16)
            except ValueError:
                return m.group(0)
            indices_raw.append((h, v))
            return m.group(0)

        re.sub(pattern, collect, s)

        if not indices_raw:
            return s

        vals = sorted({v for (_, v) in indices_raw})
        base = min(vals)
        value_to_offset = {v: i for i, v in enumerate(vals)}

        raw_to_idx = {}
        for raw, v in indices_raw:
            off = value_to_offset.get(v)
            raw_to_idx[raw] = f"IDX{off}"

        def repl(m):
            h = m.group(1)
            new_idx = raw_to_idx.get(h, h)
            return f"(select SYMBOL {new_idx})"

        new_s = re.sub(pattern, repl, s)
        return new_s


    def normalize_let_binders(self, s):
        if "(let" not in s or "a!" not in s:
            return " ".join(s.split())

        def tokenize(text):
            tokens = []
            i, n = 0, len(text)
            while i < n:
                c = text[i]
                if c.isspace():
                    i += 1
                    continue
                if c == "(" or c == ")":
                    tokens.append(c)
                    i += 1
                    continue
                j = i
                while j < n and (not text[j].isspace()) and text[j] not in "()":
                    j += 1
                tokens.append(text[i:j])
                i = j
            return tokens

        def parse(tokens):
            def parse_at(i):
                if tokens[i] != "(":
                    return tokens[i], i + 1
                i += 1
                lst = []
                while i < len(tokens) and tokens[i] != ")":
                    node, i = parse_at(i)
                    lst.append(node)
                if i >= len(tokens):
                    raise ValueError("unbalanced parens")
                return lst, i + 1

            ast, nxt = parse_at(0)
            if nxt != len(tokens):
                raise ValueError("trailing tokens")
            return ast

        def dump(node):
            if isinstance(node, str):
                return node
            return "(" + " ".join(dump(ch) for ch in node) + ")"

        def rewrite(node, env_stack):
            if isinstance(node, str):
                if node.startswith("a!") and node[2:].isdigit():
                    for env in reversed(env_stack):
                        if node in env:
                            return env[node]
                return node

            if not node:
                return node

            if isinstance(node[0], str) and node[0] == "let":
                if len(node) != 3:
                    raise ValueError("unexpected let form")

                binds = node[1]
                body = node[2]

                local_env = {}
                next_id = 0
                for b in binds:
                    if isinstance(b, list) and len(b) == 2:
                        v = b[0]
                        if isinstance(v, str) and v.startswith("a!") and v[2:].isdigit():
                            local_env[v] = "a!" + str(next_id)
                            next_id += 1

                env_stack.append(local_env)

                new_binds = []
                for b in binds:
                    if isinstance(b, list) and len(b) == 2:
                        new_binds.append([rewrite(b[0], env_stack),
                                        rewrite(b[1], env_stack)])
                    else:
                        new_binds.append(rewrite(b, env_stack))

                new_body = rewrite(body, env_stack)
                env_stack.pop()

                return ["let", new_binds, new_body]

            return [rewrite(ch, env_stack) for ch in node]

        try:
            tokens = tokenize(s)
            ast = parse(tokens)
            new_ast = rewrite(ast, [])
            out = dump(new_ast)
            return " ".join(out.split())
        except Exception:
            return " ".join(s.split())



    def normalize_hex_values_relative_formula(self, s):
        pattern = r"#x[0-9a-fA-F]+"

        hex_to_int = {}
        for h in re.findall(pattern, s):
            if h in hex_to_int:
                continue
            try:
                v = int(h[2:], 16)
            except ValueError:
                continue
            hex_to_int[h] = v

        if not hex_to_int:
            return " ".join(s.split())

        sorted_items = sorted(hex_to_int.items(), key=lambda kv: (kv[1], kv[0]))
        base_val = sorted_items[0][1]

        mapping = {}
        for h, v in sorted_items:
            if v == base_val:
                mapping[h] = "VAL0"
            else:
                mapping[h] = f"VAL0+0x{(v - base_val):x}"

        def repl(m):
            h = m.group(0)
            return mapping.get(h, h)

        s2 = re.sub(pattern, repl, s)
        return " ".join(s2.split())


    def _formula_id64(self, normalized_sexpr: str) -> int:
        s = " ".join(normalized_sexpr.split())
        h = hashlib.blake2b(s.encode("utf-8"), digest_size=8).digest()
        return int.from_bytes(h, byteorder="big", signed=False)


    def _load_existing_cores_as_ids(self, path: str):
        cores = []
        id_to_formula = {}

        if not os.path.exists(path):
            return cores, id_to_formula

        with open(path, "r") as f:
            lines = [l.strip() for l in f.readlines() if l.strip()]

        for line in lines:
            parsed = None
            try:
                parsed = json.loads(line)
            except Exception:
                parsed = None

            if isinstance(parsed, list) and parsed:
                ids = []
                ok = True
                for x in parsed:
                    if isinstance(x, int):
                        ids.append(x)
                    elif isinstance(x, str):
                        xs = x.lower()
                        if xs.startswith("0x"):
                            xs = xs[2:]
                        try:
                            ids.append(int(xs, 16))
                        except Exception:
                            ok = False
                            break
                    else:
                        ok = False
                        break
                if ok:
                    cores.append(frozenset(ids))
                continue

            try:
                data = eval(line)
                if isinstance(data, list) and data and isinstance(data[0], str):
                    ids = []
                    for s in data:
                        sid = self._formula_id64(s)
                        ids.append(sid)
                        if self.enable_formula_dict:
                            id_to_formula.setdefault(sid, s)
                    cores.append(frozenset(ids))
            except Exception:
                pass

        return cores, id_to_formula


    def read_kqueries(self, iter_versions, output_dir):
        seen = set()
        uniq_blocks = []
        for it in iter_versions:
            kq_path = f"{output_dir}/{it}/solver-queries.kquery"
            if not os.path.exists(kq_path):
                continue

            with open(kq_path, "r") as f:
                content = f.read().strip()
            if not content:
                continue

            blocks = self._split_kquery_blocks(content)

            for b in blocks:
                if not b.strip():
                    continue

                lines = []
                for l in b.splitlines():
                    s = l.strip()
                    if not s:
                        continue
                    if s.startswith("#"):
                        continue
                    if s.startswith("Instructions:"):
                        continue
                    lines.append(s)

                norm = " ".join(" ".join(lines).split())
                key = hashlib.blake2b(norm.encode("utf-8"), digest_size=16).hexdigest()

                if key in seen:
                    continue
                seen.add(key)
                uniq_blocks.append(b.strip())

        if not uniq_blocks:
            return ""
        kq_all = "\n\n".join(uniq_blocks) + "\n"
        with open(f"{output_dir}/combined.kqueries", "w") as f1:
            f1.write(kq_all)
        smt2_all = self.kquery_file_to_smt(f"{output_dir}/combined.kqueries")
        smt2_blocks = self._split_smt2_by_checksat(smt2_all)
        return uniq_blocks, smt2_blocks


    def extract_core(self, iteration_path, kq_blocks, smt2_blocks):
        cache_file = os.path.join(self.cache_path, "unsat_cores.txt")
        id_to_formula = {}
        if self._core_set_index_mem is None:
            core_sets, id_to_formula = self._load_existing_cores_as_ids(cache_file)
            self._core_sets_mem = core_sets
            self._core_set_index_mem = set(core_sets)
        core_set_index = self._core_set_index_mem
        core_sets = self._core_sets_mem
        seen = self._load_seen()
        new_seen = []
        new_core_sets = []

        try:
            use_fallback = (len(kq_blocks) == 0 or len(smt2_blocks) == 0 or len(kq_blocks) != len(smt2_blocks))

            if not use_fallback:
                for idx, kq in enumerate(kq_blocks):
                    if "Solvable: false" not in kq:
                        continue

                    smt2_string = smt2_blocks[idx]
                    key = self._smt2_cache_key(smt2_string)
                    if key in seen:
                        continue
                    seen.add(key)
                    new_seen.append(key)
                    res, core_formulas, _all_constraints = self.get_unsat_core(smt2_string)

                    if res != unsat:
                        continue

                    ids = []
                    for f in core_formulas:
                        new_f = f.sexpr()
                        new_f = new_f.replace("(= false", "(not")
                        new_f = self.normalize_let_binders(new_f)
                        new_f = self.canonicalize_array_symbol_sexpr(new_f)
                        new_f = self.normalize_select_indices_relative(new_f)
                        new_f = self.normalize_hex_values_relative_formula(new_f)
                        new_f = " ".join(new_f.split())
                        fid = self._formula_id64(new_f)
                        ids.append(fid)
                        if self.enable_formula_dict:
                            id_to_formula.setdefault(fid, new_f)

                    core_ids = frozenset(ids)
                    if core_ids not in core_set_index:
                        core_set_index.add(core_ids)
                        if core_sets is not None:
                            core_sets.append(core_ids)
                        new_core_sets.append(core_ids)
            else:
                unsat_queries = self.extract_unsat_queries(iteration_path)
                for query in unsat_queries:
                    smt2_string = self.kquery_to_smt(query)
                    key = self._smt2_cache_key(smt2_string)
                    if key in seen:
                        continue
                    seen.add(key)
                    new_seen.append(key)
                    res, core_formulas, _all_constraints = self.get_unsat_core(smt2_string)

                    if res != unsat:
                        continue

                    ids = []
                    for f in core_formulas:
                        new_f = f.sexpr()
                        new_f = new_f.replace("(= false", "(not")
                        new_f = self.normalize_let_binders(new_f)
                        new_f = self.canonicalize_array_symbol_sexpr(new_f)
                        new_f = self.normalize_select_indices_relative(new_f)
                        new_f = self.normalize_hex_values_relative_formula(new_f)
                        new_f = " ".join(new_f.split())

                        fid = self._formula_id64(new_f)
                        ids.append(fid)
                        if self.enable_formula_dict:
                            id_to_formula.setdefault(fid, new_f)

                    core_ids = frozenset(ids)
                    if core_ids not in core_set_index:
                        core_set_index.add(core_ids)
                        if core_sets is not None:
                            core_sets.append(core_ids)
                        new_core_sets.append(core_ids)

        except Exception:
            unsat_queries = self.extract_unsat_queries(iteration_path)
            for query in unsat_queries:
                smt2_string = self.kquery_to_smt(query)
                key = self._smt2_cache_key(smt2_string)
                if key in seen:
                    continue
                seen.add(key)
                new_seen.append(key)
                res, core_formulas, _all_constraints = self.get_unsat_core(smt2_string)

                if res != unsat:
                    continue

                ids = []
                for f in core_formulas:
                    new_f = f.sexpr()
                    new_f = new_f.replace("(= false", "(not")
                    # new_f = new_f.replace("(= #x00000000", "(not")
                    new_f = self.normalize_let_binders(new_f)
                    new_f = self.canonicalize_array_symbol_sexpr(new_f)
                    new_f = self.normalize_select_indices_relative(new_f)
                    new_f = self.normalize_hex_values_relative_formula(new_f)
                    new_f = " ".join(new_f.split())
    
                    fid = self._formula_id64(new_f)
                    ids.append(fid)
                    if self.enable_formula_dict:
                        id_to_formula.setdefault(fid, new_f)

                core_ids = frozenset(ids)
                if core_ids not in core_set_index:
                    core_set_index.add(core_ids)
                    if core_sets is not None:
                        core_sets.append(core_ids)
                    new_core_sets.append(core_ids)

        self._append_seen(new_seen)
        if self.use_compact_cache:
            if self.append_cores:
                if new_core_sets:
                    with open(cache_file, "a") as out:
                        for s in new_core_sets:
                            line = [format(x, "x") for x in sorted(s)]
                            out.write(json.dumps(line, separators=(",", ":")) + "\n")
            else:
                with open(cache_file, "w") as out:
                    for s in core_sets:
                        line = [format(x, "x") for x in sorted(s)]
                        out.write(json.dumps(line, separators=(",", ":")) + "\n")
        else:
            with open(cache_file, "w") as out:
                for s in core_sets:
                    line = [format(x, "x") for x in sorted(s)]
                    out.write(str(line) + "\n")

        if self.enable_formula_dict and id_to_formula:
            with open(self.formula_dict_path, "w") as df:
                for k in sorted(id_to_formula.keys()):
                    df.write(f"{format(k,'x')} {id_to_formula[k]}\n")