#include "tp.h"

struct tp_state
{

};

// waiting; new


// @ lock, the fiber adds itself to the waiting queue with the associated mutex
// and calls the scheduler

// the scheduler simply calls work items and switches to fibers which mutexes can be locked

// the scheduler queue must be thread specific