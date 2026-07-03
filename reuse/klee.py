import os
import re
import random as rd
import subprocess as sp
from pathlib import Path


class GCov:
    def __init__(self, bin='gcov'):
        self.bin = bin


    def run(self, target, gcdas, folder_depth=1):
        if len(gcdas) == 0:
            return set()

        original_path = Path().absolute()
        target_dir = Path(target).parent
        gcdas = [gcda.absolute() for gcda in gcdas]
        os.chdir(str(target_dir))

        cmd = [str(self.bin), '-b', *list(map(str, gcdas))]
        cmd = ' '.join(cmd)
        _ = sp.run(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True, check=True)

        base = Path()
        for _ in range(folder_depth):
            if "gawk" in str(target) or "make" in str(target) or "sqlite" in str(target):
                pass
            else:
                base = base / '..'

        gcov_pattern = base / '**/*.gcov'
        gcovs = list(Path().glob(str(gcov_pattern)))

        covered = set()
        for gcov in gcovs:
            try:
                with gcov.open(encoding='UTF-8', errors='replace') as f:
                    file_name = f.readline().strip().split(':')[-1]
                    for i, line in enumerate(f):
                        if ('branch' in line) and ('never' not in line) and ('taken 0%' not in line) and (
                                ":" not in line) and ("returned 0% blocks executed 0%" not in line):
                            bid = f'{file_name} {i}'
                            covered.add(bid)
            except:
                pass

        os.chdir(str(original_path))
        return covered


class KLEE:
    def __init__(self, bin='klee'):
        self.bin = bin
        self.versions = {
            "bison" : ["3.6", "3.7", "3.8"],
            "csplit" : ["9.7", "9.8", "9.9"],
            "diff" : ["3.10", "3.11", "3.12"],
            "du" : ["9.7", "9.8", "9.9"],
            "expr" : ["9.7", "9.8", "9.9"],
            "find" : ["4.8.0", "4.9.0", "4.10.0"],
            "gawk" : ["5.3.0", "5.3.1", "5.3.2"],
            "ginstall" : ["9.7", "9.8", "9.9"],
            "grep" : ["3.10", "3.11", "3.12"],
            "ls" : ["9.7", "9.8", "9.9"],
            "m4" : ["1.4.18", "1.4.19", "1.4.20"],
            "make" : ["4.2", "4.3", "4.4"],
            "nano" : ["8.5", "8.6", "8.7"],
            "objcopy" : ["2.43", "2.44", "2.45"],
            "objdump" : ["2.43", "2.44", "2.45"],
            "patch":  ["2.6", "2.7", "2.8"],
            "ptx" : ["9.7", "9.8", "9.9"],
            "sed" : ["4.6", "4.7", "4.8"],
            "sqlite3" : ["3.51.0", "3.51.1", "3.51.2"],
            "xorriso" : ["1.5.2", "1.5.4", "1.5.6"]
        }
        # self.versions = {
        #     "bison" : ["2.7", "3.0", "3.1", "3.2", "3.3", "3.4", "3.5", "3.6", "3.7", "3.8"],
        #     "csplit" : ["9.0", "9.1", "9.2", "9.3", "9.4", "9.5", "9.6", "9.7", "9.8", "9.9"],
        #     "diff" : ["3.3", "3.4", "3.5", "3.6", "3.7", "3.8", "3.9", "3.10", "3.11", "3.12"],
        #     "du" : ["9.0", "9.1", "9.2", "9.3", "9.4", "9.5", "9.6", "9.7", "9.8", "9.9"],
        #     "gawk" : ["5.0.0", "5.0.1", "5.1.0", "5.1.1", "5.2.0", "5.2.1", "5.2.2", "5.3.0", "5.3.1", "5.3.2"],
        #     "ls" : ["9.0", "9.1", "9.2", "9.3", "9.4", "9.5", "9.6", "9.7", "9.8", "9.9"],
        #     "objcopy" : ["2.36", "2.37", "2.38", "2.39", "2.40", "2.41", "2.42", "2.43", "2.44", "2.45"],
        #     "objdump" : ["2.36", "2.37", "2.38", "2.39", "2.40", "2.41", "2.42", "2.43", "2.44", "2.45"],
        #     "sqlite3" : ["3.42.0", "3.43.0", "3.44.0", "3.45.0", "3.46.0", "3.47.0", "3.48.0", "3.49.0", "3.50.0", "3.51.0"]
        # }


    def run(self, program, target, budget, test_dir, dir_path, cache_path, iteration, **kwargs):
        if iteration > 0:
            target = target.replace(self.versions[program][0], self.versions[program][iteration])
        target = Path(target).absolute()
        original_path = Path().absolute()
        output_dir = Path(dir_path).absolute()
        dir_path = f"{str(original_path)}/{dir_path}"
        os.chdir(str(target.parent))
        option_args = [f"-output-dir={dir_path}", "-libc=uclibc", "-posix-runtime", "-external-calls=all", "-max-solver-time=30s", 
                        "-simplify-sym-indices", "-output-module", "-optimize", f"-max-time={budget}", "-only-output-states-covering-new", 
                        "-switch-type=internal", "-watchdog",
                        "-search=random-path -search=nurs:covnew", 
                        # "-use-query-log=solver:kquery",
                        "-solver-backend=z3"
                        ]
        sym_args = ["-sym-args 0 1 10", "-sym-args 0 2 2", "-sym-files 1 8", "-sym-stdin 8", "-sym-stdout"]
        cmd_list = [f"KLEE_CEXCACHINGSOLVER_DISK=1 KLEE_CACHE_DIR={cache_path}", self.bin]
        cmd_list = cmd_list + option_args + [str(target)] + sym_args
        cmd = " ".join(cmd_list)

        print(cmd)
        
        try:
            result = sp.run(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True, check=True, timeout=int(1.25*budget))
        except sp.TimeoutExpired:
            print('[WARNING] ReuSE : KLEE exceeded the time budget. Iteration terminated.')
        except sp.CalledProcessError as e:
            stderr = e.stderr.decode(errors='replace')
            lastline = stderr.strip().splitlines()[-1]
            if 'KLEE' in lastline and 'kill(9)' in lastline:
                print(f'[WARNING] ReuSE : KLEE process kill(9)ed. Failed to terminate nicely.')
            else:                
                print(f'[WARNING] ReuSE : Fail({e.returncode})ed to execute KLEE.')

        testcases = list(output_dir.glob('*.ktest'))
        testcases = [tc.absolute() for tc in testcases]
        os.chdir(str(original_path))

        return testcases


