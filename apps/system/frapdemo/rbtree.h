#pragma once
/* 1. 引入标准库，解决 size_t 未定义问题 */
#include <stddef.h> 
#include <stdint.h>
// #include <compl.h>
// #include <libapfsds.h>
/* XXX:user defines */
#define RB_LEFT 0
#define RB_RIGHT 1
#ifndef NULL
#define NULL 0
#endif
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)
#define WRITE_ONCE(d, s) d = s
#define READ_ONCE(s) s
typedef unsigned int bool_t;
/* 手动定义缺失的 word_t */
typedef unsigned long word_t;
// #define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE *)0)->MEMBER)
/* 3. [关键修复] 只有当 offsetof 未定义时才定义它 */
#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define container_of(ptr, type, member) ({                                \
        void *__mptr = (void *)(ptr);                                        \
        ((type *)(__mptr - offsetof(type, member))); })
/* 3. 补充缺失的内存屏障宏 cmb() */
#ifndef cmb
#define cmb() __asm__ __volatile__("" ::: "memory")
#endif

/* 5. 定义 unused 属性宏，用于消除未使用函数的警告 */
#ifndef __unused
#define __unused __attribute__((unused))
#endif
/* XXX:user defines end */

/* rb node */
struct rb_node
{
        unsigned long __rb_parent_color;
        struct rb_node *rb_child[2];
        //        struct rb_node *rb_right;
        //        struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));
/* The alignment might seem pointless, but allegedly CRIS needs it */

struct rb_root
{
        struct rb_node *rb_node;
        word_t count;
};

/*
 * Leftmost-cached rbtrees.
 *
 * We do not cache the rightmost node based on footprint
 * size vs number of potential users that could benefit
 * from O(1) rb_last(). Just not worth it, users that want
 * this feature can always implement the logic explicitly.
 * Furthermore, users that want to cache both pointers may
 * find it a bit asymmetric, but that's ok.
 */
struct rb_root_cached
{
        struct rb_root rb_root;
        struct rb_node *rb_leftmost;
};

#define rb_parent(r) ((struct rb_node *)((r)->__rb_parent_color & ~3))

#define RB_ROOT \
        (struct rb_root) { NULL, }
#define RB_ROOT_CACHED                                  \
        (struct rb_root_cached) { {                 \
                                                                  NULL, \
                                                          },                \
                                                          NULL }
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define rb_entry_safe(ptr, type, member)                                  \
        ({                                                                                                        \
                typeof(ptr) ____ptr = (ptr);                                          \
                ____ptr ? rb_entry(____ptr, type, member) : NULL; \
        })
/**
 * rbtree_postorder_for_each_entry_safe - iterate in post-order over rb_root of
 * given type allowing the backing memory of @pos to be invalidated
 *
 * @pos:        the 'type *' to use as a loop cursor.
 * @n:                another 'type *' to use as temporary storage
 * @root:        'rb_root *' of the rbtree.
 * @field:        the name of the rb_node field within 'type'.
 *
 * rbtree_postorder_for_each_entry_safe() provides a similar guarantee as
 * list_for_each_entry_safe() and allows the iteration to continue independent
 * of changes to @pos by the body of the loop.
 *
 * Note, however, that it cannot handle other modifications that re-order the
 * rbtree it is iterating over. This includes calling rb_erase() on @pos, as
 * rb_erase() may rebalance the tree, causing us to miss some nodes.
 */
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field)                        \
        for (pos = rb_entry_safe(rb_first_postorder(root), typeof(*pos), field); \
                 pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field), \
                        typeof(*pos), field); 1; });                                                                                                           \
                 pos = n)

#define RB_EMPTY_ROOT(root) (READ_ONCE((root)->rb_node) == NULL)

/* 'empty' nodes are nodes that are known not to be inserted in an rbtree */
#define RB_EMPTY_NODE(node) \
        ((node)->__rb_parent_color == (unsigned long)(node))
#define RB_CLEAR_NODE(node) \
        ((node)->__rb_parent_color = (unsigned long)(node))

extern void rb_insert_color(struct rb_node *, struct rb_root *);
extern void rb_erase(struct rb_node *, struct rb_root *);

/* Find logical next and previous nodes in a tree */
extern struct rb_node *rb_next(const struct rb_node *);
extern struct rb_node *rb_prev(const struct rb_node *);
extern struct rb_node *rb_first(const struct rb_root *);
extern struct rb_node *rb_last(const struct rb_root *);

extern void rb_insert_color_cached(struct rb_node *,
                                                                   struct rb_root_cached *, bool_t);
extern void rb_erase_cached(struct rb_node *node, struct rb_root_cached *);
/* Same as rb_first(), but O(1) */
#define rb_first_cached(root) (root)->rb_leftmost

