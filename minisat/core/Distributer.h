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
#include <queue>
#include <mutex>
#include <memory>
#include <thread>
#include <iostream>
#include <functional>

// we need the solvertypes because it has everything - solver, lit etc
#include "minisat/core/SolverTypes.h"

namespace Minisat {

// because why the fuck is this not a predefined type?
using LitClause = vec<Lit>;

// THIS SHOULD BE SINGLETON CLASS BECAUSE NEVER TWO!!!
class Ring {
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
    Ring() {
        // initialize our connection
        MPI::Init();
        
        // and we set the rank and size
        _rank = MPI::COMM_WORLD.Get_rank();
        _size = MPI::COMM_WORLD.Get_size();
    }   

    // destructor for the ring
    virtual ~Ring() { MPI::Finalize(); }

    // our own tag
    size_t tag() { return _rank; }

    // our next neighbour tag
    size_t next() { return (tag() + 1) % _size; }
    
    // our previous neighbour tag
    size_t prev() { return (tag() - 1 + _size) % size; }
}

class Buffer {
public:
    // class to provide an interface for receiving conflict clauses
    class Receiver {
        // receive a clause
        virtual void receive(const std::shared_ptr<LitClause> &clause) = 0;
    }

private:
    // the receiver we use to tell the learned clauses
    Receiver *_receiver;

    // internal data type is vector
    std::shared_ptr<LitClause> _clause;

    // emit to the receiver
    void emit() {
        // pop the clause
        auto clause = std::move(_clause);
            
        // we emit to the receiver
        _receiver->receive(clause);
    }

public:
    // construct it
    Buffer(Receiver *receiver) : _receiver(receiver) {}

    // method to add to the buffer
    void add(const int32_t * data, size_t size) {
        // loop over all givens
        for (size_t i = 0; i < size; i++) {
            
            // the next data element
            int32_t el = data[i]; 
            
            // zero is termination, so we have one more number
            if (el == 0) emit();
            
            // not yet terminated, append
            else _clause->push(toLit(el));
        }
    } 
}

class Distributer : public Buffer::Receiver {
private:
    /** 
     *  We have the MPI ring
     */
    Ring _ring;

    /**
     *  Queue that we have to broadcast
     */
    std::queue<std::shared_ptr<LitClause>> _broadcast;

    /**
     *  Queue of stuff that still has to be picked up
     */
    std::queue<std::shared_ptr<LitClause>> _received;

    /**
     *  Mutex to protect the queue
     */
    std::mutex _mutex;

    /**
     *  The thread to run
     */
    std::thread _thread;

    /**
     *  Do we have to stop?
     */
    bool _stop = false;

    /**
     *  Partially received buffer
     */
    Buffer _buffer;

    /**
     *  Method that will run in the separate thread
     */
    void run() {
        // we keep going as long as we can
        while (!_stop) {
            
            // how much did we receive
            size_t received = 0;

            // we always try to receive in blocks of 1024
            int buffer[1024];

            // we first try to receive an open clause
            while (!yield || received != sizeof(buffer)) {
                // first of all, we try to receive now 
                MPI::Receive(buffer, sizeof(buffer), MPI::LONG, _ring.prev(), _ring.tag());

                // then we write it to the buffer
                _buffer.add(buffer, sizeof(buffer));
            }

            // empty cluase
            std::shared_ptr<LitClause> clause; 
            
            // we need a separate scope to lock the broadcast getting operation
            {
                // and then we broadcast a single clause
                // we are going to make a unique lock
                std::unique_lock<std::mutex> lock(_bmutex);

                // get the clause
                clause = _broadcast.front();
            
                // and pop it 
                _broadcast.pop();
            }

            // broadcast the obtained clause
            broadcast_clause(clause);
        }
    }

    /**
     *  Method that will broadcast a single clause (thread method, does the actual broadcast)
     */
    void broadcast_clause(const std::shared_ptr<LitClause> &clause) {

    }

    /**
     *  Method which obtains a clause from the buffer
     */
    void received(std::shared_ptr<LitClause> &clause) {
        // lock the receiving end
        std::unique_lock<std::mutex> lock(_rmutex);

        // and we push it to the received queue
        _received.push(clause);
    }

public:
    /**
     *  Initialize the distributer, which will start up a thread
     */
    Distributer() : _thread(std::bind(&Distributer::run, this)) {}

    /**
     *  On destruction, we finalize the MPI stuff.
     */
    virtual ~Distributer() { 
        // we will now stop
        _stop = true;

        // first off, we finish the thread
        _thread.join();
    }

    /**
     *  Broadcast a vector of literals
     */
    void broadcast(const LitClause &clause) {
        // first we copy (we make a shared pointer because its a lot faster and supports copying easily)
        auto copy = std::make_shared<vec<Lit>> copy;
        clause.copyTo(*copy);

        // then we lock
        std::unique_lock<std::mutex> lock(_mutex);

        // we make a direct copy to the queue
        _broadcast.push(copy);
    }

    /**
     *  Receive a conflict clause
     */
    std::shared_ptr<vec<Lit>> receive() {
        // we lock the queue
        std::unique_lock<std::mutex> lock(_recvmutex);

        // if there is nothing to receive, we're done
        if (_received.size() == 0) return nullptr;

        // otherwise, we're going to get the next one
        auto ptr = _queue.front();

        // we pop the element
        _queue.pop();

        // and we return the element
        return ptr;
    }
};

}