# ORBiS - Benchmarks

Using ORBiS, you can install three consecutive releases of 15 benchmarks

| Benchmark | version | Benchmark | version | Benchmark | version | Benchmark | version |
|:------:|:------------|:------:|:------------|:------:|:------------|:------:|:------------|
| bison  | 3.6~3.8     | find      | 4.8.0~4.10.0 | m4       | 1.4.18~1.4.20  | patch   | 2.6~2.8       |
| csplit | 9.7~9.9     | gawk      | 5.3.0~5.3.2  | make     | 4.2~4.4        | sqlite  | 3.51.0~3.51.2 |
| diff   | 3.10~3.12   | grep      | 3.10~3.12    | objcopy  | 2.43~2.45      | xorriso | 1.5.2~1.5.6   |
| du     | 9.7~9.9     | ls        | 9.7~9.9      | objdump  | 2.43~2.45      |
 

## Install Benchmarks
To install a benchmark to test, use the following command.
```
# Example for grep
/reuse/benchmarks$ python3 build_benchmarks.py grep
```

If you want to install multiple benchmarks, you can simply list them.
```
/reuse/benchmarks$ python3 build_benchmarks.py bison csplit diff du ...
```

And if you want to install all 15 benchmarks, just run the following command.
```
/reuse/benchmarks$ python3 build_benchmarks.py all
```

Finally, if you want to install multiple cores for a benchmark, use the '--n-core' option.
```
/reuse/benchmarks$ python3 build_benchmarks.py --n-core 5 grep
```
