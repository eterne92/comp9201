/* This file will contain your solution. Modify it as you wish. */
#include <types.h>
#include "producerconsumer_driver.h"
#include <synch.h>
#include <lib.h>

/* Declare any variables you need here to keep track of and
   synchronise your bounded. A sample declaration of a buffer is shown
   below. You can change this if you choose another implementation. */

static struct pc_data buffer[BUFFER_SIZE];
/* buffer_index point to the next empty position in buffer */
int buffer_index;

struct lock *lock_buffer;
struct cv *cv_full;
struct cv *cv_empty;

/* consumer_receive() is called by a consumer to request more data. It
   should block on a sync primitive if no data is available in your
   buffer. */

struct pc_data consumer_receive(void)
{
        struct pc_data thedata;

        /* get the lock */
        lock_acquire(lock_buffer);
        /* send to sleep if buffer is empty */
        while(buffer_index == 0){
                cv_wait(cv_empty, lock_buffer);
        }
        /* recieve data from buffer */
        buffer_index--;
        thedata.item1 = buffer[buffer_index].item1;
        thedata.item2 = buffer[buffer_index].item2;
        /* if buffer is not full wake up a producer */
        if(buffer_index < BUFFER_SIZE - 1){
                cv_signal(cv_full, lock_buffer);
        }
        lock_release(lock_buffer);

        return thedata;
}

/* procucer_send() is called by a producer to store data in your
   bounded buffer. */

void producer_send(struct pc_data item)
{
        /* get the lock */
        lock_acquire(lock_buffer);
        /* send to sleep if buffer is full */
        while(buffer_index == BUFFER_SIZE - 1){
                cv_wait(cv_full, lock_buffer);
        }
        /* put data into buffer */
        buffer[buffer_index] = item;   
        buffer_index++;
        /* if buffer not empty, wake up a consumer */
        if(buffer_index > 0){
                cv_signal(cv_empty, lock_buffer);
        }
        lock_release(lock_buffer);
}




/* Perform any initialisation (e.g. of global data) you need
   here. Note: You can panic if any allocation fails during setup */

void producerconsumer_startup(void)
{
        buffer_index = 0;
        /* initial lock and cvs */
        lock_buffer = lock_create("buffer");
        if(lock_buffer == NULL){
                panic("lock allocate wrong");
        }
        cv_full = cv_create("cv_full");
        if(cv_full == NULL){
                panic("cv allocate wrong");
        }
        cv_empty = cv_create("cv_empty");
        if(cv_empty == NULL){
                panic("cv allocate wrong");
        }
}

/* Perform any clean-up you need here */
void producerconsumer_shutdown(void)
{
        lock_destroy(lock_buffer);
        cv_destroy(cv_full);
        cv_destroy(cv_empty);
}

