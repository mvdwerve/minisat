/**
 *  Distributer.h
 *  
 *  Class to handle all message passing and receiving. This will take care
 *  of distributing the workload.
 *
 *  @author Michael van der Werve
 *  @author Yves van Montfort
 *  @copyright 2017
 */

#pragma once

#include <mpi.h>
#include <iostream>

namespace Minisat {

class Distributer {
private:
    /** 
     *  The rank
     */
    size_t _rank;

    /**
     *  The total size of the network
     */
    size_t _size;

public:
    /**
     *  Initialize the distributer. 
     */
    Distributer() {
        // initialize our connection
        MPI::Init();

        // and we set the rank and size
        _rank = MPI::COMM_WORLD.Get_rank();
        _size = MPI::COMM_WORLD.Get_size();
    }

    /**
     *  On destruction, we finalize the MPI stuff.
     */
    virtual ~Distributer() { MPI::Finalize(); }
};

}