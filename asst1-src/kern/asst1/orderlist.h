
#ifndef ORDERLIST_H
#define ORDERLIST_H

/* some very simple implementation of a circle array */

#include "bar.h"
#define ORDER_LIST_SIZE 10
struct orderlist {
    struct barorder *list[ORDER_LIST_SIZE];
    int order_start;
    int order_end;
};

struct orderlist *orderlist_create(void);
void orderlist_destroy(struct orderlist *);
int order_empty(struct orderlist *);
int order_full(struct orderlist *);
void order_insert(struct orderlist *, struct barorder *order);
struct barorder *order_get(struct orderlist *);

#endif