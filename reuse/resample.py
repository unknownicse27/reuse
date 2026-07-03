import random as rd
import subprocess as sp

import os
import re


class Resampler:
    def __init__(self, ktest_tool_bin, original_path, sample_k, versions, gcov_path):
        self.ktest_tool_bin = ktest_tool_bin
        self.sample_k = sample_k
        self.versions = versions

        self.scores = dict()            # {symbol : {index : {value : score}}}
        self.prob_hint = 0.5

        for ver in versions:
            if f"/{ver}/" in gcov_path:
                init_version = ver
                break
        self.extract_valid(gcov_path, init_version)


    def extract_valid(self, gcov_path, init_version):
        self.valid_args = dict()    # {version : {idx : {value : frequency}}}
        for ver in self.versions:
            if ver != init_version:
                gcov_path = gcov_path.replace(f"/{init_version}/", f"/{ver}/")
            self.valid_args[ver] = dict()
            help_cmd = [gcov_path, "--help"]
            result = sp.run(help_cmd, capture_output=True, text=True)
            output = result.stdout + result.stderr
            options = re.findall(r'(?<!\S)--?[A-Za-z0-9][A-Za-z0-9_-]*', output)
            for option in options:
                for i in range(len(option)):
                    if i not in self.valid_args[ver].keys():
                        self.valid_args[ver][i] = dict()
                    val = ord(option[i])
                    if val in self.valid_args[ver][i].keys():
                        self.valid_args[ver][i][val] += 1
                    else:
                        self.valid_args[ver][i][val] = 1


    def extract_testcase_data(self, testcase):
        def check_modifiable(symbol):
            return (("arg" in symbol) and ("n_args" not in symbol)) or (("_data" in symbol) and ("_stat" not in symbol)) or (symbol in ["stdin"])

        tc_lens = dict()
        cmd = [self.ktest_tool_bin, testcase]
        cmd = " ".join(cmd)
        result = sp.run(cmd, shell=True, capture_output=True, text=True, timeout=1)
        output = result.stdout
        symbol_pattern = r"(object\s+\d+:\s*name:[\s\S]*?)(?=object\s+\d+:|$)"
        symbol_objs = re.findall(symbol_pattern, output)
        value_pattern = r"(object\s+\d+:[\s\S]*?)(?=object\s+\d+:|$)"
        value_objs = re.findall(value_pattern, output)

        for symbol_obj in symbol_objs:
            symbol_match = re.search(r"name:\s*'([^']+)'", symbol_obj)
            if symbol_match:
                symbol = symbol_match.group(1)
                if check_modifiable(symbol):
                    symbol_idx = symbol_obj[:symbol_obj.find(":")]
                    value_obj = [value for value in value_objs if symbol_idx in value]
                    for value in value_obj:
                        value_match = re.search(r"hex\s*:\s*0x([0-9a-fA-F]+)", value)
                        if value_match:
                            value_data = value_match.group(1)
                            break
                        else:
                            value_data = None
                    if value_data is not None:
                        # if "arg" in symbol:
                        #     tc_lens[symbol] = (len(value_data) // 2) - 1
                        # else:
                        tc_lens[symbol] = len(value_data) // 2
        return tc_lens


    def extract_query_data(self, expr):
        expr = expr.strip()
        value_with_condition = list()   # (symbol, index, value)
        patterns = [
            # e.g., (Eq 45 (Read w8 0 arg00))
            (r"\(Eq\s+(\d+)\s+\(Read\s+w\d+\s+(\d+)\s+([A-Za-z0-9_]+)\)\)", ("value", "index", "name")),
            # e.g., (Eq (Read w8 0 arg00) 45)
            (r"\(Eq\s+\(Read\s+w\d+\s+(\d+)\s+([A-Za-z0-9_]+)\)\s+(\d+)\)", ("index", "name", "value")),
            # e.g., (Eq 102 (Extract w8 0 (SExt w32 (Read w8 1 arg00))))
            (r"\(Eq\s+(\d+)\s+\(Extract\s+w\d+\s+\d+\s+\(SExt\s+w\d+\s+\(Read\s+w\d+\s+(\d+)\s+([A-Za-z0-9_]+)\)\)\)\)", ("value", "index", "name")),
            # e.g., (Eq (Extract w8 0 (SExt w32 (Read w8 1 arg00))) 102)
            (r"\(Eq\s+\(Extract\s+w\d+\s+\d+\s+\(SExt\s+w\d+\s+\(Read\s+w\d+\s+(\d+)\s+([A-Za-z0-9_]+)\)\)\)\s+(\d+)\)", ("index", "name", "value")),
        ]
        for pat, order in patterns:
            found = re.search(pat, expr)
            if found:
                g1, g2, g3 = found.groups()
                data = {}
                for key, val in zip(order, (g1, g2, g3)):
                    if key in ("value", "index"):
                        data[key] = int(val)
                    else:
                        data[key] = val
                value_with_condition.append((data["index"], data["value"]))
        data = {}
        sle_pattern = r"\(Sle\s+(\d+)\s+\(SExt\s+w\d+\s+\(Read\s+w\d+\s+(\d+)\s+([A-Za-z0-9_]+)\)\)\)"
        found = re.search(sle_pattern, expr)
        if found:
            bound, index, name = found.groups()
            data = {"bound": int(bound), "index": int(index), "name": name}
            value_with_condition = value_with_condition + [(data["index"], x) for x in range(data["bound"])]
        return value_with_condition


    def update_score(self, version_idx):
        version = self.versions[version_idx]
        for symbol, score_data in self.scores.items():
            if "arg" in symbol:
                for idx, data in self.scores[symbol].items():
                    if idx in self.valid_args[version].keys():
                        max_score = max(list(data.values()))
                        for val in data.keys():
                            if val in self.valid_args[version][idx].keys():
                                self.scores[symbol][idx][val] += (self.valid_args[version][idx][val] * max_score)


    def resample(self, testcases, iteration_dir, version_idx):
        testcases = sorted([str(tc) for tc in testcases])
        const_files = [f.replace(".ktest", ".const") for f in testcases if os.path.exists(f.replace('.ktest', ".const"))]

        tc_data = dict()        # {testcase : {symbol : (length, true_data, false_data)}}
        new_sampled = dict()    # {testcase : {symbol : newly sampled list}}
        ascii_range = list(range(128))
        sample_range = list(range(32, 127))
        
        for testcase in testcases:
            tc_data[testcase] = dict()
            tc_lens = self.extract_testcase_data(testcase)
            constraint_file = testcase.replace(".ktest", ".const")
            if os.path.exists(constraint_file):
                with open(constraint_file, "r") as f:
                    constraints = f.read().strip()
                    constraints = eval(constraints)
                for symbol in tc_lens.keys():
                    if symbol not in self.scores.keys():
                        self.scores[symbol] = {idx : {val : 1 for val in ascii_range} for idx in range(tc_lens[symbol])}
                    if tc_lens[symbol] > len(self.scores[symbol]):
                        for new_idx in range(len(self.scores[symbol]), tc_lens[symbol]):
                            self.scores[symbol][new_idx] = {val : 1 for val in ascii_range}
                    
                    true_data, false_data = list(), list()
                    obj_consts = [c for c in constraints if f"{symbol})" in c]
                    true_const = [c for c in obj_consts if "(Eq false" not in c]
                    negate_const = [c for c in obj_consts if "(Eq false" in c]
                    for const in true_const:
                        const_data = self.extract_query_data(const)
                        const_data = [(idx, val) for idx, val in const_data if val in ascii_range]
                        true_data = true_data + const_data
                        for idx, val in true_data:
                            self.scores[symbol][idx][val] += 1
                    for const in negate_const:
                        const_data = self.extract_query_data(const)
                        const_data = [(idx, val) for idx, val in const_data if val in ascii_range]
                        false_data = false_data + const_data
                        for idx, val in false_data:
                            self.scores[symbol][idx] = {v : score+1 for v, score in self.scores[symbol][idx].items()}
                            self.scores[symbol][idx][val] -= 1
                    tc_data[testcase][symbol] = (tc_lens[symbol], true_data, false_data)
        
        if rd.random() > self.prob_hint:
            self.update_score(version_idx)
            
        # new_sampled = {testcase : {symbol : newly sampled list}}
        for testcase, data in tc_data.items():
            new_sampled[testcase] = dict()
            for symbol, (length, fixed, forbidden) in data.items():
                new_sampled[testcase][symbol] = list()
                fix_data = {idx : val for (idx, val) in fixed}
                forbid_data = dict()
                for (idx, val) in forbidden:
                    if idx in forbid_data.keys():
                        forbid_data[idx].append(val)
                    else:
                        forbid_data[idx] = [val]
                for _ in range(self.sample_k):
                    new_arg = [0 for _ in range(length)]
                    for idx in range(len(new_arg)):
                        if idx in fix_data.keys():
                            new_arg[idx] = fix_data[idx]
                        else:
                            if idx in forbid_data.keys():
                                candidate = {val : score for val, score in self.scores[symbol][idx].items() if val not in forbid_data[idx]}
                            else:
                                candidate = {val : score for val, score in self.scores[symbol][idx].items()}
                            # candidate = {val : score for val, score in candidate.items() if val in sample_range}
                            sampled_val = rd.choices(list(candidate.keys()), weights=list(candidate.values()), k=1)[0]
                            new_arg[idx] = sampled_val
                            self.scores[symbol][idx] = {v : score+1 for v, score in self.scores[symbol][idx].items()}
                            self.scores[symbol][idx][sampled_val] -= 1
                    new_sampled[testcase][symbol].append(new_arg)       
        return new_sampled