/* Postorder iteration - always visit the parent after its children */
extern struct rb_node *rb_first_postorder(const struct rb_root *);
extern struct rb_node *rb_next_postorder(const struct rb_node *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
extern void rb_replace_node(struct rb_node *victim, struct rb_node *new,
                                                        struct rb_root *root);
extern void rb_replace_node_cached(struct rb_node *victim, struct rb_node *new,
                                                                   struct rb_root_cached *root);

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                                                struct rb_node **rb_link)
{
        node->__rb_parent_color = (unsigned long)parent;
        node->rb_child[RB_LEFT] = node->rb_child[RB_RIGHT] = NULL;

        *rb_link = node;
}

/*
 * Please note - only struct rb_augment_callbacks and the prototypes for
 * rb_insert_augmented() and rb_erase_augmented() are intended to be public.
 * The rest are implementation details you are not expected to depend on.
 *
 * See Documentation/rbtree.txt for documentation and samples.
 */

struct rb_augment_callbacks
{
        void (*propagate)(struct rb_node *node, struct rb_node *stop);
        void (*copy)(struct rb_node *old, struct rb_node *new);
        void (*rotate)(struct rb_node *old, struct rb_node *new);
};

extern void __rb_insert_augmented(struct rb_node *node,
                                                                  struct rb_root *root,
                                                                  bool_t newleft, struct rb_node **leftmost,
                                                                  void (*augment_rotate)(struct rb_node *old, struct rb_node *new));
/*
 * Fixup the rbtree and update the augmented information when rebalancing.
 *
 * On insertion, the user must update the augmented information on the path
 * leading to the inserted node, then call rb_link_node() as usual and
 * rb_insert_augmented() instead of the usual rb_insert_color() call.
 * If rb_insert_augmented() rebalances the rbtree, it will callback into
 * a user provided function to update the augmented information on the
 * affected subtrees.
 */
static inline void
rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                                        const struct rb_augment_callbacks *augment)
{
        __rb_insert_augmented(node, root, false, NULL, augment->rotate);
}

static inline void
rb_insert_augmented_cached(struct rb_node *node,
                                                   struct rb_root_cached *root, bool_t newleft,
                                                   const struct rb_augment_callbacks *augment)
{
        __rb_insert_augmented(node, &root->rb_root,
                                                  newleft, &root->rb_leftmost, augment->rotate);
}

#define RB_RED 0
#define RB_BLACK 1

#define __rb_parent(pc) ((struct rb_node *)(pc & ~3))

#define __rb_color(pc) ((pc)&1)
#define __rb_is_black(pc) __rb_color(pc)
#define __rb_is_red(pc) (!__rb_color(pc))
#define rb_color(rb) __rb_color((rb)->__rb_parent_color)
#define rb_is_red(rb) __rb_is_red((rb)->__rb_parent_color)
#define rb_is_black(rb) __rb_is_black((rb)->__rb_parent_color)

static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
        rb->__rb_parent_color = rb_color(rb) | (unsigned long)p;
}

static inline void rb_set_parent_color(struct rb_node *rb,
                                                                           struct rb_node *p, int color)
{
        rb->__rb_parent_color = (unsigned long)p | color;
}

static inline void
__rb_change_child(struct rb_node *old, struct rb_node *new,
                                  struct rb_node *parent, struct rb_root *root)
{
        int dir;
        if (parent)
        {
                /*
                if (parent->rb_left == old)
                        WRITE_ONCE(parent->rb_left, new);
                else
                        WRITE_ONCE(parent->rb_right, new);
                        */
                dir = (parent->rb_child[RB_LEFT] != old);
                WRITE_ONCE(parent->rb_child[dir], new);
        }
        else
                WRITE_ONCE(root->rb_node, new);
}

extern void __rb_erase_color(struct rb_node *parent, struct rb_root *root,
                                                         void (*augment_rotate)(struct rb_node *old, struct rb_node *new));

