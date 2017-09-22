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
    int32_t _rank;
     
    /**
     *  The total size of the network
     */
    int32_t _size;
public:
    Ring() {
        // get the rank and size
        MPI_Comm_rank(MPI_COMM_WORLD, &_rank);
        MPI_Comm_size(MPI_COMM_WORLD, &_size);
    }   

    // destructor for the ring
    virtual ~Ring() { MPI_Finalize(); }

    // our own tag
    size_t tag() const { return _rank; }

    // our next neighbour tag
    size_t next() const { return (tag() + 1) % _size; }
    
    // our previous neighbour tag
    size_t prev() const { return (tag() - 1 + _size) % _size; }
};

class Buffer {
public:
    // class to provide an interface for receiving conflict clauses
    class Receiver {
    public:
        // receive a clause
        virtual void receive(int32_t source, const std::shared_ptr<LitClause> &clause) = 0;
    };

private:
    // the receiver we use to tell the learned clauses
    Receiver *_receiver;

    // internal data type is vector
    std::shared_ptr<LitClause> _clause;

    // the source
    int32_t _source = -1;

    // emit to the receiver
    void emit() {
        // pop the clause
        auto clause = _clause;

        // the clause is now a new (empty) one
        _clause = std::make_shared<LitClause>();
            
        // the source is now -1 (unset it on emit)
        _source = -1;

        std::cout << "doing an emit " << std::endl;

        // we emit to the receiver
        _receiver->receive(_source, clause);
    }

public:
    // construct it
    Buffer(Receiver *receiver) : _receiver(receiver), _clause(std::make_shared<LitClause>()) {}

    // method to add to the buffer
    void add(const int32_t * data, size_t size) {
        // loop over all givens
        for (size_t i = 0; i < size; i++) {  
            // the next data element
            int32_t el = data[i]; 
            
            // if there are no elements yet, this is the source
            if (_source == -1) _source = el;

            // zero is termination, so we have one more number
            else if (el == 0) emit();
            
            // not yet terminated, append
            else _clause->push(toLit(el));
        }
    } 
};

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
     *  Mutex to protect the broadcast queue
     */
    std::mutex _bmutex;

    /**
     *  Mutex to protect the receive queue
     */
    std::mutex _rmutex;

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
            
            // we are going to receive some bytes first
            receive_bytes();

            // empty cluase
            std::shared_ptr<LitClause> clause; 

            // we need a separate scope to lock the broadcast getting operation
            {
                // and then we broadcast a single clause
                // we are going to make a unique lock
                std::unique_lock<std::mutex> lock(_bmutex);

                // just keep going with the receive if there is nothing to send
                if (_broadcast.empty()) continue;

                // get the clause
                clause = _broadcast.front();
            
                // and pop it 
                _broadcast.pop();
            }

            // broadcast the obtained clause
            broadcast_clause(_ring.tag(), clause);
        }
    }

    // receive a set of bytes
    void receive_bytes() {
        // make sure we're not receiving from ourself (no use only wastes time)
        if (_ring.next() == _ring.tag()) return;
        
        // status to receive
        MPI_Status status;

        // whether or not available
        int available, received;

        // we probe
        auto result = MPI_Iprobe(_ring.prev(), _ring.tag(), MPI_COMM_WORLD, &available, &status); 

        // if there is no message available, we skip for now
        if (!available) return;
        
        // we always try to receive in blocks of 1024
        int buffer[1024];
        
        // first of all, we try to receive now 
        MPI_Recv(buffer, sizeof(buffer), MPI_INT, _ring.prev(), 0, MPI_COMM_WORLD, &status);
        MPI_Get_count(&status, MPI_INT, &received);

        // we write it to the buffer
        _buffer.add(buffer, received);
    }

    /**
     *  Method that will broadcast a single clause (thread method, does the actual broadcast)
     */
    void broadcast_clause(int32_t source, const std::shared_ptr<LitClause> &clause) {
        // make sure we're not broadcasting to ourself (no use only wastes time)
        if (_ring.next() == _ring.tag()) return;

        // we're done if the source is our next-door neighbour
        if (source == _ring.next()) return;
                
        // we need to convert it to a native datatype
        std::vector<int32_t> native;

        // reserve the correct size for efficiency
        native.reserve(clause->size() + 2);

        // we add our own number to it (so that we see it)
        native.push_back(source);

        // get it as a nice reference
        auto &cls = *clause;

        // loop over the clause
        for (int i = 0; i < clause->size(); i++) native.push_back(toInt(cls[i]));

        // we append the zero (end)
        native.push_back(0);

        // and finally broadcast it
        MPI_Send(native.data(), native.size(), MPI_INT, _ring.next(), 0, MPI_COMM_WORLD);
    }

    /**
     *  Method which obtains a clause from the buffer
     */
    virtual void receive(int32_t source, const std::shared_ptr<LitClause> &clause) {
        // scope for mutexed code
        {
            // lock the receiving end
            std::unique_lock<std::mutex> lock(_rmutex);
        
            // and we push it to the received queue
            _received.push(clause);
        }

        // we're going to broadcast this clause to the next door neighbour immediately, to propagate
        broadcast_clause(source, clause);
    }

public:
    /**
     *  Initialize the distributer, which will start up a thread
     */
    Distributer() : _thread(std::bind(&Distributer::run, this)), _buffer(this) {}

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
        auto copy = std::make_shared<LitClause>();
        clause.copyTo(*copy);

        // then we lock
        std::unique_lock<std::mutex> lock(_bmutex);

        // we make a direct copy to the queue
        _broadcast.push(copy);
    }

    /**
     *  Receive a conflict clause
     */
    std::shared_ptr<LitClause> receive() {
        // we lock the queue
        std::unique_lock<std::mutex> lock(_rmutex);

        // if there is nothing to receive, we're done
        if (_received.size() == 0) return nullptr;

        // otherwise, we're going to get the next one
        auto ptr = _received.front();

        // we pop the element
        _received.pop();

        // and we return the element
        return ptr;
    }

    // get a seed
    int seed() const { return _ring.tag(); }
}; 

}