class KLEEReplay:
    def __init__(self, original_path, bin='klee-replay'):
        self.bin = bin
        self.original_path = original_path

    def run(self, target, testcases, folder_depth=1):
        target = Path(target).absolute()
        original_path = Path().absolute()
        for testcase in testcases:
            testcase = Path(testcase).absolute()
            os.chdir(str(target.parent))
            cmd = [str(self.bin), str(target), str(testcase)]
            cmd = ' '.join(cmd)
            process = sp.Popen(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True)
            try:
                _, stderr = process.communicate(timeout=0.1)
            except sp.TimeoutExpired:
                print(f'[WARNING] ReuSE : KLEE replay timeout: {testcase}')
            finally:
                process.kill()
        base = Path()
        for _ in range(folder_depth):
            base = base / '..'
        gcda_pattern = base / '**/*.gcda'
        gcdas = list(target.parent.glob(str(gcda_pattern)))
        gcdas = [gcda.absolute() for gcda in gcdas]

        os.chdir(str(original_path))
        return gcdas
    

    def run_resampled(self, target, new_sampled, sampled_k, iteration_dir, num_tc, folder_depth=1):
        target = Path(target).absolute()
        original_path = Path().absolute()
        new_sampled_path = list()
        n_new_sampled = num_tc + 1
        
        for testcase, data in new_sampled.items():
            symbols = list(data.keys())
            for i in range(sampled_k):
                symbol_list = [f"-o {symbol}={','.join(str(x) for x in data[symbol][i])}" for symbol in symbols]
                option_arg = " ".join(symbol_list)
                cmd = [str(self.bin), option_arg, "-O", f"{iteration_dir}/test{str(n_new_sampled).zfill(6)}.ktest", str(target), testcase]
                cmd = ' '.join(cmd)
                process = sp.Popen(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True)
                try:
                    _, stderr = process.communicate(timeout=0.1)
                except sp.TimeoutExpired:
                    print(f'[WARNING] ReuSE : KLEE replay timeout: {testcase}')
                finally:
                    process.kill()
                new_sampled_path.append(f"{iteration_dir}/test{str(n_new_sampled).zfill(6)}.ktest")
                n_new_sampled += 1
        base = Path()
        for _ in range(folder_depth):
            base = base / '..'
        gcda_pattern = base / '**/*.gcda'
        gcdas = list(target.parent.glob(str(gcda_pattern)))
        gcdas = [gcda.absolute() for gcda in gcdas]

        os.chdir(str(original_path))
        return gcdas, new_sampled_path