static __always_inline struct rb_node *
__rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                                         struct rb_node **leftmost,
                                         const struct rb_augment_callbacks *augment)
{
        struct rb_node *child = node->rb_child[RB_RIGHT];
        struct rb_node *tmp = node->rb_child[RB_LEFT];
        struct rb_node *parent, *rebalance;
        unsigned long pc;

        if (leftmost && node == *leftmost)
                *leftmost = rb_next(node);

        if (!tmp)
        {
                /*
                 * Case 1: node to erase has no more than 1 child (easy!)
                 *
                 * Note that if there is one child it must be red due to 5)
                 * and node must be black due to 4). We adjust colors locally
                 * so as to bypass __rb_erase_color() later on.
                 */
                pc = node->__rb_parent_color;
                parent = __rb_parent(pc);
                __rb_change_child(node, child, parent, root);
                if (child)
                {
                        child->__rb_parent_color = pc;
                        rebalance = NULL;
                }
                else
                        rebalance = __rb_is_black(pc) ? parent : NULL;
                tmp = parent;
        }
        else if (!child)
        {
                /* Still case 1, but this time the child is node->rb_left */
                tmp->__rb_parent_color = pc = node->__rb_parent_color;
                parent = __rb_parent(pc);
                __rb_change_child(node, tmp, parent, root);
                rebalance = NULL;
                tmp = parent;
        }
        else
        {
                struct rb_node *successor = child, *child2;

                tmp = child->rb_child[RB_LEFT];
                if (!tmp)
                {
                        /*
                         * Case 2: node's successor is its right child
                         *
                         *        (n)                  (s)
                         *        / \                  / \
                         *  (x) (s)  ->  (x) (c)
                         *                \
                         *                (c)
                         */
                        parent = successor;
                        child2 = successor->rb_child[RB_RIGHT];

                        augment->copy(node, successor);
                }
                else
                {
                        /*
                         * Case 3: node's successor is leftmost under
                         * node's right child subtree
                         *
                         *        (n)                  (s)
                         *        / \                  / \
                         *  (x) (y)  ->  (x) (y)
                         *          /                        /
                         *        (p)                  (p)
                         *        /                        /
                         *  (s)                  (c)
                         *        \
                         *        (c)
                         */
                        do
                        {
                                parent = successor;
                                successor = tmp;
                                tmp = tmp->rb_child[RB_LEFT];
                        } while (tmp);
                        child2 = successor->rb_child[RB_RIGHT];
                        WRITE_ONCE(parent->rb_child[RB_LEFT], child2);
                        WRITE_ONCE(successor->rb_child[RB_RIGHT], child);
                        rb_set_parent(child, successor);

                        augment->copy(node, successor);
                        augment->propagate(parent, successor);
                }

                tmp = node->rb_child[RB_LEFT];
                WRITE_ONCE(successor->rb_child[RB_LEFT], tmp);
                rb_set_parent(tmp, successor);

                pc = node->__rb_parent_color;
                tmp = __rb_parent(pc);
                __rb_change_child(node, successor, tmp, root);

                if (child2)
                {
                        successor->__rb_parent_color = pc;
                        rb_set_parent_color(child2, parent, RB_BLACK);
                        rebalance = NULL;
                }
                else
                {
                        unsigned long pc2 = successor->__rb_parent_color;
                        successor->__rb_parent_color = pc;
                        rebalance = __rb_is_black(pc2) ? parent : NULL;
                }
                tmp = successor;
        }

        augment->propagate(tmp, NULL);
        return rebalance;
}

// static __always_inline void
// rb_erase_augmented(struct rb_node *node, struct rb_root *root,
//                                    const struct rb_augment_callbacks *augment)
// {
//         struct rb_node *rebalance = __rb_erase_augmented(node, root,
//                                                                                                          NULL, augment);
//         if (rebalance)
//                 __rb_erase_color(rebalance, root, augment->rotate);
// }

// static __always_inline void
// rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
//                                                   const struct rb_augment_callbacks *augment)
// {
//         struct rb_node *rebalance = __rb_erase_augmented(node, &root->rb_root,
//                                                                                                          &root->rb_leftmost,
//                                                                                                          augment);
//         if (rebalance)
//                 __rb_erase_color(rebalance, &root->rb_root, augment->rotate);
// }

/*
 * red-black trees properties:  http://en.wikipedia.org/wiki/Rbtree
 *
 *  1) A node is either red or black
 *  2) The root is black
 *  3) All leaves (NULL) are black
 *  4) Both children of every red node are black
 *  5) Every simple path from root to leaves contains the same number
 *         of black nodes.
 *
 *  4 and 5 give the O(log n) guarantee, since 4 implies you cannot have two
 *  consecutive red nodes in a path and every red node is therefore followed by
 *  a black. So if B is the number of black nodes on every simple path (as per
 *  5), then the longest possible path due to 4 is 2B.
 *
 *  We shall indicate color with case, where black nodes are uppercase and red
 *  nodes will be lowercase. Unknown color nodes shall be drawn as red within
 *  parentheses and have some accompanying text comment.
 */

/*
 * Notes on lockless lookups:
 *
 * All stores to the tree structure (rb_left and rb_right) must be done using
 * WRITE_ONCE(). And we must not inadvertently cause (temporary) loops in the
 * tree structure as seen in program order.
 *
 * These two requirements will allow lockless iteration of the tree -- not
 * correct iteration mind you, tree rotations are not atomic so a lookup might
 * miss entire subtrees.
 *
 * But they do guarantee that any such traversal will only see valid elements
 * and that it will indeed complete -- does not get stuck in a loop.
 *
 * It also guarantees that if the lookup returns an element it is the 'correct'
 * one. But not returning an element does _NOT_ mean it's not present.
 *
 * NOTE:
 *
 * Stores to __rb_parent_color are not important for simple lookups so those
 * are left undone as of now. Nor did I check for loops involving parent
 * pointers.
 */

