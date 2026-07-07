#!/bin/bash
#PBS -N pio_spike
#PBS -A NCGD0067
#PBS -j oe
#PBS -k eod
#PBS -q main
#PBS -l walltime=00:05:00
#PBS -l select=1:ncpus=4:mpiprocs=4

cd ${PBS_O_WORKDIR:-$(dirname "$0")}
module purge
module load ncarenv/25.10 intel/2025.2.1 ncarcompilers/1.1.0 cray-mpich/8.1.32 \
            hdf5/1.14.6 netcdf/4.9.3 parallelio/2.6.8
mpiexec -n 4 ./pio_spike
