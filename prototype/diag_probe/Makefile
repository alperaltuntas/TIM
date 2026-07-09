# Diag-tier probe tests.
#   parse_test / reduce_test / time_test: host-only, plain g++.
#   manager_test: needs MPI + ParallelIO; build on Derecho with
#     module load ncarenv/25.10 intel/2025.2.1 ncarcompilers/1.1.0 \
#                 cray-mpich/8.1.32 hdf5/1.14.6 netcdf/4.9.3 parallelio/2.6.8
#     make manager_test
CXX      ?= CC
PIO_ROOT ?= $(NCAR_ROOT_PARALLELIO)
TIM      := ../../tim/cpp

HOST_CXX ?= g++

host: parse_test reduce_test time_test

parse_test: parse_test.cpp $(TIM)/diag/tim_diag_config.cpp
	$(HOST_CXX) -std=c++17 -O2 -Wall -o $@ $^

reduce_test: reduce_test.cpp $(TIM)/diag/tim_diag_reduce.cpp
	$(HOST_CXX) -std=c++17 -O2 -Wall -o $@ $^

time_test: time_test.cpp $(TIM)/core/tim_time.cpp
	$(HOST_CXX) -std=c++17 -O2 -Wall -o $@ $^

MGR_SRCS := manager_test.cpp \
            $(TIM)/diag/tim_diag_manager.cpp \
            $(TIM)/diag/tim_diag_reduce.cpp \
            $(TIM)/diag/tim_diag_config.cpp \
            $(TIM)/core/tim_time.cpp \
            $(TIM)/io/tim_file.cpp \
            $(TIM)/io/tim_iosystem.cpp \
            $(TIM)/io/tim_decomp_cache.cpp \
            $(TIM)/io/tim_backend.cpp

manager_test: $(MGR_SRCS)
	$(CXX) --std=c++17 -O0 -g -I$(PIO_ROOT)/include -o $@ $(MGR_SRCS) \
	    -L$(PIO_ROOT)/lib -lpioc $(shell nc-config --libs)

EXT_SRCS := extfield_test.cpp \
            $(TIM)/io/tim_external_field.cpp \
            $(TIM)/core/tim_time.cpp \
            $(TIM)/io/tim_file.cpp \
            $(TIM)/io/tim_iosystem.cpp \
            $(TIM)/io/tim_decomp_cache.cpp \
            $(TIM)/io/tim_backend.cpp

extfield_test: $(EXT_SRCS)
	$(CXX) --std=c++17 -O0 -g -I$(PIO_ROOT)/include -o $@ $(EXT_SRCS) \
	    -L$(PIO_ROOT)/lib -lpioc $(shell nc-config --libs)

clean:
	rm -f parse_test reduce_test time_test manager_test diag_table_test
	rm -rf run_a run_b run_c run_d diag_state.nc