static inline void rb_set_black(struct rb_node *rb)
{
        rb->__rb_parent_color |= RB_BLACK;
}

static inline struct rb_node *rb_red_parent(struct rb_node *red)
{
        return (struct rb_node *)red->__rb_parent_color;
}

/*
 * Helper function for rotations:
 * - old's parent and color get assigned to new
 * - old gets assigned new as a parent and 'color' as a color.
 */
static inline void
__rb_rotate_set_parents(struct rb_node *old, struct rb_node *new,
                                                struct rb_root *root, int color)
{
        struct rb_node *parent = rb_parent(old);
        new->__rb_parent_color = old->__rb_parent_color;
        rb_set_parent_color(old, new, color);
        __rb_change_child(old, new, parent, root);
}

static __always_inline void
__rb_insert(struct rb_node *node, struct rb_root *root,
                        bool_t newleft, struct rb_node **leftmost,
                        void (*augment_rotate)(struct rb_node *old, struct rb_node *new))
{
        struct rb_node *parent = rb_red_parent(node), *gparent, *tmp;

        if (newleft)
                *leftmost = node;

        while (true)
        {
                /*
                 * Loop invariant: node is red.
                 */
                if (unlikely(!parent))
                {
                        /*
                         * The inserted node is root. Either this is the
                         * first node, or we recursed at Case 1 below and
                         * are no longer violating 4).
                         */
                        rb_set_parent_color(node, NULL, RB_BLACK);
                        break;
                }

                /*
                 * If there is a black parent, we are done.
                 * Otherwise, take some corrective action as,
                 * per 4), we don't want a red root or two
                 * consecutive red nodes.
                 */
                if (rb_is_black(parent))
                        break;

                gparent = rb_red_parent(parent);

                tmp = gparent->rb_child[RB_RIGHT];
                if (parent != tmp)
                { /* parent == gparent->rb_child[RB_LEFT] */
                        if (tmp && rb_is_red(tmp))
                        {
                                /*
                                 * Case 1 - node's uncle is red (color flips).
                                 *
                                 *           G                        g
                                 *          / \                  / \
                                 *         p   u  -->   P   U
                                 *        /                        /
                                 *   n                        n
                                 *
                                 * However, since g's parent might be red, and
                                 * 4) does not allow this, we need to recurse
                                 * at g.
                                 */
                                rb_set_parent_color(tmp, gparent, RB_BLACK);
                                rb_set_parent_color(parent, gparent, RB_BLACK);
                                node = gparent;
                                parent = rb_parent(node);
                                rb_set_parent_color(node, parent, RB_RED);
                                continue;
                        }

                        tmp = parent->rb_child[RB_RIGHT];
                        if (node == tmp)
                        {
                                /*
                                 * Case 2 - node's uncle is black and node is
                                 * the parent's right child (left rotate at parent).
                                 *
                                 *          G                         G
                                 *         / \                   / \
                                 *        p   U  -->        n   U
                                 *         \                   /
                                 *          n                 p
                                 *
                                 * This still leaves us in violation of 4), the
                                 * continuation into Case 3 will fix that.
                                 */
                                tmp = node->rb_child[RB_LEFT];
                                WRITE_ONCE(parent->rb_child[RB_RIGHT], tmp);
                                WRITE_ONCE(node->rb_child[RB_LEFT], parent);
                                if (tmp)
                                        rb_set_parent_color(tmp, parent,
                                                                                RB_BLACK);
                                rb_set_parent_color(parent, node, RB_RED);
                                augment_rotate(parent, node);
                                parent = node;
                                tmp = node->rb_child[RB_RIGHT];
                        }

                        /*
                         * Case 3 - node's uncle is black and node is
                         * the parent's left child (right rotate at gparent).
                         *
                         *                G                   P
                         *           / \                 / \
                         *          p   U  -->  n   g
                         *         /                                 \
                         *        n                                   U
                         */
                        WRITE_ONCE(gparent->rb_child[RB_LEFT], tmp); /* == parent->rb_child[RB_RIGHT] */
                        WRITE_ONCE(parent->rb_child[RB_RIGHT], gparent);
                        if (tmp)
                                rb_set_parent_color(tmp, gparent, RB_BLACK);
                        __rb_rotate_set_parents(gparent, parent, root, RB_RED);
                        augment_rotate(gparent, parent);
                        break;
                }
                else
                {
                        tmp = gparent->rb_child[RB_LEFT];
                        if (tmp && rb_is_red(tmp))
                        {
                                /* Case 1 - color flips */
                                rb_set_parent_color(tmp, gparent, RB_BLACK);
                                rb_set_parent_color(parent, gparent, RB_BLACK);
                                node = gparent;
                                parent = rb_parent(node);
                                rb_set_parent_color(node, parent, RB_RED);
                                continue;
                        }

                        tmp = parent->rb_child[RB_LEFT];
                        if (node == tmp)
                        {
                                /* Case 2 - right rotate at parent */
                                tmp = node->rb_child[RB_RIGHT];
                                WRITE_ONCE(parent->rb_child[RB_LEFT], tmp);
                                WRITE_ONCE(node->rb_child[RB_RIGHT], parent);
                                if (tmp)
                                        rb_set_parent_color(tmp, parent,
                                                                                RB_BLACK);
                                rb_set_parent_color(parent, node, RB_RED);
                                augment_rotate(parent, node);
                                parent = node;
                                tmp = node->rb_child[RB_LEFT];
                        }

                        /* Case 3 - left rotate at gparent */
                        WRITE_ONCE(gparent->rb_child[RB_RIGHT], tmp); /* == parent->rb_child[RB_LEFT] */
                        WRITE_ONCE(parent->rb_child[RB_LEFT], gparent);
                        if (tmp)
                                rb_set_parent_color(tmp, gparent, RB_BLACK);
                        __rb_rotate_set_parents(gparent, parent, root, RB_RED);
                        augment_rotate(gparent, parent);
                        break;
                }
        }
}

