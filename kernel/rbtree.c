/*
 * rbtree.c - Red-Black Tree implementation for scheduler
 *
 * Standard red-black tree with:
 *   - Left/right rotation
 *   - Insert with color fixup
 *   - Erase with delete fixup
 *   - Min/max/find/successor operations
 *
 * All operations use kmalloc/kfree for any temporary allocations.
 * Nodes are embedded in task_struct — the caller manages node lifetime.
 */
#include "rbtree.h"
#include "include/log.h"
#include "mem.h"
#include <stddef.h>

/* ================================================================
 * Helper macros
 * ================================================================ */
#define rb_parent(r)  ((r)->parent)
#define rb_color(r)   ((r)->rb_color)
#define rb_is_red(r)  ((r) && (r)->rb_color == RB_RED)
#define rb_is_black(r) (!(r) || (r)->rb_color == RB_BLACK)

static inline void rb_set_red(struct rb_node *node)   { if (node) node->rb_color = RB_RED; }
static inline void rb_set_black(struct rb_node *node) { if (node) node->rb_color = RB_BLACK; }

/* ================================================================
 * Initialization
 * ================================================================ */
void rb_init(struct rb_root *root) {
    root->root = NULL;
}

/* ================================================================
 * Rotation operations
 * ================================================================ */

/*
 * Left rotation on node 'x':
 *
 *       x                y
 *      / \              / \
 *     a   y    --->    x   c
 *        / \          / \
 *       b   c        a   b
 */
static void rb_rotate_left(struct rb_root *root, struct rb_node *x) {
    struct rb_node *y = x->right;
    if (!y) return;

    /* Turn y's left subtree into x's right subtree */
    x->right = y->left;
    if (y->left) y->left->parent = x;

    /* Link x's parent to y */
    y->parent = x->parent;
    if (!x->parent) {
        root->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }

    /* Put x on y's left */
    y->left = x;
    x->parent = y;
}

/*
 * Right rotation on node 'y':
 *
 *       y                x
 *      / \              / \
 *     x   c    --->    a   y
 *    / \                  / \
 *   a   b                b   c
 */
static void rb_rotate_right(struct rb_root *root, struct rb_node *y) {
    struct rb_node *x = y->left;
    if (!x) return;

    /* Turn x's right subtree into y's left subtree */
    y->left = x->right;
    if (x->right) x->right->parent = y;

    /* Link y's parent to x */
    x->parent = y->parent;
    if (!y->parent) {
        root->root = x;
    } else if (y == y->parent->left) {
        y->parent->left = x;
    } else {
        y->parent->right = x;
    }

    /* Put y on x's right */
    x->right = y;
    y->parent = x;
}

/* ================================================================
 * Insertion
 * ================================================================ */

/*
 * rb_insert_fixup: Restore red-black tree invariants after insertion.
 * The new node is RED. Violations: red-red parent-child, or red root.
 */
static void rb_insert_fixup(struct rb_root *root, struct rb_node *node) {
    struct rb_node *parent, *grandparent, *uncle;

    while (rb_is_red(node->parent)) {
        parent = node->parent;
        grandparent = parent->parent;
        if (!grandparent) break;

        if (parent == grandparent->left) {
            uncle = grandparent->right;

            /* Case 1: Uncle is RED — recolor */
            if (rb_is_red(uncle)) {
                rb_set_black(parent);
                rb_set_black(uncle);
                rb_set_red(grandparent);
                node = grandparent;
                continue;
            }

            /* Case 2: node is right child — left rotate */
            if (node == parent->right) {
                rb_rotate_left(root, parent);
                node = parent;
                parent = node->parent;
            }

            /* Case 3: node is left child — right rotate + recolor */
            rb_set_black(parent);
            rb_set_red(grandparent);
            rb_rotate_right(root, grandparent);
        } else {
            /* Mirror: parent is right child of grandparent */
            uncle = grandparent->left;

            /* Case 1: Uncle is RED — recolor */
            if (rb_is_red(uncle)) {
                rb_set_black(parent);
                rb_set_black(uncle);
                rb_set_red(grandparent);
                node = grandparent;
                continue;
            }

            /* Case 2: node is left child — right rotate */
            if (node == parent->left) {
                rb_rotate_right(root, parent);
                node = parent;
                parent = node->parent;
            }

            /* Case 3: node is right child — left rotate + recolor */
            rb_set_black(parent);
            rb_set_red(grandparent);
            rb_rotate_left(root, grandparent);
        }
    }

    /* Root must always be black */
    rb_set_black(root->root);
}

