
from argparse import ArgumentParser

import os
import sys

import subprocess as sp


def collect_err_files(directories):
    error_index = []
    for file in directories:
        dir_num = 0
        for i in list(os.scandir(file)):
            if 'version' in str(i):
                dir_num += 1
        
        os.chdir("./%s" % (file))
        for num in range(dir_num):
            err_files = []
            os.chdir("version-%d" % (num + 1))
            testcases = os.listdir("./")
            for tc in testcases:
                if ".err" in tc:
                    err_files.append(tc)
            
            for err_tc in err_files:
                error_index.append("%d-%s" % (num + 1, err_tc))
        
            os.chdir("../")

        os.chdir("../")
    return error_index


def replay_tcs(gcov_path, testcases, err_files, replay_bin):
    error_data = list()
    used_argument = list()

    for tc in testcases:
        cmd_list = [replay_bin, gcov_path, tc]
        cmd = " ".join(cmd_list)
        try:
            result = sp.run(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True, timeout=1)
            stdout_str = result.stdout.decode("utf-8", errors="ignore")
            stderr_str = result.stderr.decode("utf-8", errors="ignore")
            if stdout_str == '':
                text = stderr_str
            elif stderr_str == '':
                text = stdout_str
            
            text_list = [l.strip() for l in text.split("\n") if len(l.strip()) > 0]
            crash_lines = [l for l in text_list if ("EXIT STATUS:" in l) and ("CRASHED" in l)]
            if len(crash_lines) > 0:
                crash_signals = list()
                for crash_line in crash_lines:
                    idx_cr_start = crash_line.find('CRASHED')
                    idx_cr_end = crash_line.find('(')
                    crash_signal = crash_line[idx_cr_start:idx_cr_end - 1]
                    crash_signals.append(crash_signal)
                it = tc[:tc.rfind("/")]
                tc_idx = tc[tc.rfind("/") + 1:]
                tc_idx = tc_idx[:tc_idx.rfind(".")]
                errored_f = [f for f in err_files if f"{it}/{tc_idx}" in f]

                crashed_loc = "unknown"
                crashed_line = "unknown"
                if len(errored_f) > 0:
                    with open(errored_f[0], 'r') as err_f:
                        f_line = err_f.read()
                        f_list = f_line.split("\n")
                        for fn in f_list:
                            if "File:" in fn:
                                crashed_loc = fn
                            elif "Line:" in fn:
                                crashed_line = fn
                                crashed_loc = 'unknown'

                arg_lines = [l for l in text_list if "Arguments:" in l]
                if len(arg_lines) > 0:
                    idx_arg = arg_lines[0].find('"')
                    argument = arg_lines[0][idx_arg:]
                else:
                    argument = "unknown"
                
                if argument not in used_argument:
                    error_data.append((tc, argument, str(crash_signals), crashed_loc, crashed_line))
                    used_argument.append(argument)
        except sp.TimeoutExpired:
            print('[WARNING] ReuSE : KLEE exceeded the time budget. Iteration terminated.')
    return error_data


def report_bug(pgm, version, error_data, running_dir):
    report_path = f"{running_dir}/bug_report_{pgm}.txt"
    with open(report_path, "a") as f:
        f.write(f"=============== {pgm}-{version} ===============\n")
        for tc_path, argument, crashes, crashed_file, crashed_line in error_data:
            f.write(f"Testcase path      : {tc_path}\n")
            f.write(f"Used argument      : {argument}\n")
            f.write(f"Crashed signal     : {crashes}\n")
            f.write(f"File error occured : {crashed_file}\n")
            f.write(f"Line error occured : {crashed_line}\n")
            f.write("\n")
        f.write("\n")


def main(*argv):
    versions = {
        "bison" : ["3.6", "3.7", "3.8"],
        "csplit" : ["9.7", "9.8", "9.9"],
        "diff" : ["3.10", "3.11", "3.12"],
        "du" : ["9.7", "9.8", "9.9"],
        "find" : ["4.8.0", "4.9.0", "4.10.0"],
        "gawk" : ["5.3.0", "5.3.1", "5.3.2"],
        "grep" : ["3.10", "3.11", "3.12"],
        "ls" : ["9.7", "9.8", "9.9"],
        "m4" : ["1.4.18", "1.4.19", "1.4.20"],
        "make" : ["4.2", "4.3", "4.4"],
        "objcopy" : ["2.43", "2.44", "2.45"],
        "objdump" : ["2.43", "2.44", "2.45"],
        "patch":  ["2.6", "2.7", "2.8"],
        "sqlite3" : ["3.51.0", "3.51.1", "3.51.2"],
        "xorriso" : ["1.5.2", "1.5.4", "1.5.6"]
    }

    parser = ArgumentParser()
    parser.add_argument("output_dirs", nargs="+", help="Output directories to calculate branch coverage.")
    parser.add_argument("--program", required=True, help="Name of tested program.")

    args = parser.parse_args(argv)
    output_dirs = args.output_dirs
    program = args.program
    running_dir = os.getcwd()
    replay_bin = f"{running_dir}/../engines/klee/build/bin/klee-replay"

    if program in ["gawk", "make", "sqlite3"]:
        src = ""
    elif program in ["objcopy", "objdump"]:
        src = "binutils"
    elif program in ["find", "xorriso"]:
        src = program
    else:
        src = "src"

    versions = [ver.replace('.', "_") for ver in versions[program]]
    testcases = {ver : list() for ver in versions}
    err_files = {ver : list() for ver in versions}
    for output_dir in output_dirs:
        iterations = [f"{running_dir}/{output_dir}/{it}" for it in os.listdir(f"{running_dir}/{output_dir}") if it.startswith("iteration-")]
        for it in iterations:
            tcs = [f"{it}/{tc}" for tc in os.listdir(it) if tc.endswith(".ktest")]
            err_fs = [f"{it}/{tc}" for tc in os.listdir(it) if tc.endswith(".err")]
            it_version = it.split("-")[-2]
            testcases[it_version] = testcases[it_version] + tcs
            err_files[it_version] = err_files[it_version] + err_fs

    for ver, tcs in testcases.items():
        orig_ver = ver.replace('_', '.')
        err_fs = err_files[ver]
        print(f"[INFO] ReuSE : Analyzing program {program}-{orig_ver}")
        gcov_path = f"{running_dir}/../benchmarks/{program}/{orig_ver}/obj-gcov1/{src}/{program}"
        error_data = replay_tcs(gcov_path, tcs, err_fs, replay_bin)
        print(error_data)
        report_bug(program, ver, error_data, running_dir)
        print()
        
    print(f'[INFO] ReuSE : The detected bugs were saved in "{running_dir}/bug_report_{program}.txt" file.')


if __name__ == '__main__':
    main(*sys.argv[1:])
