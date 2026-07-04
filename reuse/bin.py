import argparse
import csv
import json
import os
import resource
import shutil
import sys
import time

import subprocess as sp

from pathlib import Path

from reuse.klee import KLEE, KLEERun
from reuse.filter import DiskFilter, CexFilter
from reuse.unsat_core import CoreExtractor
from reuse.resample import Resampler


def main(argv=None):
    if argv == None:
        argv = sys.argv[1:]

    parser = argparse.ArgumentParser()
    root_dir = f"{os.getcwd()}/.."

    # Execution settings
    executable = parser.add_argument_group('executable settings')
    executable.add_argument('--klee', default=f'{root_dir}/klee/build/bin/klee', type=str,
                            help='Path to "klee" executable (default=klee)')
    executable.add_argument('--klee-replay', default=f'{root_dir}/klee/build/bin/klee-replay', type=str,
                            help='Path to "klee-replay" executable (default=klee-replay)')
    executable.add_argument('--kleaver', default=f'{root_dir}/klee/build/bin/kleaver', type=str,
                            help='Path to "kleaver" executable (default=kleaver)')
    executable.add_argument('--ktest-tool', default=f'{root_dir}/klee/build/bin/ktest-tool', type=str,
                            help='Path to "ktest-tool" executable (default=ktest-tool)')
    executable.add_argument('--gcov', default='gcov', type=str,
                            help='Path to "gcov" executable (default=gcov)')

    # Hyperparameters
    hyperparameters = parser.add_argument_group('hyperparameters')
    hyperparameters.add_argument('--init-budget', default=43200, type=int, metavar='INT',
                                 help='Initial symbolic argument formats')
    hyperparameters.add_argument('--gcov-depth', default=1, type=int, metavar='INT',
                                 help='Depth from the obj-gcov directory to the directory where the gcov file was created')

    # Others
    parser.add_argument('-d', '--output-dir', default='ReuSE_TEST', type=str,
                        help='Directory where experiment results are saved (default=ReuSE_TEST)')
    parser.add_argument('--num-resample', default=5, type=int, metavar='INT',
                        help='Initial symbolic argument formats')
    parser.add_argument('--repetition', default=3, type=int,
                        help='Number of releases to test consecutively (list releases in klee.py)')
    parser.add_argument('--unsat-core', default=False, type=bool,
                        help='Decide whether to extract the UNSAT core')

    # Required arguments
    required = parser.add_argument_group('required arguments')
    required.add_argument('-t', '--budget', default=3600, type=int, metavar='INT',
                          help='Time budget for each release')
    required.add_argument('llvm_bc', nargs='?', default=None,
                          help='.bc file in obj-llvm directory')
    required.add_argument('gcov_obj', nargs='?', default=None,
                          help='executable file in obj-gcov# directory')
    args = parser.parse_args(argv)

    if args.llvm_bc is None or args.gcov_obj is None:
        parser.print_usage()
        print('[INFO] ReuSE : following parameters are required: llvm_bc, gcov_obj')
        sys.exit(1)

    output_dir = Path(args.output_dir)
    running_dir = str(os.getcwd())
    if args.gcov_obj[0] != "/":
        args.gcov_obj = f"{running_dir}/{args.gcov_obj}"
    if args.llvm_bc[0] != "/":
        args.llvm_bc = f"{running_dir}/{args.llvm_bc}"
    program = args.gcov_obj[args.gcov_obj.rfind("/") + 1:]

    original_path = f"{running_dir}/{args.output_dir}"
    cache_path = f"{original_path}/caches"
    if output_dir.exists():
        shutil.rmtree(str(output_dir))
        print(f'[WARNING] ReuSE : Existing output directory is deleted: {output_dir}')
    else:
        output_dir.mkdir(parents=True)
    os.makedirs(cache_path, exist_ok=True)
    coverage_csv = f"{original_path}/coverage.csv"
    print(f'[INFO] ReuSE : Coverage will be recorded at "{coverage_csv}" at every iteration.')

    # Start Execution
    symbolic_executor = KLEE(args.klee)
    runner = KLEERun(args.gcov_obj, original_path, args.budget, args.num_resample, args.klee_replay, args.gcov)
    diskfilter = DiskFilter(cache_path)
    cexfilter = CexFilter(cache_path)
    core_extractor = CoreExtractor(args.kleaver, cache_path)
    resampler = Resampler(args.ktest_tool, original_path, args.num_resample, symbolic_executor.versions[program], args.gcov_obj)

    runner.clear_gcov(args.gcov_depth)

    # Initialize Variables
    cov_data = dict()
    ver_cov_list = list()
    ver_iter_list = list()
    acc_testcases = list()
    total_coverage = set()
    i, j = 0, 0
    elapsed = 0
    num_iter = 1

    print(f'[INFO] ReuSE : All configuration loaded. Start testing.')
    
    while i < args.repetition:
        start = time.time()
        version = symbolic_executor.versions[program][i]
        iteration_dir = output_dir / f'iteration-{version.replace(".","_")}-{j}'
        iter_budget = runner.budget_handler(elapsed, args.init_budget, num_iter)
        ver_iter_list.append(f'iteration-{version.replace(".","_")}-{j}')

        # Run symbolic executor
        testcases = symbolic_executor.run(program, args.llvm_bc, iter_budget, args.output_dir, iteration_dir, cache_path, i)
        acc_testcases = acc_testcases + testcases
        new_sampled = resampler.resample(testcases, f"{running_dir}/{iteration_dir}", i)

        # Collect result
        iter_covered, new_sampled_covered = runner.evaluate(args.gcov_obj, acc_testcases, len(testcases), new_sampled, f"{running_dir}/{iteration_dir}", args.gcov_depth)
        total_coverage = total_coverage.union(iter_covered, new_sampled_covered)

        if args.unsat_core:
            filter_start = time.time()
            num_unsat = cexfilter.filtering(str(iteration_dir))

        elapsed += int(time.time() - start)

        print(f'[INFO] ReuSE : Version: {i} '
                f'Testing budget: {args.budget} '
                f'Time elapsed: {elapsed} '
                f'Iteration Coverage: {len(total_coverage)} ')

        with open(coverage_csv, 'a', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow([i, elapsed, len(total_coverage), len(iter_covered), len(new_sampled_covered)])
        
        acc_testcases = acc_testcases + [Path(f"{running_dir}/{iteration_dir}/{tc}") for tc in os.listdir(f"{running_dir}/{iteration_dir}") if tc.endswith(".ktest")]
        acc_testcases = list(set(acc_testcases))

        j += 1
        num_iter += 1

        if elapsed >= args.budget:
            if args.unsat_core:
                extract_core_start = time.time()
                kq_blocks, smt2_blocks = core_extractor.read_kqueries(ver_iter_list, original_path)
                if (i < args.repetition - 1):
                    core_extractor.extract_core(str(iteration_dir), kq_blocks, smt2_blocks)
            ver_cov_list.append(len(total_coverage))
            elapsed = 0
            total_coverage = set()
            ver_iter_list = list()
            start = time.time()
            num_iter = 1
            i += 1
            j = 0
            os.system(f"rm -f {original_path}/*/solver-queries.kquery")

        
    print(f'[INFO] ReuSE : Testing done.')
    for k in range(args.repetition):
        print(f'[INFO] ReuSE : {program}-{symbolic_executor.versions[program][k]} achieved {ver_cov_list[k]} coverage.')
    os.system(f"rm -f {original_path}/*/assembly.ll")
    os.system(f"rm -f {original_path}/*/final.bc")
    os.system(f"rm -f {original_path}/*/run.istats")
    os.system(f"rm -f {original_path}/*/*.kquery")