/*
 * Inline version for rb_erase() use - we want to be able to inline
 * and eliminate the dummy_rotate callback there
 */
static __always_inline void
____rb_erase_color(struct rb_node *parent, struct rb_root *root,
                                   void (*augment_rotate)(struct rb_node *old, struct rb_node *new))
{
        struct rb_node *node = NULL, *sibling, *tmp1, *tmp2;

        while (true)
        {
                /*
                 * Loop invariants:
                 * - node is black (or NULL on first iteration)
                 * - node is not the root (parent is not NULL)
                 * - All leaf paths going through parent and node have a
                 *   black node count that is 1 lower than other leaf paths.
                 */
                sibling = parent->rb_child[RB_RIGHT];
                if (node != sibling)
                { /* node == parent->rb_child[RB_LEFT] */
                        if (rb_is_red(sibling))
                        {
                                /*
                                 * Case 1 - left rotate at parent
                                 *
                                 *         P                           S
                                 *        / \                         / \
                                 *   N   s        -->        p   Sr
                                 *          / \                 / \
                                 *         Sl  Sr          N   Sl
                                 */
                                tmp1 = sibling->rb_child[RB_LEFT];
                                WRITE_ONCE(parent->rb_child[RB_RIGHT], tmp1);
                                WRITE_ONCE(sibling->rb_child[RB_LEFT], parent);
                                rb_set_parent_color(tmp1, parent, RB_BLACK);
                                __rb_rotate_set_parents(parent, sibling, root,
                                                                                RB_RED);
                                augment_rotate(parent, sibling);
                                sibling = tmp1;
                        }
                        tmp1 = sibling->rb_child[RB_RIGHT];
                        if (!tmp1 || rb_is_black(tmp1))
                        {
                                tmp2 = sibling->rb_child[RB_LEFT];
                                if (!tmp2 || rb_is_black(tmp2))
                                {
                                        /*
                                         * Case 2 - sibling color flip
                                         * (p could be either color here)
                                         *
                                         *        (p)                   (p)
                                         *        / \                   / \
                                         *   N   S        -->  N   s
                                         *          / \                   / \
                                         *         Sl  Sr                Sl  Sr
                                         *
                                         * This leaves us violating 5) which
                                         * can be fixed by flipping p to black
                                         * if it was red, or by recursing at p.
                                         * p is red when coming from Case 1.
                                         */
                                        rb_set_parent_color(sibling, parent,
                                                                                RB_RED);
                                        if (rb_is_red(parent))
                                                rb_set_black(parent);
                                        else
                                        {
                                                node = parent;
                                                parent = rb_parent(node);
                                                if (parent)
                                                        continue;
                                        }
                                        break;
                                }
                                /*
                                 * Case 3 - right rotate at sibling
                                 * (p could be either color here)
                                 *
                                 *   (p)                   (p)
                                 *   / \                   / \
                                 *  N   S        -->  N   sl
                                 *         / \                         \
                                 *        sl  Sr                        S
                                 *                                           \
                                 *                                                Sr
                                 *
                                 * Note: p might be red, and then both
                                 * p and sl are red after rotation(which
                                 * breaks property 4). This is fixed in
                                 * Case 4 (in __rb_rotate_set_parents()
                                 *                 which set sl the color of p
                                 *                 and set p RB_BLACK)
                                 *
                                 *   (p)                        (sl)
                                 *   / \                        /  \
                                 *  N   sl   -->   P        S
                                 *           \                /          \
                                 *                S          N                Sr
                                 *                 \
                                 *                  Sr
                                 */
                                tmp1 = tmp2->rb_child[RB_RIGHT];
                                WRITE_ONCE(sibling->rb_child[RB_LEFT], tmp1);
                                WRITE_ONCE(tmp2->rb_child[RB_RIGHT], sibling);
                                WRITE_ONCE(parent->rb_child[RB_RIGHT], tmp2);
                                if (tmp1)
                                        rb_set_parent_color(tmp1, sibling,
                                                                                RB_BLACK);
                                augment_rotate(sibling, tmp2);
                                tmp1 = sibling;
                                sibling = tmp2;
                        }
                        /*
                         * Case 4 - left rotate at parent + color flips
                         * (p and sl could be either color here.
                         *  After rotation, p becomes black, s acquires
                         *  p's color, and sl keeps its color)
                         *
                         *          (p)                         (s)
                         *          / \                         / \
                         *         N   S         -->   P   Sr
                         *                / \                 / \
                         *          (sl) sr          N  (sl)
                         */
                        tmp2 = sibling->rb_child[RB_LEFT];
                        WRITE_ONCE(parent->rb_child[RB_RIGHT], tmp2);
                        WRITE_ONCE(sibling->rb_child[RB_LEFT], parent);
                        rb_set_parent_color(tmp1, sibling, RB_BLACK);
                        if (tmp2)
                                rb_set_parent(tmp2, parent);
                        __rb_rotate_set_parents(parent, sibling, root,
                                                                        RB_BLACK);
                        augment_rotate(parent, sibling);
                        break;
                }
                else
                {
                        sibling = parent->rb_child[RB_LEFT];
                        if (rb_is_red(sibling))
                        {
                                /* Case 1 - right rotate at parent */
                                tmp1 = sibling->rb_child[RB_RIGHT];
                                WRITE_ONCE(parent->rb_child[RB_LEFT], tmp1);
                                WRITE_ONCE(sibling->rb_child[RB_RIGHT], parent);
                                rb_set_parent_color(tmp1, parent, RB_BLACK);
                                __rb_rotate_set_parents(parent, sibling, root,
                                                                                RB_RED);
                                augment_rotate(parent, sibling);
                                sibling = tmp1;
                        }
                        tmp1 = sibling->rb_child[RB_LEFT];
                        if (!tmp1 || rb_is_black(tmp1))
                        {
                                tmp2 = sibling->rb_child[RB_RIGHT];
                                if (!tmp2 || rb_is_black(tmp2))
                                {
                                        /* Case 2 - sibling color flip */
                                        rb_set_parent_color(sibling, parent,
                                                                                RB_RED);
                                        if (rb_is_red(parent))
                                                rb_set_black(parent);
                                        else
                                        {
                                                node = parent;
                                                parent = rb_parent(node);
                                                if (parent)
                                                        continue;
                                        }
                                        break;
                                }
                                /* Case 3 - left rotate at sibling */
                                tmp1 = tmp2->rb_child[RB_LEFT];
                                WRITE_ONCE(sibling->rb_child[RB_RIGHT], tmp1);
                                WRITE_ONCE(tmp2->rb_child[RB_LEFT], sibling);
                                WRITE_ONCE(parent->rb_child[RB_LEFT], tmp2);
                                if (tmp1)
                                        rb_set_parent_color(tmp1, sibling,
                                                                                RB_BLACK);
                                augment_rotate(sibling, tmp2);
                                tmp1 = sibling;
                                sibling = tmp2;
                        }
                        /* Case 4 - right rotate at parent + color flips */
                        tmp2 = sibling->rb_child[RB_RIGHT];
                        WRITE_ONCE(parent->rb_child[RB_LEFT], tmp2);
                        WRITE_ONCE(sibling->rb_child[RB_RIGHT], parent);
                        rb_set_parent_color(tmp1, sibling, RB_BLACK);
                        if (tmp2)
                                rb_set_parent(tmp2, parent);
                        __rb_rotate_set_parents(parent, sibling, root,
                                                                        RB_BLACK);
                        augment_rotate(parent, sibling);
                        break;
                }
        }
}

