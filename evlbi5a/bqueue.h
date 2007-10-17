// efficient producer/consumer queue for threads
#ifndef EVLBI5A_QUEUE_H
#define EVLBI5A_QUEUE_H

#include <queue>
#include <pthread.h>

struct block {
    // empty block: iov_len == 0 and iov_base == 0
    block();

    // initialized block: point at sz bytes starting from
    // base
    block(void* base, size_t sz);

    // convenience function that return true iff the
    // block is empty by what we take to mean empty,
    // ie iov_base==0 AND iov_len==0
    bool empty( void ) const;

    void*   iov_base;
    size_t  iov_len;
};


class bqueue {
    public:
        typedef std::queue<block> queue_type;

        // create a disabled queue of capacity 0.
        // Your threads will not block on push() or pop() 
        // but will not be able to do so anyway.
        // Call 'enable()' with a capacity>0 for 
        // usable queue
        bqueue();

        // create a (possibly) fully enabled
        // 'cap'. You *can* pass '0' as capacity but don't
        // assume much will happen...
        bqueue( const queue_type::size_type cap );
  
        // disable the queue: this means that all threads
        // waiting to push or pop will be signalled and
        // will return values to their callers indicating
        // that the queue was disabled:
        // push(): will return false [if not cancelled
        //         it will always return true as it will
        //         blocking wait until it *CAN* push]
        // pop():  return an empty block [both len&&base 0]
        //         This assumes the producer never inserts
        //         an empty block...
        void disable( void );

        // enable [and possibly resize] the queue.
        // After calling this, push'ing and pop'ing
        // is enabled(again) - so calls will, if
        // necessary, block until they *can* or the
        // queue is disabled(again).
        // If you pass 'newcap' as '0' (default) the
        // queue is just enabled and not resized.
        void enable( const queue_type::size_type newcap=0 );


        // push(): only returns false is queue is disabled.
        //         otherwise it waits indefinitely until
        //         the datum *can* be pushed (or queue is
        //         disabled).
        //         Note: a *copy* of b is pushed on the queue
        bool push( const block& b );

        // pop(): returns an empty block if the queue is
        //        disabled [empty: block.iov_base==0 AND block.iov_len==0;
        //        again, this assumes the producer will never push() such
        //        a block].
        //        If queue not disabled it will wait indefinitely for
        //        someone to push() a block [or disable the queue]
        //        Note: a *copy* of '.front()' is returned
        block pop( void );

        // disables the queue
        ~bqueue();

    private:
        bool                   enabled;
        queue_type             queue;
        unsigned int           numRegistered;
        pthread_cond_t         condition;
        pthread_mutex_t        mutex;
        queue_type::size_type  capacity;

        // do not support copy/assignment
        bqueue( const bqueue& );
        const bqueue& operator=( const bqueue& );
};



#endif
