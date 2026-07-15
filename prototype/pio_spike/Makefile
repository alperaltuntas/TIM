# PIO2 / TIM::IO spikes — build on Derecho:
#   module load ncarenv/25.10 intel/2025.2.1 ncarcompilers/1.1.0 cray-mpich/8.1.32 \
#               hdf5/1.14.6 netcdf/4.9.3 parallelio/2.6.8
#   make            # pio_spike (raw-PIO ergonomics)
#   make gates      # get_vara-bcast + blockDecomp round-trip + DOF identity
CXX      ?= CC
HOST_CXX ?= g++
PIO_ROOT ?= $(NCAR_ROOT_PARALLELIO)
TIM      := ../../tim/cpp

CXXFLAGS := --std=c++20 -O0 -g -I$(PIO_ROOT)/include
PIOLIBS  := -L$(PIO_ROOT)/lib -lpioc $(shell nc-config --libs)

IO_SRCS  := $(TIM)/io/tim_file.cpp $(TIM)/io/tim_iosystem.cpp \
            $(TIM)/io/tim_dofmap.cpp $(TIM)/io/tim_backend.cpp

pio_spike: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -L$(PIO_ROOT)/lib -lpioc

# Gate 1a: get_vara broadcast semantics (raw PIO, 4 ranks).
probe_getvar_bcast: probe_getvar_bcast.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -L$(PIO_ROOT)/lib -lpioc

# Gate 1b: blockDecomp / readReplicated round-trip through the TIM::IO stack.
blockdecomp_roundtrip: blockdecomp_roundtrip.cpp $(IO_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ blockdecomp_roundtrip.cpp $(IO_SRCS) $(PIOLIBS)

# Gate 1c: refactored DofMapCache DOFs vs the captured golden.
dof_capture: dof_capture.cpp
	$(HOST_CXX) -std=c++20 -O2 -o $@ $<
dof_identity: dof_identity.cpp $(TIM)/io/tim_dofmap.cpp $(TIM)/io/tim_backend.cpp
	$(CXX) $(CXXFLAGS) -o $@ dof_identity.cpp $(TIM)/io/tim_dofmap.cpp \
	    $(TIM)/io/tim_backend.cpp $(PIOLIBS)

gates: probe_getvar_bcast blockdecomp_roundtrip dof_identity

clean:
	rm -f pio_spike probe_getvar_bcast blockdecomp_roundtrip dof_identity \
	    dof_capture spike_*.nc