/* Non-inline version for rb_erase_augmented() use */
void __rb_erase_color(struct rb_node *parent, struct rb_root *root,
                                          void (*augment_rotate)(struct rb_node *old, struct rb_node *new))
{
        ____rb_erase_color(parent, root, augment_rotate);
}

/*
 * Non-augmented rbtree manipulation functions.
 *
 * We use dummy augmented callbacks here, and have the compiler optimize them
 * out of the rb_insert_color() and rb_erase() function definitions.
 */

static inline void dummy_propagate(struct rb_node *node, struct rb_node *stop) {}
static inline void dummy_copy(struct rb_node *old, struct rb_node *new) {}
static inline void dummy_rotate(struct rb_node *old, struct rb_node *new) {}

static const struct rb_augment_callbacks dummy_callbacks = {
        .propagate = dummy_propagate,
        .copy = dummy_copy,
        .rotate = dummy_rotate};

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
        __rb_insert(node, root, false, NULL, dummy_rotate);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
        struct rb_node *rebalance;
        rebalance = __rb_erase_augmented(node, root,
                                                                         NULL, &dummy_callbacks);
        if (rebalance)
                ____rb_erase_color(rebalance, root, dummy_rotate);
}

void rb_insert_color_cached(struct rb_node *node,
                                                        struct rb_root_cached *root, bool_t leftmost)
{
        __rb_insert(node, &root->rb_root, leftmost,
                                &root->rb_leftmost, dummy_rotate);
}

