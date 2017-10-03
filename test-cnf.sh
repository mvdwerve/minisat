#/usr/bin/bash
# we run it on 1, 2, 4, and 8 processes
tmpfile=`mktemp`
echo 'ELIMINATING unneeded clauses'
./minisat -no-solve -dimacs=$tmpfile $1 > /dev/null 
echo 'SINGLE'
mpirun -n 1 ./minisat $tmpfile 
echo 'DOUBLE'
mpirun -n 2 ./minisat -rnd-freq=0.5 $tmpfile
echo 'FOUR'
mpirun -n 4 ./minisat -rnd-freq=0.5 $tmpfile
echo 'EIGHT'
mpirun -n 8 ./minisat -rnd-freq=0.5 $tmpfile
