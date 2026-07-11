/*
 * rbtree.h - Red-Black Tree for scheduler vruntime ordering
 *
 * Implements a standard left-leaning red-black tree keyed by uint64_t
 * (vruntime). Used by the scheduler to find the task with the smallest
 * virtual runtime in O(log n) time.
 *
 * Nodes are embedded in task_struct — no separate allocation needed.
 */
#ifndef RBTREE_H
#define RBTREE_H

#include <stdint.h>

#define RB_RED   0
#define RB_BLACK 1

struct rb_node {
    int             rb_color;   /* RB_RED or RB_BLACK */
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    uint64_t        key;        /* vruntime */
};

struct rb_root {
    struct rb_node *root;
};

void rb_init(struct rb_root *root);
void rb_insert(struct rb_root *root, struct rb_node *node);
void rb_erase(struct rb_root *root, struct rb_node *node);
struct rb_node *rb_find(struct rb_root *root, uint64_t key);
struct rb_node *rb_find_min(struct rb_root *root);
struct rb_node *rb_first(struct rb_root *root);
struct rb_node *rb_next(struct rb_node *node);

#endif /* RBTREE_H */