class KLEERun:
    def __init__(self, gcov_path, original_path, budget, sampled_k, klee_replay=None, gcov=None):
        if klee_replay is None:
            klee_replay = KLEEReplay()
        elif isinstance(klee_replay, str):
            klee_replay = KLEEReplay(original_path, klee_replay)
        self.klee_replay = klee_replay
        if gcov is None:
            gcov = GCov()
        elif isinstance(gcov, str):
            gcov = GCov(gcov)
        self.gcov = gcov
        self.gcov_path = gcov_path[:gcov_path.rfind('/')]
        self.budget = budget
        self.sampled_k = sampled_k


    def evaluate(self, target, testcases, num_tc, new_sampled, iteration_dir, folder_depth=1):
        base = Path(target).parent
        for _ in range(folder_depth):
            base = base / '..'

        cmd = ['rm', '-f', str(base / '**/*.gcda'), str(base / '**/*.gcov')]
        cmd = ' '.join(cmd)
        _ = sp.run(cmd, shell=True, check=True)
        gcdas = self.klee_replay.run(target, testcases, folder_depth=folder_depth)
        iter_covered = self.gcov.run(target, gcdas, folder_depth=folder_depth)

        cmd = ['rm', '-f', str(base / '**/*.gcda'), str(base / '**/*.gcov')]
        cmd = ' '.join(cmd)
        _ = sp.run(cmd, shell=True, check=True)
        gcdas, new_sampled_path = self.klee_replay.run_resampled(target, new_sampled, self.sampled_k, iteration_dir, num_tc, folder_depth=folder_depth)
        new_sampled_covered = self.gcov.run(target, gcdas, folder_depth=folder_depth)
        self.optimize(new_sampled_covered, iter_covered, new_sampled_path)
        return iter_covered, new_sampled_covered


    def budget_handler(self, elapsed, iteration_budget, num_iter):
        ## Take the best one to the Methodology
        if ((self.budget - elapsed) >= iteration_budget * num_iter):
            return iteration_budget * num_iter
        else:
            return self.budget - elapsed


    def find_all(self, path, ends):
        # Search for files in the current directory and all sub-directories
        found = []
        for root, dirs, files in os.walk(path):
            for file in files:
                if file.endswith(f'.{ends}'):
                    found.append(os.path.join(root, file))
        return found


    def clear_gcov(self, depth):
        # Initialize ".gcda" and ".gcov" files
        g_path = self.gcov_path
        for _ in range(depth):
            g_path = g_path[:g_path.rfind('/')]
        gcdas = self.find_all(g_path, "gcda")
        gcovs = self.find_all(g_path, "gcov")
        for gcda in gcdas:
            os.system(f"rm -f {gcda}")
        for gcov in gcovs:
            os.system(f"rm -f {gcov}")
    
    def optimize(self, new_sampled_covered, other_covered, new_sampled_path):
        if new_sampled_covered.union(other_covered) == other_covered:
            for path in new_sampled_path:
                if os.path.exists(path):
                    os.remove(path)
