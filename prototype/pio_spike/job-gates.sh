#!/bin/bash
#PBS -N tim_io_gates
#PBS -A NCGD0067
#PBS -j oe
#PBS -k eod
#PBS -q main
#PBS -l walltime=00:08:00
#PBS -l select=1:ncpus=4:mpiprocs=4

cd ${PBS_O_WORKDIR:-$(dirname "$0")}
export TMPDIR=${SCRATCH}/${USER}/temp && mkdir -p $TMPDIR
module purge
module load ncarenv/25.10 gcc/14.3.0 ncarcompilers/1.1.0 cray-mpich/8.1.32 \
            hdf5/1.14.6 netcdf/4.9.3 parallelio/2.6.8

echo "########## GATE 1a: get_vara broadcast ##########"
mpiexec -n 4 ./probe_getvar_bcast
echo "########## GATE 1b: blockDecomp round-trip ##########"
mpiexec -n 4 ./blockdecomp_roundtrip
echo "########## GATE 3: ExternalField (small + large branch) ##########"
cd ../diag_probe
mpiexec -n 1 ./extfield_test
echo "########## DONE ##########"