/*
 * rb_insert: Insert a node into the red-black tree keyed by node->key.
 * The caller must ensure node->key is set before calling.
 */
void rb_insert(struct rb_root *root, struct rb_node *node) {
    if (!root || !node) return;

    struct rb_node *parent = NULL;
    struct rb_node **link = &root->root;

    /* Standard BST insert */
    while (*link) {
        parent = *link;
        if (node->key < parent->key) {
            link = &parent->left;
        } else {
            link = &parent->right;
        }
    }

    /* Initialize the new node */
    node->parent  = parent;
    node->left    = NULL;
    node->right   = NULL;
    node->rb_color = RB_RED;
    *link = node;

    /* Fix red-black tree violations */
    rb_insert_fixup(root, node);

    log_printf(LOG_LEVEL_DEBUG, "rbtree: inserted node key=%llu\n",
               (unsigned long long)node->key);
}

/* ================================================================
 * Deletion
 * ================================================================ */

/*
 * rb_transplant: Replace subtree rooted at 'u' with subtree rooted at 'v'.
 */
static void rb_transplant(struct rb_root *root, struct rb_node *u,
                          struct rb_node *v) {
    if (!u->parent) {
        root->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    if (v) v->parent = u->parent;
}

/*
 * rb_erase_fixup: Restore red-black tree invariants after deletion.
 * 'x' is the node that replaced the deleted node (may be NULL).
 * 'x_parent' is the parent of x (needed when x is NULL).
 * 'x_is_left' is 1 if x is (or was) a left child, 0 otherwise.
 */
static void rb_erase_fixup(struct rb_root *root, struct rb_node *x,
                           struct rb_node *x_parent, int x_is_left) {
    while (x != root->root && rb_is_black(x)) {
        struct rb_node *w;

        if (x_is_left) {
            w = x_parent->right;
            if (!w) break;  /* safety: malformed tree */

            /* Case 1: sibling is RED */
            if (rb_is_red(w)) {
                rb_set_black(w);
                rb_set_red(x_parent);
                rb_rotate_left(root, x_parent);
                w = x_parent->right;
                if (!w) break;
            }

            /* Case 2: sibling's children are both BLACK */
            if (rb_is_black(w->left) && rb_is_black(w->right)) {
                rb_set_red(w);
                x = x_parent;
                x_parent = x->parent;
                if (x_parent) x_is_left = (x == x_parent->left);
            } else {
                /* Case 3: sibling's right child is BLACK */
                if (rb_is_black(w->right)) {
                    rb_set_black(w->left);
                    rb_set_red(w);
                    rb_rotate_right(root, w);
                    w = x_parent->right;
                    if (!w) break;
                }

                /* Case 4: sibling's right child is RED */
                w->rb_color = x_parent->rb_color;
                rb_set_black(x_parent);
                if (w->right) rb_set_black(w->right);
                rb_rotate_left(root, x_parent);
                x = root->root;
                break;
            }
        } else {
            /* Mirror: x is right child */
            w = x_parent->left;
            if (!w) break;  /* safety: malformed tree */

            /* Case 1: sibling is RED */
            if (rb_is_red(w)) {
                rb_set_black(w);
                rb_set_red(x_parent);
                rb_rotate_right(root, x_parent);
                w = x_parent->left;
                if (!w) break;
            }

            /* Case 2: sibling's children are both BLACK */
            if (rb_is_black(w->left) && rb_is_black(w->right)) {
                rb_set_red(w);
                x = x_parent;
                x_parent = x->parent;
                if (x_parent) x_is_left = (x == x_parent->left);
            } else {
                /* Case 3: sibling's left child is BLACK */
                if (rb_is_black(w->left)) {
                    rb_set_black(w->right);
                    rb_set_red(w);
                    rb_rotate_left(root, w);
                    w = x_parent->left;
                    if (!w) break;
                }

                /* Case 4: sibling's left child is RED */
                w->rb_color = x_parent->rb_color;
                rb_set_black(x_parent);
                if (w->left) rb_set_black(w->left);
                rb_rotate_right(root, x_parent);
                x = root->root;
                break;
            }
        }
    }

    if (x) rb_set_black(x);
}

/*
 * rb_erase: Remove a node from the red-black tree.
 * The node pointer must be valid and present in the tree.
 */
void rb_erase(struct rb_root *root, struct rb_node *z) {
    if (!root || !z) return;

    struct rb_node *y = z;
    struct rb_node *x = NULL;
    struct rb_node *x_parent = NULL;
    int x_is_left = 0;
    int y_original_color = y->rb_color;

    if (!z->left) {
        /* No left child: replace with right child */
        x = z->right;
        x_parent = z->parent;
        if (x_parent) x_is_left = (z == x_parent->left);
        rb_transplant(root, z, z->right);
    } else if (!z->right) {
        /* No right child: replace with left child */
        x = z->left;
        x_parent = z->parent;
        if (x_parent) x_is_left = (z == x_parent->left);
        rb_transplant(root, z, z->left);
    } else {
        /* Both children exist: find successor */
        y = z->right;
        while (y->left) y = y->left;
        y_original_color = y->rb_color;

        x = y->right;
        if (y->parent == z) {
            x_parent = y;
            x_is_left = 0;  /* y is right child of z, x is right child of y */
            if (x) x->parent = y;
        } else {
            x_parent = y->parent;
            x_is_left = (y == x_parent->left);
            rb_transplant(root, y, y->right);
            y->right = z->right;
            if (y->right) y->right->parent = y;
        }

        rb_transplant(root, z, y);
        y->left = z->left;
        if (y->left) y->left->parent = y;
        y->rb_color = z->rb_color;
    }

    if (y_original_color == RB_BLACK) {
        rb_erase_fixup(root, x, x_parent, x_is_left);
    }

    log_printf(LOG_LEVEL_DEBUG, "rbtree: erased node key=%llu\n",
               (unsigned long long)z->key);
}

/* ================================================================
 * Search operations
 * ================================================================ */

/*
 * rb_find_min: Return the node with the smallest key in the tree.
 * Returns NULL if the tree is empty.
 */
struct rb_node *rb_find_min(struct rb_root *root) {
    if (!root || !root->root) return NULL;
    struct rb_node *node = root->root;
    while (node->left) node = node->left;
    return node;
}

/*
 * rb_first: Alias for rb_find_min — returns the first node in order.
 */
struct rb_node *rb_first(struct rb_root *root) {
    return rb_find_min(root);
}

/*
 * rb_find: Find a node with the given key.
 * Returns NULL if not found.
 */
struct rb_node *rb_find(struct rb_root *root, uint64_t key) {
    if (!root) return NULL;
    struct rb_node *node = root->root;
    while (node) {
        if (key < node->key) {
            node = node->left;
        } else if (key > node->key) {
            node = node->right;
        } else {
            return node;
        }
    }
    return NULL;
}

/*
 * rb_next: In-order successor of the given node.
 * Returns NULL if node is the last (maximum) in the tree.
 */
struct rb_node *rb_next(struct rb_node *node) {
    if (!node) return NULL;

    /* If right subtree exists, successor is the minimum of right subtree */
    if (node->right) {
        struct rb_node *next = node->right;
        while (next->left) next = next->left;
        return next;
    }

    /* Otherwise, go up until we are a left child */
    struct rb_node *parent = node->parent;
    while (parent && node == parent->right) {
        node = parent;
        parent = parent->parent;
    }
    return parent;
}