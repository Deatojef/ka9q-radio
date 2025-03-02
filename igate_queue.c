#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>


#include "igate.h"



// ------------------------------------------------------
// create_payload
//
// Allocate memory for a new payload structure and return that buffer
// Arguments are a pointer to the text packet and the size of that text.
// ------------------------------------------------------
struct payload_t *create_payload(char *aprstext, size_t n) {
    struct payload_t *buf = malloc(sizeof(struct payload_t));

    // if memory was successfullly allocated, then proceed to setup the payload structure
    if (buf) {

        // allocate memory to contain the text packet
        buf->aprs_text = calloc(n, sizeof(char));

        // only proceed if memory allocation was successful for the text packet
        if (buf->aprs_text) {

            // copy over the text from the supplied packet into the newly allocated buffer
            memcpy(buf->aprs_text, aprstext, n);

            // initialize payload items
            buf->heard_direct = false;
            buf->is_satellite = false;
            buf->frequency = 0;
            memset(buf->heard_from, 0, MAX_CALLSIGN_SIZE);
            memset(buf->callsign, 0, MAX_CALLSIGN_SIZE);
        }

        // Unable to allocate memory for the aprs_text portion of the payload
        // ...in this case, we then need to deallocate the packet structure as we can't create half a structure (so to speak...)
        else {
            free(buf);
            buf = NULL;
        }
    }

    return buf;
}


// ------------------------------------------------------
// create_node
//
// allocate memory for a new node structure and return that buffer
// ------------------------------------------------------
struct node_t *create_node(struct payload_t *P) {

    // allocate memory for a node structure
    struct node_t *buf = malloc(sizeof(struct node_t));

    // if memory was successful, then set to default the node structural elements
    if (buf) {

        // initialize pointers
        buf->next = NULL;
        buf->prev = NULL;

        // assign the payload pointer to the provided payload_t structure.
        buf->payload  = P;
    }

    return buf;
}


// ------------------------------------------------------
// create_queue
//
// allocate memory for a new queue structure and return that buffer
// ------------------------------------------------------
struct queue_t *create_queue() {

    // allocate memory for a queue structure
    struct node_t *buf = malloc(sizeof(struct queue_t));

    // if memory was successful, then set the queue to default
    if (buf) {

        // initialize pointers
        buf->start = NULL;
        buf->end = NULL;

        // initialize number of queue items to zero
        buf->n = 0;
    }

    return buf;
}

// ------------------------------------------------------
// destroy_node
//
// deallocate memory for a node...this is usually called from the 'dequeue' function as part of removing a node from the double linked list.
// ------------------------------------------------------
bool destroy_node(struct node_t *N) {

    // sanity check
    if (D == NULL)
        return false;

    // deallocate any memory used for the payload structure associated with this node
    destroy_payload(N->payload);

    // free the memory from this node
    free(N);

    return true;
}

// ------------------------------------------------------
// destroy_payload
//
// deallocate memory for a payload...this is usually called from the 'destroy_node' function as part of removing a payload from a node.
// ------------------------------------------------------
bool destroy_payload(struct payload_t *P) {

    // sanity check
    if (P == NULL)
        return false;

    // free any memory used to store the raw text packet
    if (P->packet)
        free(P->packet);

    // deallocate any memory used for the payload structure 
    free(P);

    return true;
}

// ------------------------------------------------------
// destroy_queue
//
// deallocate memory for a payload...this is usually called from the 'destroy_node' function as part of removing a payload from a node.
// ------------------------------------------------------
bool destroy_queue(struct queue_t *Q, mutex_t *queue_mutex) {
    
    // sanity check
    if (Q == NULL)
        return false;

    // wait until we acquire a lock on the queue
    pthread_mutex_lock(queue_mutex);

    // the first node in the list
    struct node_t *n = Q->start;

    // loop through each node in the queue
    while(n) {

        // the next node in the list
        struct node_t *next_one = n->next;

        // destroy this node
        destroy_node(n);

        // point to the next one in the list and run back through the loop
        n = next_one;
    }

    // reset start, end, and the node counter
    Q->start = NULL;
    Q->end = NULL;
    Q->n = 0;

    // unlock the queue
    pthread_mutex_unlock(queue_mutex);

    return true;
}


// ------------------------------------------------------
// enqueue
//
// Adds a 'node' the the provided queue using the provided mutex to lock/unlock against
// ------------------------------------------------------
int enqueue(struct queue_t *Q, struct node_t *new_entry, mutex_t *queue_mutex) {

    // set the new entry's next pointer to be NULL
    new_entry->next = NULL;

    // get a lock on the queue's mutex before we proceed to "change" the queue by adding this entry to it.
    pthread_mutex_lock(queue_mutex);

    // if there is an end node in the queue...
    if (Q->end) {

        // The last node in the queue now points to new_entry...as new_entry will become the new "last" node in the double linked list.
        Q->end->next = new_entry;

        // Set the previous pointer of new_entry to the current last node
        new_entry->prev = Q->end;

        // The end of the queue is now the new node.
        Q->end = new_entry;

    }
    // otherwise, we don't have any existing entries in the queue.  
    else {

        // This is the first entry in the queue, so all pointers point to new_entry.
        Q->end = new_netry;
        Q->start = new_entry;

        // this is the only node in the list, so there isn't a "next" node to point too.
        new_entry->prev = NULL;
    }

    // Increment the list counter
    Q->n++;

    // release our lock 
    pthread_mutex_unlock(queue_mutex);

    // return the number of entries in the queue
    return Q->n;
}


// ------------------------------------------------------
// dequeue
//
// removes a node from the provided queue using the provided mutex to lock/unlock
// ------------------------------------------------------
int dequeue(struct queue_t *Q, struct node_t *node, mutex_t *queue_mutex) {

    // the previous and next pointers from the node we want to remove
    struct node_t *before = node->prev;
    struct node_t *after = node->next;

    // acquire a lock on the queue as we're about to make changes (i.e. remove a node from the queue and adust pointers to match)
    pthread_mutex_lock(queue_mutex);

    // this node is in the middle of the linked list somewhere
    if (node->next && node->prev) {

        // have the pointers for the nodes before and after this target node, point to each other.
        before->next = after;
        after->prev = before;
    }

    // This is at the beginning of the list...and the list has more than just this node in it.
    else if (node->next && node->prev == NULL) {

        // The next node in the list now becomes the "beginning" node, so its prev pointer now points to NULL.
        after->prev = NULL;

        // Adjust the starting pointer for the queue
        Q->start = after;
    }

    // This is at the end of the list
    else if (node->next == NULL && node->prev) {

        // set the prior node's next pointer to NULL as it is now the end of the list
        before->next = NULL;

        // Adjust the end of the list
        Q->end = before;
    }

    // this node is the only node in the list
    else {

        // set the queue's pointers to NULL as this was the last node in the list
        Q->start = NULL;
        Q->end = NULL;
    }

    // delete this node
    destroy_node(node);

    // decrement tne node counter for the queue
    Q->n--;

    // unlock the queue as changes to the linked list are complete
    pthread_mutex_unlock(queue_mutex);

    // return the number of entries left in the queue
    return Q->n;

}





