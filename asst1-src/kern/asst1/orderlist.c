#include <types.h>
#include <lib.h>
#include <synch.h>
#include <test.h>
#include <thread.h>
#include "orderlist.h"


/* simple implementation of circle list */
/* always use inside lock */

struct orderlist *orderlist_create(){
        struct orderlist *ret = kmalloc(sizeof(struct orderlist));
        ret->order_end = 0;
        ret->order_start = 0;
        return ret;
}

void orderlist_destroy(struct orderlist *l) {
        kfree(l);
}

int order_full(struct orderlist *l) {
        if(l->order_end == ORDER_LIST_SIZE - 1) {
                /* list is empty */
                if(l->order_start == 0) {
                        return 1;
                }
                else {
                        return 0;
                }
        }
        else {
                if(l->order_end + 1 == l->order_start) {
                        return 1;
                }
                else {
                        return 0;
                }
        }
}

int order_empty(struct orderlist *l) {
        if(l->order_end == l->order_start) {
                return 1;
        }
        else {
                return 0;
        }
}

/* only used when list status checked, a little bit lazy */
void order_insert(struct orderlist *l, struct barorder *order) {
        l->list[l->order_end] = order;
        l->order_end++;
        if(l->order_end == ORDER_LIST_SIZE){
                l->order_end = 0;
        }
}

struct barorder *order_get(struct orderlist *l){
        struct barorder *ret = l->list[l->order_start];
        l->order_start++;
        if(l->order_start == ORDER_LIST_SIZE){
                l->order_start = 0;
        }
        return ret;
}