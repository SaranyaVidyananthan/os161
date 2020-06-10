#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>
#include <queue.h>
/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */


Direction volatile curDirection = north;
int volatile count = 0;

static struct lock * lock;
static struct cv * n;
static struct cv * e;
static struct cv * s;
static struct cv * w;
static struct queue * queue;


/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
	lock = lock_create("intersectionLock");
	if (lock == NULL) {
		panic("could not create intersection lock");
	}

	n = cv_create("north");
	if (n == NULL) {
		panic("could not create north cv");
	}

	e = cv_create("east");
        if (e == NULL) {
                panic("could not create east cv");
        }

	s = cv_create("south");
        if (s == NULL) {
                panic("could not create south cv");
        }

	w = cv_create("west");
        if (w == NULL) {
                panic("could not create west cv");
        }
	
	queue = q_create(1);
	if (queue == NULL) {
		panic("could not create queue");
	}

	return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  KASSERT(lock != NULL);
  KASSERT(n != NULL);
  KASSERT(e != NULL);
  KASSERT(s != NULL);
  KASSERT(w != NULL);

  lock_destroy(lock);
  cv_destroy(n);
  cv_destroy(e);
  cv_destroy(s);
  cv_destroy(w);
  q_destroy(queue);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  (void)destination;

  lock_acquire(lock);

  if (count != 0 && curDirection != origin) {

      Direction * new_direction = (Direction *)kmalloc(sizeof(Direction));
      *new_direction = origin;
      q_addtail(queue, (void *)new_direction);

      while(count != 0 && curDirection != origin) {
	 switch (origin) {
	    case north:
		    cv_wait(n, lock);
		    break;
	    case east:
		    cv_wait(e, lock);
		    break;
	    case south:
		    cv_wait(s, lock);
		    break;
	    case west:
		    cv_wait(w, lock);
		    break;
	 }
     }
      q_remhead(queue);
  }
  curDirection = origin;
  count = count + 1;

  lock_release(lock);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{

  (void)destination;
  (void)origin;

  lock_acquire(lock);
  count = count - 1;

  /*if (count != 0) {
      switch (origin) {
            case north:
                    cv_signal(n, lock);
                    break;
            case east:
                    cv_signal(e, lock);
                    break;
            case south:
                    cv_signal(s, lock);
                    break;
            case west:
                    cv_signal(w, lock);
                    break;
      }
  }*/
  if (count == 0 && !q_empty(queue)) {
	  Direction * nextDirection = (Direction *)q_peek(queue);
	  curDirection = *nextDirection;
	  switch (curDirection) {
		  case north:
			  cv_signal(n, lock);
                          break;
		  case east:
                          cv_signal(e, lock);
                          break;
                  case south:
                          cv_signal(s, lock);
                          break;
                  case west:
                          cv_signal(w, lock);
                          break;
	 }
  }
  lock_release(lock);
}