void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root)
{
        struct rb_node *rebalance;
        rebalance = __rb_erase_augmented(node, &root->rb_root,
                                                                         &root->rb_leftmost, &dummy_callbacks);
        if (rebalance)
                ____rb_erase_color(rebalance, &root->rb_root, dummy_rotate);
}

/*
 * Augmented rbtree manipulation functions.
 *
 * This instantiates the same __always_inline functions as in the non-augmented
 * case, but this time with user-defined callbacks.
 */

void __rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                                                   bool_t newleft, struct rb_node **leftmost,
                                                   void (*augment_rotate)(struct rb_node *old, struct rb_node *new))
{
        __rb_insert(node, root, newleft, leftmost, augment_rotate);
}

/*
 * This function returns the first node (in sort order) of the tree.
 */
struct rb_node *rb_first(const struct rb_root *root)
{
        struct rb_node *n;

        n = root->rb_node;
        if (!n)
                return NULL;
        while (n->rb_child[RB_LEFT])
                n = n->rb_child[RB_LEFT];
        return n;
}

struct rb_node *rb_last(const struct rb_root *root)
{
        struct rb_node *n;

        n = root->rb_node;
        if (!n)
                return NULL;
        while (n->rb_child[RB_RIGHT])
                n = n->rb_child[RB_RIGHT];
        return n;
}

struct rb_node *rb_next(const struct rb_node *node)
{
        struct rb_node *parent;

        if (RB_EMPTY_NODE(node))
                return NULL;

        /*
         * If we have a right-hand child, go down and then left as far
         * as we can.
         */
        if (node->rb_child[RB_RIGHT])
        {
                node = node->rb_child[RB_RIGHT];
                while (node->rb_child[RB_LEFT])
                        node = node->rb_child[RB_LEFT];
                return (struct rb_node *)node;
        }

        /*
         * No right-hand children. Everything down and left is smaller than us,
         * so any 'next' node must be in the general direction of our parent.
         * Go up the tree; any time the ancestor is a right-hand child of its
         * parent, keep going up. First time it's a left-hand child of its
         * parent, said parent is our 'next' node.
         */
        while ((parent = rb_parent(node)) && node == parent->rb_child[RB_RIGHT])
                node = parent;

        return parent;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
        struct rb_node *parent;

        if (RB_EMPTY_NODE(node))
                return NULL;

        /*
         * If we have a left-hand child, go down and then right as far
         * as we can.
         */
        if (node->rb_child[RB_LEFT])
        {
                node = node->rb_child[RB_LEFT];
                while (node->rb_child[RB_RIGHT])
                        node = node->rb_child[RB_RIGHT];
                return (struct rb_node *)node;
        }

        /*
         * No left-hand children. Go up till we find an ancestor which
         * is a right-hand child of its parent.
         */
        while ((parent = rb_parent(node)) && node == parent->rb_child[RB_LEFT])
                node = parent;

        return parent;
}

void rb_replace_node(struct rb_node *victim, struct rb_node *new,
                                         struct rb_root *root)
{
        struct rb_node *parent = rb_parent(victim);

        /* Copy the pointers/colour from the victim to the replacement */
        *new = *victim;

        /* Set the surrounding nodes to point to the replacement */
        if (victim->rb_child[RB_LEFT])
                rb_set_parent(victim->rb_child[RB_LEFT], new);
        if (victim->rb_child[RB_RIGHT])
                rb_set_parent(victim->rb_child[RB_RIGHT], new);
        __rb_change_child(victim, new, parent, root);
}

void rb_replace_node_cached(struct rb_node *victim, struct rb_node *new,
                                                        struct rb_root_cached *root)
{
        rb_replace_node(victim, new, &root->rb_root);

