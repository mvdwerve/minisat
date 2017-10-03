#/usr/bin/bash
# we run it on 1, 2, 4, and 8 processes
mpirun -n 1 ./minisat -rnd-init $1 
mpirun -n 2 ./minisat -rnd-init $1
mpirun -n 4 ./minisat -rnd-init $1
mpirun -n 8 ./minisat -rnd-init $1