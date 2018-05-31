#include <types.h>
#include <lib.h>
#include <synch.h>
#include <test.h>
#include <thread.h>

#include "bar.h"
#include "bar_driver.h"
#include "orderlist.h"


/*
 * **********************************************************************
 * YOU ARE FREE TO CHANGE THIS FILE BELOW THIS POINT AS YOU SEE FIT
 *
 */

/* Declare any globals you need here (e.g. locks, etc...) */
/* a circle list to store orders */
/* order_start point to list head */
/* order_end point to list end */
struct orderlist *order_queue;
/* a lock to lockup order_list */
struct lock *lock_order;
/* two cvs to control order flow */
struct cv *cv_customer;
struct cv *cv_server;
/* a series locks to lockup bottles with one idle lock 
 * so wecan simply use lock_bottles[eachwine] for eachwine */
struct lock *lock_bottles[NBOTTLES + 1];

/* NCUSTOMERS semaphore for all the orders */
struct semaphore *order_sem[NCUSTOMERS];
int sem_index;          //index always point to next available semaphore
/* a lock along with the semaphore list */
struct lock *lock_sem;
/*
 * **********************************************************************
 * FUNCTIONS EXECUTED BY CUSTOMER THREADS
 * **********************************************************************
 */

/*
 * order_drink()
 *
 * Takes one argument referring to the order to be filled. The
 * function makes the order available to staff threads and then blocks
 * until a bartender has filled the glass with the appropriate drinks.
 */

void order_drink(struct barorder *order)
{
        /* when start order, first initialize its semaphore */
        lock_acquire(lock_sem);
        /* grab a available semaphore */
        order->sem_order = order_sem[sem_index];
        sem_index++;
        lock_release(lock_sem);

        /* get the lock */
        lock_acquire(lock_order);
        /* if order list is full put it to sleep */
        while(order_full(order_queue)){
                cv_wait(cv_customer, lock_order);
        }
        /* insert order into orderlist */
        order_insert(order_queue,order);
        /* if order list is not empty, wake a server up */
        if(!order_empty(order_queue)){
                cv_signal(cv_server, lock_order);
        }
        lock_release(lock_order);

        /* wait to wake up signal */
        P(order->sem_order);
        /* return the semaphore */
        lock_acquire(lock_sem);
        sem_index--;
        order_sem[sem_index] = order->sem_order;
        lock_release(lock_sem);
}



/*
 * **********************************************************************
 * FUNCTIONS EXECUTED BY BARTENDER THREADS
 * **********************************************************************
 */

/*
 * take_order()
 *
 * This function waits for a new order to be submitted by
 * customers. When submitted, it returns a pointer to the order.
 *
 */

struct barorder *take_order(void)
{
        struct barorder *ret = NULL;
        /* test orderlist empty */
        /* get the lock */
        lock_acquire(lock_order);
        /* if order list is empty, put it to sleep */
        while(order_empty(order_queue)){
                cv_wait(cv_server, lock_order);
        }
        /* take order */
        ret = order_get(order_queue);
        /* if order list is not full, wake up a server */
        if(!order_full(order_queue)){
                cv_signal(cv_customer, lock_order);
        }
        lock_release(lock_order);
        return ret;
}


/*
 * fill_order()
 *
 * This function takes an order provided by take_order and fills the
 * order using the mix() function to mix the drink.
 *
 * NOTE: IT NEEDS TO ENSURE THAT MIX HAS EXCLUSIVE ACCESS TO THE
 * REQUIRED BOTTLES (AND, IDEALLY, ONLY THE BOTTLES) IT NEEDS TO USE TO
 * FILL THE ORDER.
 */

void fill_order(struct barorder *order)
{

        /* add any sync primitives you need to ensure mutual exclusion
           holds as described */
        /* lock all bottles */
        /* need to lock from BOTTLE1 to N to avoid deadlock */
        int bottles[NBOTTLES + 1] = {0};
        /* find out all bottles */
        for(int i = 0;i< DRINK_COMPLEXITY;i++){
                if(order->requested_bottles[i] != 0){
                        bottles[order->requested_bottles[i]] = 1;
                }
        }
        /* lock them up */
        for(int i= 1;i<=NBOTTLES;i++){
                if(bottles[i] == 1){
                        lock_acquire(lock_bottles[i]);
                }
        }

        mix(order);

        /* then release them one by one */
        for(int i = 1;i<=NBOTTLES;i++){
                if(bottles[i] == 1){
                        lock_release(lock_bottles[i]);
                }
        }
}


/*
 * serve_order()
 *
 * Takes a filled order and makes it available to (unblocks) the
 * waiting customer.
 */

void serve_order(struct barorder *order)
{
        /* order done */
        /* wake customer up */
        V(order->sem_order);
}



/*
 * **********************************************************************
 * INITIALISATION AND CLEANUP FUNCTIONS
 * **********************************************************************
 */


/*
 * bar_open()
 *
 * Perform any initialisation you need prior to opening the bar to
 * bartenders and customers. Typically, allocation and initialisation of
 * synch primitive and variable.
 */

void bar_open(void)
{
        order_queue = orderlist_create();
        if(order_queue == NULL) {
                panic("orderlist allocate wrong");
        }
        /* initialize locks */
        lock_order = lock_create("order lock");
        if(lock_order == NULL){
                panic("lock allocate wrong");
        }

        lock_sem = lock_create("sem lock");
        if(lock_sem == NULL) {
                panic("lock allocate wrong");
        }

        for (int i = 1; i <= NBOTTLES; i++)
        {
                lock_bottles[i] = lock_create("bottlelock");
                if(lock_bottles[i] == NULL){
                        panic("lock allocate wrong");
                }
        }
        /* intialize cvs */
        cv_customer = cv_create("cv_customer");
        if(cv_customer == NULL){
                panic("cv allocate wrong");
        }
        cv_server = cv_create("cv_server");
        if(cv_server == NULL){
                panic("cv allocate wrong");
        }

        /* initial all the semaphores */
        for(int i = 0;i < NCUSTOMERS; i++) {
                order_sem[i] = sem_create("sem order", 0);
                if(order_sem[i] == NULL) {
                        panic("semaphore allocate wrong");
                }
        }
}

/*
 * bar_close()
 *
 * Perform any cleanup after the bar has closed and everybody
 * has gone home.
 */

void bar_close(void)
{
        orderlist_destroy(order_queue);
        lock_destroy(lock_order);
        lock_destroy(lock_sem);
        for(int i = 1; i<= NBOTTLES; i++){
                lock_destroy(lock_bottles[i]);
        }
        cv_destroy(cv_server);
        cv_destroy(cv_customer);
        /* I believe when all the orders done, all the semaphore should be returned */
        /* So please dont close the bar when there is still order not served */
        for(int i = 0;i < NCUSTOMERS; i++) {
                sem_destroy(order_sem[i]);
        }
}