        if (root->rb_leftmost == victim)
                root->rb_leftmost = new;
}

static struct rb_node *rb_left_deepest_node(const struct rb_node *node)
{
        for (;;)
        {
                if (node->rb_child[RB_LEFT])
                        node = node->rb_child[RB_LEFT];
                else if (node->rb_child[RB_RIGHT])
                        node = node->rb_child[RB_RIGHT];
                else
                        return (struct rb_node *)node;
        }
}

struct rb_node *rb_next_postorder(const struct rb_node *node)
{
        const struct rb_node *parent;
        if (!node)
                return NULL;
        parent = rb_parent(node);

        /* If we're sitting on node, we've already seen our children */
        if (parent && node == parent->rb_child[RB_LEFT] && parent->rb_child[RB_RIGHT])
        {
                /* If we are the parent's left node, go to the parent's right
                 * node then all the way down to the left */
                return rb_left_deepest_node(parent->rb_child[RB_RIGHT]);
        }
        else
                /* Otherwise we are the parent's right node, and the parent
                 * should be next */
                return (struct rb_node *)parent;
}

struct rb_node *rb_first_postorder(const struct rb_root *root)
{
        if (!root->rb_node)
                return NULL;

        return rb_left_deepest_node(root->rb_node);
}

//
struct mynode
{
        struct rb_node node;
        unsigned long key;
};

static inline struct rb_root *my_rbinit(struct rb_root *root)
{
        root->rb_node = NULL;
        root->count = 0;
        return root;
}

static inline struct mynode *my_search(struct rb_root *root, unsigned long key)
{
        struct rb_node *pnode = root->rb_node;
        //if(pnode == NULL) fatal("pnode为空?\n");
        while (pnode)
        {
                struct mynode *data = rb_entry(pnode, struct mynode, node);
                // int result;
                unsigned long key1 = data->key;
                // result = key1;

                if (key < key1)
                        pnode = pnode->rb_child[RB_LEFT];
                else if (key > key1)
                        pnode = pnode->rb_child[RB_RIGHT];
                else{
                        // if(data->key != key) {        
                        //         printf("key:%llu, key1:%llx, data->key:%llu\n",key,key1,data->key);
                        //         //fatal("!???\n");
                        // }
                        return data;
                }
        }

        return NULL;
}

static inline int my_insert(struct rb_root *root, struct mynode *data)
{
        struct rb_node **new = &(root->rb_node), *parent = NULL;
        data->node.rb_child[0] = NULL;
        data->node.rb_child[1] = NULL;
        data->node.__rb_parent_color = 0;
        /* Figure out where to put new node */
          while (*new) {
                  struct mynode *this = rb_entry(*new, struct mynode, node);
                  //int result = data->key>=this->key;

                parent = *new;
                  if (data->key < this->key )
                          new = &((*new)->rb_child[RB_LEFT]);
                  else if (data->key >= this->key)
                          new = &((*new)->rb_child[RB_RIGHT]);
                  else
                          return 0;
          }
        cmb();
        /* Add new node and rebalance tree. */
        rb_link_node(&data->node, parent, new);
        rb_insert_color(&data->node, root);

        return 1;
}

static inline void my_free(struct mynode *node)
{
        if (node != NULL)
        {
                //free(node);
                node = NULL;
        }
}

// word_t rbtree_insert_cb(APFSDS_ARGLIST)
// {
//         PARAM_USED(arg2, arg3);
//         // word_t ret;
//         if ((arg0 != 0) && (arg1 != 0))
//                 my_insert((struct rb_root *)arg0, (struct mynode *)arg1);
//         //if(!ret)fatal("insert failed!\n");
//         //else printf("rb_root%llx,插入成功%llx, key:%llu\n",arg0,arg1, ((struct mynode *)arg1)->key);
//         //wmb();
//         return (++((struct rb_root *)arg0)->count);
// }

// word_t rbtree_remove_cb(APFSDS_ARGLIST)
// {
//         PARAM_USED(arg2, arg3);
//         struct mynode *ret = NULL;
//         // arg 0 = tree, arg1 = key
//         if (arg0 == 0)
//                 return (word_t)ret;

//         ret = my_search((struct rb_root *)arg0, (unsigned long)arg1);
//         if (ret)
//         {
//                 rb_erase(&ret->node, (struct rb_root *)arg0);
//                 //printf("rb_root%llx,弹出成功%llx, original key:%llu, key:%llu\n",arg0, ret, (unsigned long)arg1, ret->key);
//         }
//         // else {
//         //         printf("search failed, key: %llu\n", arg1);        
//         //         fatal("search none!\n");
//         // }
//         //wmb();
//         return (word_t)ret;
// }