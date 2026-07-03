FROM ubuntu:22.04

ARG BASE_DIR=/root/main
ARG SOURCE_DIR=/root/main/
ARG DEBIAN_FRONTEND=noninteractive

# install requirements
RUN apt-get -y update
RUN apt-get -y install build-essential cmake curl file g++-multilib gcc-multilib git libcap-dev libgoogle-perftools-dev libncurses5-dev libsqlite3-dev libtcmalloc-minimal4 python3-pip unzip graphviz doxygen

# install python-3.10.12
RUN apt-get install -y wget build-essential checkinstall libreadline-dev libssl-dev tk-dev libgdbm-dev libc6-dev libbz2-dev libffi-dev zlib1g-dev
WORKDIR /root
RUN wget https://www.python.org/ftp/python/3.10.12/Python-3.10.12.tgz
RUN tar xzf Python-3.10.12.tgz
WORKDIR /root/Python-3.10.12
RUN ./configure --enable-optimizations
RUN make install


RUN pip3 install lit wllvm
RUN apt-get -y install python3-tabulate
RUN apt-get -y install clang-13 llvm-13 llvm-13-dev llvm-13-tools
RUN ln -s /usr/bin/clang-13 /usr/bin/clang
RUN ln -s /usr/bin/clang++-13 /usr/bin/clang++
RUN ln -s /usr/bin/llvm-config-13 /usr/bin/llvm-config
RUN ln -s /usr/bin/llvm-link-13 /usr/bin/llvm-link

WORKDIR /root

# Install ReuSE
WORKDIR ${BASE_DIR}
RUN git clone https://github.com/unknownicse27/reuse.git
WORKDIR ${BASE_DIR}/reuse
RUN python3 setup.py install

# Install STP solver
RUN apt-get -y install cmake bison flex libboost-all-dev python-is-python3 perl minisat
WORKDIR ${BASE_DIR}
RUN git clone https://github.com/stp/stp.git
WORKDIR ${BASE_DIR}/stp
RUN git checkout tags/2.3.3
RUN mkdir build
WORKDIR ${BASE_DIR}/stp/build
RUN cmake ..
RUN make
RUN make install


# Install Z3 solver
WORKDIR ${BASE_DIR}
RUN git clone https://github.com/Z3Prover/z3.git
WORKDIR ${BASE_DIR}/z3
RUN git checkout z3-4.8.15
RUN mkdir build
WORKDIR ${BASE_DIR}/z3/build
RUN cmake -DCMAKE_BUILD_TYPE=Release ..
RUN make -j2
RUN make install
RUN ldconfig

RUN echo "ulimit -s unlimited" >> /root/.bashrc


# install Klee-uclibc
WORKDIR ${BASE_DIR}/reuse
RUN git clone https://github.com/klee/klee-uclibc.git
WORKDIR ${BASE_DIR}/reuse/klee-uclibc
RUN chmod 777 -R *
RUN ./configure --make-llvm-lib
RUN make -j2


# Build ReuSE engine (based on KLEE-3.1)
WORKDIR ${BASE_DIR}/reuse
RUN chmod 777 -R *
RUN curl -OL https://github.com/google/googletest/archive/release-1.11.0.zip
RUN unzip release-1.11.0.zip
WORKDIR ${BASE_DIR}/reuse/klee
RUN echo "export LLVM_COMPILER=clang" >> /root/.bashrc
RUN echo "export WLLVM_COMPILER=clang" >> /root/.bashrc
RUN echo "KLEE_REPLAY_TIMEOUT=1" >> /root/.bashrc
RUN mkdir build
WORKDIR ${BASE_DIR}/reuse/klee/build
RUN apt-get -y install libzstd-dev
RUN cmake -DENABLE_SOLVER_STP=ON -DENABLE_SOLVER_Z3=ON -DENABLE_POSIX_RUNTIME=ON -DKLEE_UCLIBC_PATH=${BASE_DIR}/reuse/klee-uclibc -DENABLE_UNIT_TESTS=ON -DGTEST_SRC_DIR=${BASE_DIR}/reuse/googletest-release-1.11.0 ${BASE_DIR}/reuse/klee
RUN make
WORKDIR ${BASE_DIR}/reuse/klee
RUN env -i /bin/bash -c '(source testing-env.sh; env > test.env)'
RUN echo "export PATH=$PATH:/root/main/reuse/klee/build/bin" >> /root/.bashrc

ENV LLVM_COMPILER=clang
ENV WLLVM_COMPILER=clang

# Install Benchmarks (e.g. grep)
WORKDIR ${BASE_DIR}/reuse/benchmarks
RUN python3 build_benchmarks.py grep

# Initiating Starting Directory
WORKDIR ${BASE_DIR}/reuse/experiments
