/* -*- mode: C -*-  */
/*
   IGraph library.
   Copyright (C) 2010-2021  The igraph development team

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA

*/

#include "igraph_flow.h"

#include "igraph_adjlist.h"
#include "igraph_constants.h"
#include "igraph_constructors.h"
#include "igraph_components.h"
#include "igraph_error.h"
#include "igraph_interface.h"
#include "igraph_operators.h"
#include "igraph_stack.h"
#include "igraph_visitor.h"

#include "core/estack.h"
#include "core/marked_queue.h"
#include "flow/flow_internal.h"
#include "graph/attributes.h"
#include "math/safe_intop.h"

typedef igraph_error_t igraph_provan_shier_pivot_t(const igraph_t *graph,
                                                   const igraph_marked_queue_int_t *S,
                                                   const igraph_estack_t *T,
                                                   igraph_integer_t source,
                                                   igraph_integer_t target,
                                                   igraph_integer_t *v,
                                                   igraph_vector_int_t *Isv,
                                                   void *arg);

/**
 * \function igraph_even_tarjan_reduction
 * \brief Even-Tarjan reduction of a graph.
 *
 * A digraph is created with twice as many vertices and edges. For each
 * original vertex \c i, two vertices <code>i' = i</code> and
 * <code>i'' = i' + n</code> are created,
 * with a directed edge from <code>i'</code> to <code>i''</code>.
 * For each original directed edge from \c i to \c j, two new edges are created,
 * from <code>i'</code> to <code>j''</code> and from <code>i''</code>
 * to <code>j'</code>.
 *
 * </para><para>This reduction is used in the paper (observation 2):
 * Arkady Kanevsky: Finding all minimum-size separating vertex sets in
 * a graph, Networks 23, 533--541, 1993.
 *
 * </para><para>The original paper where this reduction was conceived is
 * Shimon Even and R. Endre Tarjan: Network Flow and Testing Graph
 * Connectivity, SIAM J. Comput., 4(4), 507–518.
 *
 * \param graph A graph. Although directness is not checked, this function
 *        is commonly used only on directed graphs.
 * \param graphbar Pointer to a new directed graph that will contain the
 *        reduction, with twice as many vertices and edges.
 * \param capacity Pointer to an initialized vector or a null pointer. If
 *        not a null pointer, then it will be filled the capacity from
 *        the reduction: the first |E| elements are 1, the remaining |E|
 *        are equal to |V| (which is used to indicate infinity).
 * \return Error code.
 *
 * Time complexity: O(|E|+|V|).
 *
 * \example examples/simple/even_tarjan.c
 */

igraph_error_t igraph_even_tarjan_reduction(const igraph_t *graph, igraph_t *graphbar,
                                 igraph_vector_t *capacity) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_integer_t no_of_edges = igraph_ecount(graph);

    igraph_integer_t new_no_of_nodes;
    igraph_integer_t new_no_of_edges = no_of_edges * 2;

    igraph_vector_int_t edges;
    igraph_integer_t edgeptr = 0, capptr = 0;
    igraph_integer_t i;

    IGRAPH_SAFE_MULT(no_of_nodes, 2, &new_no_of_nodes);
    IGRAPH_SAFE_ADD(new_no_of_edges, no_of_nodes, &new_no_of_edges);

    /* To ensure the size of the edges vector will not overflow. */
    if (new_no_of_edges > IGRAPH_ECOUNT_MAX) {
        IGRAPH_ERROR("Overflow in number of edges.", IGRAPH_EOVERFLOW);
    }

    IGRAPH_VECTOR_INT_INIT_FINALLY(&edges, new_no_of_edges * 2);

    if (capacity) {
        IGRAPH_CHECK(igraph_vector_resize(capacity, new_no_of_edges));
    }

    /* Every vertex 'i' is replaced by two vertices, i' and i'' */
    /* id[i'] := id[i] ; id[i''] := id[i] + no_of_nodes */

    /* One edge for each original vertex, for i, we add (i',i'') */
    for (i = 0; i < no_of_nodes; i++) {
        VECTOR(edges)[edgeptr++] = i;
        VECTOR(edges)[edgeptr++] = i + no_of_nodes;
        if (capacity) {
            VECTOR(*capacity)[capptr++] = 1.0;
        }
    }

    /* Two news edges for each original edge
       (from,to) becomes (from'',to'), (to'',from') */
    for (i = 0; i < no_of_edges; i++) {
        igraph_integer_t from = IGRAPH_FROM(graph, i);
        igraph_integer_t to = IGRAPH_TO(graph, i);
        VECTOR(edges)[edgeptr++] = from + no_of_nodes;
        VECTOR(edges)[edgeptr++] = to;
        VECTOR(edges)[edgeptr++] = to + no_of_nodes;
        VECTOR(edges)[edgeptr++] = from;
        if (capacity) {
            VECTOR(*capacity)[capptr++] = no_of_nodes; /* TODO: should be Inf */
            VECTOR(*capacity)[capptr++] = no_of_nodes; /* TODO: should be Inf */
        }
    }

    IGRAPH_CHECK(igraph_create(graphbar, &edges, new_no_of_nodes,
                               IGRAPH_DIRECTED));

    igraph_vector_int_destroy(&edges);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

static igraph_error_t igraph_i_residual_graph(const igraph_t *graph,
                                   const igraph_vector_t *capacity,
                                   igraph_t *residual,
                                   igraph_vector_t *residual_capacity,
                                   const igraph_vector_t *flow,
                                   igraph_vector_int_t *tmp) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_integer_t no_of_edges = igraph_ecount(graph);
    igraph_integer_t i, no_new_edges = 0;
    igraph_integer_t edgeptr = 0, capptr = 0;

    for (i = 0; i < no_of_edges; i++) {
        if (VECTOR(*flow)[i] < VECTOR(*capacity)[i]) {
            no_new_edges++;
        }
    }

    IGRAPH_CHECK(igraph_vector_int_resize(tmp, no_new_edges * 2));
    if (residual_capacity) {
        IGRAPH_CHECK(igraph_vector_resize(residual_capacity, no_new_edges));
    }

    for (i = 0; i < no_of_edges; i++) {
        igraph_real_t c = VECTOR(*capacity)[i] - VECTOR(*flow)[i];
        if (c > 0) {
            igraph_integer_t from = IGRAPH_FROM(graph, i);
            igraph_integer_t to = IGRAPH_TO(graph, i);
            VECTOR(*tmp)[edgeptr++] = from;
            VECTOR(*tmp)[edgeptr++] = to;
            if (residual_capacity) {
                VECTOR(*residual_capacity)[capptr++] = c;
            }
        }
    }

    IGRAPH_CHECK(igraph_create(residual, tmp, no_of_nodes,
                               IGRAPH_DIRECTED));

    return IGRAPH_SUCCESS;
}

igraph_error_t igraph_residual_graph(const igraph_t *graph,
                          const igraph_vector_t *capacity,
                          igraph_t *residual,
                          igraph_vector_t *residual_capacity,
                          const igraph_vector_t *flow) {

    igraph_vector_int_t tmp;
    igraph_integer_t no_of_edges = igraph_ecount(graph);

    if (igraph_vector_size(capacity) != no_of_edges) {
        IGRAPH_ERROR("Invalid `capacity' vector size", IGRAPH_EINVAL);
    }
    if (igraph_vector_size(flow) != no_of_edges) {
        IGRAPH_ERROR("Invalid `flow' vector size", IGRAPH_EINVAL);
    }

    IGRAPH_VECTOR_INT_INIT_FINALLY(&tmp, 0);

    IGRAPH_CHECK(igraph_i_residual_graph(graph, capacity, residual,
                                         residual_capacity, flow, &tmp));

    igraph_vector_int_destroy(&tmp);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

static igraph_error_t igraph_i_reverse_residual_graph(const igraph_t *graph,
                                           const igraph_vector_t *capacity,
                                           igraph_t *residual,
                                           const igraph_vector_t *flow,
                                           igraph_vector_int_t *tmp) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_integer_t no_of_edges = igraph_ecount(graph);
    igraph_integer_t i, no_new_edges = 0;
    igraph_integer_t edgeptr = 0;

    for (i = 0; i < no_of_edges; i++) {
        igraph_real_t cap = capacity ? VECTOR(*capacity)[i] : 1.0;
        if (VECTOR(*flow)[i] > 0) {
            no_new_edges++;
        }
        if (VECTOR(*flow)[i] < cap) {
            no_new_edges++;
        }
    }

    IGRAPH_CHECK(igraph_vector_int_resize(tmp, no_new_edges * 2));

    for (i = 0; i < no_of_edges; i++) {
        igraph_integer_t from = IGRAPH_FROM(graph, i);
        igraph_integer_t to = IGRAPH_TO(graph, i);
        igraph_real_t cap = capacity ? VECTOR(*capacity)[i] : 1.0;
        if (VECTOR(*flow)[i] > 0) {
            VECTOR(*tmp)[edgeptr++] = from;
            VECTOR(*tmp)[edgeptr++] = to;
        }
        if (VECTOR(*flow)[i] < cap) {
            VECTOR(*tmp)[edgeptr++] = to;
            VECTOR(*tmp)[edgeptr++] = from;
        }
    }

    IGRAPH_CHECK(igraph_create(residual, tmp, no_of_nodes,
                               IGRAPH_DIRECTED));

    return IGRAPH_SUCCESS;
}

igraph_error_t igraph_reverse_residual_graph(const igraph_t *graph,
                                  const igraph_vector_t *capacity,
                                  igraph_t *residual,
                                  const igraph_vector_t *flow) {
    igraph_vector_int_t tmp;
    igraph_integer_t no_of_edges = igraph_ecount(graph);

    if (capacity && igraph_vector_size(capacity) != no_of_edges) {
        IGRAPH_ERROR("Invalid `capacity' vector size", IGRAPH_EINVAL);
    }
    if (igraph_vector_size(flow) != no_of_edges) {
        IGRAPH_ERROR("Invalid `flow' vector size", IGRAPH_EINVAL);
    }
    IGRAPH_VECTOR_INT_INIT_FINALLY(&tmp, 0);

    IGRAPH_CHECK(igraph_i_reverse_residual_graph(graph, capacity, residual,
                 flow, &tmp));

    igraph_vector_int_destroy(&tmp);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

typedef struct igraph_i_dbucket_t {
    igraph_vector_int_t head;
    igraph_vector_int_t next;
} igraph_i_dbucket_t;

static igraph_error_t igraph_i_dbucket_init(igraph_i_dbucket_t *buck, igraph_integer_t size) {
    IGRAPH_VECTOR_INT_INIT_FINALLY(&buck->head, size);
    IGRAPH_CHECK(igraph_vector_int_init(&buck->next, size));
    IGRAPH_FINALLY_CLEAN(1);
    return IGRAPH_SUCCESS;
}

static void igraph_i_dbucket_destroy(igraph_i_dbucket_t *buck) {
    igraph_vector_int_destroy(&buck->head);
    igraph_vector_int_destroy(&buck->next);
}

static igraph_error_t igraph_i_dbucket_insert(igraph_i_dbucket_t *buck, igraph_integer_t bid,
                                   igraph_integer_t elem) {
    /* Note: we can do this, since elem is not in any buckets */
    VECTOR(buck->next)[elem] = VECTOR(buck->head)[bid];
    VECTOR(buck->head)[bid] = elem + 1;
    return IGRAPH_SUCCESS;
}

static igraph_integer_t igraph_i_dbucket_empty(const igraph_i_dbucket_t *buck,
                                       igraph_integer_t bid) {
    return VECTOR(buck->head)[bid] == 0;
}

static igraph_integer_t igraph_i_dbucket_delete(igraph_i_dbucket_t *buck, igraph_integer_t bid) {
    igraph_integer_t elem = VECTOR(buck->head)[bid] - 1;
    VECTOR(buck->head)[bid] = VECTOR(buck->next)[elem];
    return elem;
}

static igraph_error_t igraph_i_dominator_LINK(igraph_integer_t v, igraph_integer_t w,
                                   igraph_vector_int_t *ancestor) {
    VECTOR(*ancestor)[w] = v + 1;
    return IGRAPH_SUCCESS;
}

/* TODO: don't always reallocate path */

static igraph_error_t igraph_i_dominator_COMPRESS(igraph_integer_t v,
                                       igraph_vector_int_t *ancestor,
                                       igraph_vector_int_t *label,
                                       igraph_vector_int_t *semi) {
    igraph_stack_int_t path;
    igraph_integer_t w = v;
    igraph_integer_t top, pretop;

    IGRAPH_CHECK(igraph_stack_int_init(&path, 10));
    IGRAPH_FINALLY(igraph_stack_int_destroy, &path);

    while (VECTOR(*ancestor)[w] != 0) {
        IGRAPH_CHECK(igraph_stack_int_push(&path, w));
        w = VECTOR(*ancestor)[w] - 1;
    }

    top = igraph_stack_int_pop(&path);
    while (!igraph_stack_int_empty(&path)) {
        pretop = igraph_stack_int_pop(&path);

        if (VECTOR(*semi)[VECTOR(*label)[top]] <
            VECTOR(*semi)[VECTOR(*label)[pretop]]) {
            VECTOR(*label)[pretop] = VECTOR(*label)[top];
        }
        VECTOR(*ancestor)[pretop] = VECTOR(*ancestor)[top];

        top = pretop;
    }

    igraph_stack_int_destroy(&path);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

static igraph_integer_t igraph_i_dominator_EVAL(igraph_integer_t v,
                                        igraph_vector_int_t *ancestor,
                                        igraph_vector_int_t *label,
                                        igraph_vector_int_t *semi) {
    if (VECTOR(*ancestor)[v] == 0) {
        return v;
    } else {
        igraph_i_dominator_COMPRESS(v, ancestor, label, semi);
        return VECTOR(*label)[v];
    }
}

/* TODO: implement the faster version. */

/**
 * \function igraph_dominator_tree
 * Calculates the dominator tree of a flowgraph
 *
 * A flowgraph is a directed graph with a distinguished start (or
 * root) vertex r, such that for any vertex v, there is a path from r
 * to v. A vertex v dominates another vertex w (not equal to v), if
 * every path from r to w contains v. Vertex v is the immediate
 * dominator or w, v=idom(w), if v dominates w and every other
 * dominator of w dominates v. The edges {(idom(w), w)| w is not r}
 * form a directed tree, rooted at r, called the dominator tree of the
 * graph. Vertex v dominates vertex w if and only if v is an ancestor
 * of w in the dominator tree.
 *
 * </para><para>This function implements the Lengauer-Tarjan algorithm
 * to construct the dominator tree of a directed graph. For details
 * please see Thomas Lengauer, Robert Endre Tarjan: A fast algorithm
 * for finding dominators in a flowgraph, ACM Transactions on
 * Programming Languages and Systems (TOPLAS) I/1, 121--141, 1979.
 *
 * \param graph A directed graph. If it is not a flowgraph, and it
 *        contains some vertices not reachable from the root vertex,
 *        then these vertices will be collected in the \c leftout
 *        vector.
 * \param root The id of the root (or source) vertex, this will be the
 *        root of the tree.
 * \param dom Pointer to an initialized vector or a null pointer. If
 *        not a null pointer, then the immediate dominator of each
 *        vertex will be stored here. For vertices that are not
 *        reachable from the root, -2 is stored here. For
 *        the root vertex itself, -1 is added.
 * \param domtree Pointer to an uninitialized igraph_t, or NULL. If
 *        not a null pointer, then the dominator tree is returned
 *        here. The graph contains the vertices that are unreachable
 *        from the root (if any), these will be isolates.
 * \param leftout Pointer to an initialized vector object, or NULL. If
 *        not NULL, then the IDs of the vertices that are unreachable
 *        from the root vertex (and thus not part of the dominator
 *        tree) are stored here.
 * \param mode Constant, must be \c IGRAPH_IN or \c IGRAPH_OUT. If it
 *        is \c IGRAPH_IN, then all directions are considered as
 *        opposite to the original one in the input graph.
 * \return Error code.
 *
 * Time complexity: very close to O(|E|+|V|), linear in the number of
 * edges and vertices. More precisely, it is O(|V|+|E|alpha(|E|,|V|)),
 * where alpha(|E|,|V|) is a functional inverse of Ackermann's
 * function.
 *
 * \example examples/simple/dominator_tree.c
 */

igraph_error_t igraph_dominator_tree(const igraph_t *graph,
                          igraph_integer_t root,
                          igraph_vector_int_t *dom,
                          igraph_t *domtree,
                          igraph_vector_int_t *leftout,
                          igraph_neimode_t mode) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);

    igraph_adjlist_t succ, pred;
    igraph_vector_int_t parent;
    igraph_vector_int_t semi;    /* +1 always */
    igraph_vector_int_t vertex;   /* +1 always */
    igraph_i_dbucket_t bucket;
    igraph_vector_int_t ancestor;
    igraph_vector_int_t label;

    igraph_neimode_t invmode = IGRAPH_REVERSE_MODE(mode);

    igraph_integer_t i;

    igraph_vector_int_t vdom, *mydom = dom;

    igraph_integer_t component_size = 0;

    if (root < 0 || root >= no_of_nodes) {
        IGRAPH_ERROR("Invalid root vertex ID for dominator tree",
                     IGRAPH_EINVAL);
    }

    if (!igraph_is_directed(graph)) {
        IGRAPH_ERROR("Dominator tree of an undirected graph requested",
                     IGRAPH_EINVAL);
    }

    if (mode == IGRAPH_ALL) {
        IGRAPH_ERROR("Invalid neighbor mode for dominator tree",
                     IGRAPH_EINVAL);
    }

    if (dom) {
        IGRAPH_CHECK(igraph_vector_int_resize(dom, no_of_nodes));
    } else {
        mydom = &vdom;
        IGRAPH_VECTOR_INT_INIT_FINALLY(mydom, no_of_nodes);
    }
    igraph_vector_int_fill(mydom, -2);

    IGRAPH_VECTOR_INT_INIT_FINALLY(&parent, no_of_nodes);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&semi, no_of_nodes);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&vertex, no_of_nodes);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&ancestor, no_of_nodes);
    IGRAPH_CHECK(igraph_vector_int_init_range(&label, 0, no_of_nodes));
    IGRAPH_FINALLY(igraph_vector_int_destroy, &label);
    IGRAPH_CHECK(igraph_adjlist_init(graph, &succ, mode, IGRAPH_LOOPS_ONCE, IGRAPH_MULTIPLE));
    IGRAPH_FINALLY(igraph_adjlist_destroy, &succ);
    IGRAPH_CHECK(igraph_adjlist_init(graph, &pred, invmode, IGRAPH_LOOPS_ONCE, IGRAPH_MULTIPLE));
    IGRAPH_FINALLY(igraph_adjlist_destroy, &pred);
    IGRAPH_CHECK(igraph_i_dbucket_init(&bucket, no_of_nodes));
    IGRAPH_FINALLY(igraph_i_dbucket_destroy, &bucket);

    /* DFS first, to set semi, vertex and parent, step 1 */

    IGRAPH_CHECK(igraph_dfs(graph, root, mode, /*unreachable=*/ 0,
                            /*order=*/ &vertex,
                            /*order_out=*/ 0, /*father=*/ &parent,
                            /*dist=*/ 0, /*in_callback=*/ 0,
                            /*out_callback=*/ 0, /*extra=*/ 0));

    for (i = 0; i < no_of_nodes; i++) {
        if (VECTOR(vertex)[i] >= 0) {
            igraph_integer_t t = VECTOR(vertex)[i];
            VECTOR(semi)[t] = component_size + 1;
            VECTOR(vertex)[component_size] = t + 1;
            component_size++;
        }
    }
    if (leftout) {
        igraph_integer_t n = no_of_nodes - component_size;
        igraph_integer_t p = 0, j;
        IGRAPH_CHECK(igraph_vector_int_resize(leftout, n));
        for (j = 0; j < no_of_nodes && p < n; j++) {
            if (VECTOR(parent)[j] < -1) {
                VECTOR(*leftout)[p++] = j;
            }
        }
    }

    /* We need to go over 'pred' because it should contain only the
       edges towards the target vertex. */
    for (i = 0; i < no_of_nodes; i++) {
        igraph_vector_int_t *v = igraph_adjlist_get(&pred, i);
        igraph_integer_t j, n = igraph_vector_int_size(v);
        for (j = 0; j < n; ) {
            igraph_integer_t v2 = VECTOR(*v)[j];
            if (VECTOR(parent)[v2] >= -1) {
                j++;
            } else {
                VECTOR(*v)[j] = VECTOR(*v)[n - 1];
                igraph_vector_int_pop_back(v);
                n--;
            }
        }
    }

    /* Now comes the main algorithm, steps 2 & 3 */

    for (i = component_size - 1; i > 0; i--) {
        igraph_integer_t w = VECTOR(vertex)[i] - 1;
        igraph_vector_int_t *predw = igraph_adjlist_get(&pred, w);
        igraph_integer_t j, n = igraph_vector_int_size(predw);
        for (j = 0; j < n; j++) {
            igraph_integer_t v = VECTOR(*predw)[j];
            igraph_integer_t u = igraph_i_dominator_EVAL(v, &ancestor, &label, &semi);
            if (VECTOR(semi)[u] < VECTOR(semi)[w]) {
                VECTOR(semi)[w] = VECTOR(semi)[u];
            }
        }
        igraph_i_dbucket_insert(&bucket, VECTOR(vertex)[ VECTOR(semi)[w] - 1 ] - 1, w);
        igraph_i_dominator_LINK(VECTOR(parent)[w], w, &ancestor);
        while (!igraph_i_dbucket_empty(&bucket, VECTOR(parent)[w])) {
            igraph_integer_t v = igraph_i_dbucket_delete(&bucket, VECTOR(parent)[w]);
            igraph_integer_t u = igraph_i_dominator_EVAL(v, &ancestor, &label, &semi);
            VECTOR(*mydom)[v] = VECTOR(semi)[u] < VECTOR(semi)[v] ? u :
                                VECTOR(parent)[w];
        }
    }

    /* Finally, step 4 */

    for (i = 1; i < component_size; i++) {
        igraph_integer_t w = VECTOR(vertex)[i] - 1;
        if (VECTOR(*mydom)[w] != VECTOR(vertex)[VECTOR(semi)[w] - 1] - 1) {
            VECTOR(*mydom)[w] = VECTOR(*mydom)[VECTOR(*mydom)[w]];
        }
    }
    VECTOR(*mydom)[root] = -1;

    igraph_i_dbucket_destroy(&bucket);
    igraph_adjlist_destroy(&pred);
    igraph_adjlist_destroy(&succ);
    igraph_vector_int_destroy(&label);
    igraph_vector_int_destroy(&ancestor);
    igraph_vector_int_destroy(&vertex);
    igraph_vector_int_destroy(&semi);
    igraph_vector_int_destroy(&parent);
    IGRAPH_FINALLY_CLEAN(8);

    if (domtree) {
        igraph_vector_int_t edges;
        igraph_integer_t ptr = 0;
        IGRAPH_VECTOR_INT_INIT_FINALLY(&edges, component_size * 2 - 2);
        for (i = 0; i < no_of_nodes; i++) {
            if (i != root && VECTOR(*mydom)[i] >= 0) {
                if (mode == IGRAPH_OUT) {
                    VECTOR(edges)[ptr++] = VECTOR(*mydom)[i];
                    VECTOR(edges)[ptr++] = i;
                } else {
                    VECTOR(edges)[ptr++] = i;
                    VECTOR(edges)[ptr++] = VECTOR(*mydom)[i];
                }
            }
        }
        IGRAPH_CHECK(igraph_create(domtree, &edges, no_of_nodes,
                                   IGRAPH_DIRECTED));
        igraph_vector_int_destroy(&edges);
        IGRAPH_FINALLY_CLEAN(1);

        IGRAPH_I_ATTRIBUTE_DESTROY(domtree);
        IGRAPH_I_ATTRIBUTE_COPY(domtree, graph, /*graph=*/ 1, /*vertex=*/ 1,
                                /*edge=*/ 0);
    }

    if (!dom) {
        igraph_vector_int_destroy(&vdom);
        IGRAPH_FINALLY_CLEAN(1);
    }

    return IGRAPH_SUCCESS;
}

typedef struct igraph_i_all_st_cuts_minimal_dfs_data_t {
    igraph_stack_int_t *stack;
    igraph_vector_bool_t *nomark;
    const igraph_vector_bool_t *GammaX;
    igraph_integer_t root;
    const igraph_vector_int_t *map;
} igraph_i_all_st_cuts_minimal_dfs_data_t;

static igraph_error_t igraph_i_all_st_cuts_minimal_dfs_incb(
        const igraph_t *graph,
        igraph_integer_t vid,
        igraph_integer_t dist,
        void *extra) {

    igraph_i_all_st_cuts_minimal_dfs_data_t *data = extra;
    igraph_stack_int_t *stack = data->stack;
    igraph_vector_bool_t *nomark = data->nomark;
    const igraph_vector_bool_t *GammaX = data->GammaX;
    const igraph_vector_int_t *map = data->map;
    igraph_integer_t realvid = VECTOR(*map)[vid];

    IGRAPH_UNUSED(graph); IGRAPH_UNUSED(dist);

    if (VECTOR(*GammaX)[realvid]) {
        if (!igraph_stack_int_empty(stack)) {
            igraph_integer_t top = igraph_stack_int_top(stack);
            VECTOR(*nomark)[top] = 1; /* we just found a smaller one */
        }
        IGRAPH_CHECK(igraph_stack_int_push(stack, realvid));
    }

    return IGRAPH_SUCCESS;
}

static igraph_error_t igraph_i_all_st_cuts_minimal_dfs_outcb(
        const igraph_t *graph,
        igraph_integer_t vid,
        igraph_integer_t dist,
        void *extra) {
    igraph_i_all_st_cuts_minimal_dfs_data_t *data = extra;
    igraph_stack_int_t *stack = data->stack;
    const igraph_vector_int_t *map = data->map;
    igraph_integer_t realvid = VECTOR(*map)[vid];

    IGRAPH_UNUSED(graph); IGRAPH_UNUSED(dist);

    if (!igraph_stack_int_empty(stack) &&
        igraph_stack_int_top(stack) == realvid) {
        igraph_stack_int_pop(stack);
    }

    return IGRAPH_SUCCESS;
}

static igraph_error_t igraph_i_all_st_cuts_minimal(const igraph_t *graph,
                                        const igraph_t *domtree,
                                        igraph_integer_t root,
                                        const igraph_marked_queue_int_t *X,
                                        const igraph_vector_bool_t *GammaX,
                                        const igraph_vector_int_t *invmap,
                                        igraph_vector_int_t *minimal) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_stack_int_t stack;
    igraph_vector_bool_t nomark;
    igraph_i_all_st_cuts_minimal_dfs_data_t data;
    igraph_integer_t i;

    IGRAPH_UNUSED(X);

    IGRAPH_STACK_INT_INIT_FINALLY(&stack, 10);
    IGRAPH_VECTOR_BOOL_INIT_FINALLY(&nomark, no_of_nodes);

    data.stack = &stack;
    data.nomark = &nomark;
    data.GammaX = GammaX;
    data.root = root;
    data.map = invmap;

    /* We mark all GammaX elements as minimal first.
       TODO: actually, we could just use GammaX to return the minimal
       elements. */
    for (i = 0; i < no_of_nodes; i++) {
        VECTOR(nomark)[i] = (VECTOR(*GammaX)[i] == 0);
    }

    /* We do a reverse DFS from root. If, along a path we find a GammaX
       vertex after (=below) another GammaX vertex, we mark the higher
       one as non-minimal. */

    IGRAPH_CHECK(igraph_dfs(domtree, root, IGRAPH_IN,
                            /*unreachable=*/ 0, /*order=*/ 0,
                            /*order_out=*/ 0, /*father=*/ 0,
                            /*dist=*/ 0, /*in_callback=*/
                            igraph_i_all_st_cuts_minimal_dfs_incb,
                            /*out_callback=*/
                            igraph_i_all_st_cuts_minimal_dfs_outcb,
                            /*extra=*/ &data));

    igraph_vector_int_clear(minimal);
    for (i = 0; i < no_of_nodes; i++) {
        if (!VECTOR(nomark)[i]) {
            IGRAPH_CHECK(igraph_vector_int_push_back(minimal, i));
        }
    }

    igraph_vector_bool_destroy(&nomark);
    igraph_stack_int_destroy(&stack);
    IGRAPH_FINALLY_CLEAN(2);

    return IGRAPH_SUCCESS;
}

/* not 'static' because used in igraph_all_st_cuts.c test program */
igraph_error_t igraph_i_all_st_cuts_pivot(
    const igraph_t *graph, const igraph_marked_queue_int_t *S,
    const igraph_estack_t *T, igraph_integer_t source, igraph_integer_t target,
    igraph_integer_t *v, igraph_vector_int_t *Isv, void *arg
) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_t Sbar;
    igraph_vector_int_t Sbar_map, Sbar_invmap;
    igraph_vector_int_t keep;
    igraph_t domtree;
    igraph_vector_int_t leftout;
    igraph_integer_t i, nomin, n;
    igraph_integer_t root;
    igraph_vector_int_t M;
    igraph_vector_bool_t GammaS;
    igraph_vector_int_t Nuv;
    igraph_vector_int_t Isv_min;
    igraph_vector_int_t GammaS_vec;
    igraph_integer_t Sbar_size;

    IGRAPH_UNUSED(arg);

    /* We need to create the graph induced by Sbar */
    IGRAPH_VECTOR_INT_INIT_FINALLY(&Sbar_map, 0);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&Sbar_invmap, 0);

    IGRAPH_VECTOR_INT_INIT_FINALLY(&keep, 0);
    for (i = 0; i < no_of_nodes; i++) {
        if (!igraph_marked_queue_int_iselement(S, i)) {
            IGRAPH_CHECK(igraph_vector_int_push_back(&keep, i));
        }
    }
    Sbar_size = igraph_vector_int_size(&keep);

    IGRAPH_CHECK(igraph_induced_subgraph_map(graph, &Sbar,
                 igraph_vss_vector(&keep),
                 IGRAPH_SUBGRAPH_AUTO,
                 /* map= */ &Sbar_map,
                 /* invmap= */ &Sbar_invmap));
    igraph_vector_int_destroy(&keep);
    IGRAPH_FINALLY_CLEAN(1);
    IGRAPH_FINALLY(igraph_destroy, &Sbar);

    root = VECTOR(Sbar_map)[target] - 1;

    /* -------------------------------------------------------------*/
    /* Construct the dominator tree of Sbar */

    IGRAPH_VECTOR_INT_INIT_FINALLY(&leftout, 0);
    IGRAPH_CHECK(igraph_dominator_tree(&Sbar, root,
                                       /*dom=*/ 0, &domtree,
                                       &leftout, IGRAPH_IN));
    IGRAPH_FINALLY(igraph_destroy, &domtree);

    /* -------------------------------------------------------------*/
    /* Identify the set M of minimal elements of Gamma(S) with respect
       to the dominator relation. */

    /* First we create GammaS */
    /* TODO: use the adjacency list, instead of neighbors() */
    IGRAPH_CHECK(igraph_vector_bool_init(&GammaS, no_of_nodes));
    IGRAPH_FINALLY(igraph_vector_bool_destroy, &GammaS);
    if (igraph_marked_queue_int_size(S) == 0) {
        VECTOR(GammaS)[VECTOR(Sbar_map)[source] - 1] = 1;
    } else {
        for (i = 0; i < no_of_nodes; i++) {
            if (igraph_marked_queue_int_iselement(S, i)) {
                igraph_vector_int_t neis;
                igraph_integer_t j;
                IGRAPH_VECTOR_INT_INIT_FINALLY(&neis, 0);
                IGRAPH_CHECK(igraph_neighbors(graph, &neis, i,
                                              IGRAPH_OUT));
                n = igraph_vector_int_size(&neis);
                for (j = 0; j < n; j++) {
                    igraph_integer_t nei = VECTOR(neis)[j];
                    if (!igraph_marked_queue_int_iselement(S, nei)) {
                        VECTOR(GammaS)[nei] = 1;
                    }
                }
                igraph_vector_int_destroy(&neis);
                IGRAPH_FINALLY_CLEAN(1);
            }
        }
    }

    /* Relabel left out vertices (set K in Provan & Shier) to
       correspond to node labelling of graph instead of SBar.
       At the same time ensure that GammaS is a proper subset of
       L, where L are the nodes in the dominator tree. */
    n = igraph_vector_int_size(&leftout);
    for (i = 0; i < n; i++) {
        VECTOR(leftout)[i] = VECTOR(Sbar_invmap)[VECTOR(leftout)[i]];
        VECTOR(GammaS)[VECTOR(leftout)[i]] = 0;
    }

    IGRAPH_VECTOR_INT_INIT_FINALLY(&M, 0);
    if (igraph_ecount(&domtree) > 0) {
        IGRAPH_CHECK(igraph_i_all_st_cuts_minimal(graph, &domtree, root, S,
                     &GammaS, &Sbar_invmap, &M));
    }

    igraph_vector_int_clear(Isv);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&Nuv, 0);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&Isv_min, 0);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&GammaS_vec, 0);
    for (i = 0; i < no_of_nodes; i++) {
        if (VECTOR(GammaS)[i]) {
            IGRAPH_CHECK(igraph_vector_int_push_back(&GammaS_vec, i));
        }
    }

    nomin = igraph_vector_int_size(&M);
    for (i = 0; i < nomin; i++) {
        /* -------------------------------------------------------------*/
        /* For each v in M find the set Nu(v)=dom(Sbar, v)-K
           Nu(v) contains all vertices that are dominated by v, for every
           v, this is a subtree of the dominator tree, rooted at v. The
           different subtrees are disjoint. */
        igraph_integer_t min = VECTOR(Sbar_map)[ VECTOR(M)[i] ] - 1;
        igraph_integer_t nuvsize, isvlen, j;
        IGRAPH_CHECK(igraph_dfs(&domtree, min, IGRAPH_IN,
                                /*unreachable=*/ 0, /*order=*/ &Nuv,
                                /*order_out=*/ 0, /*father=*/ 0, /*dist=*/ 0,
                                /*in_callback=*/ 0, /*out_callback=*/ 0,
                                /*extra=*/ 0));
        /* Remove the negative values from the end of the vector */
        for (nuvsize = 0; nuvsize < Sbar_size; nuvsize++) {
            igraph_integer_t t = VECTOR(Nuv)[nuvsize];
            if (t >= 0) {
                VECTOR(Nuv)[nuvsize] = VECTOR(Sbar_invmap)[t];
            } else {
                break;
            }
        }
        igraph_vector_int_resize(&Nuv, nuvsize); /* shrinks, error safe */

        /* -------------------------------------------------------------*/
        /* By a BFS search of <Nu(v)> determine I(S,v)-K.
           I(S,v) contains all vertices that are in Nu(v) and that are
           reachable from Gamma(S) via a path in Nu(v). */
        IGRAPH_CHECK(igraph_bfs(graph, /*root=*/ -1, /*roots=*/ &GammaS_vec,
                                /*mode=*/ IGRAPH_OUT, /*unreachable=*/ 0,
                                /*restricted=*/ &Nuv,
                                /*order=*/ &Isv_min, /*rank=*/ 0,
                                /*father=*/ 0, /*pred=*/ 0, /*succ=*/ 0,
                                /*dist=*/ 0, /*callback=*/ 0, /*extra=*/ 0));
        for (isvlen = 0; isvlen < no_of_nodes; isvlen++) {
            if (VECTOR(Isv_min)[isvlen] < 0) {
                break;
            }
        }
        igraph_vector_int_resize(&Isv_min, isvlen);

        /* -------------------------------------------------------------*/
        /* For each c in M check whether Isv-K is included in Tbar. If
           such a v is found, compute Isv={x|v[Nu(v) U K]x} and return v and
           Isv; otherwise return Isv={}. */
        for (j = 0; j < isvlen; j++) {
            igraph_integer_t u = VECTOR(Isv_min)[j];
            if (igraph_estack_iselement(T, u) || u == target) {
                break;
            }
        }
        /* We might have found one */
        if (j == isvlen) {
            *v = VECTOR(M)[i];
            /* Calculate real Isv */
            IGRAPH_CHECK(igraph_vector_int_append(&Nuv, &leftout));
            IGRAPH_CHECK(igraph_bfs(graph, /*root=*/ *v,
                                    /*roots=*/ 0, /*mode=*/ IGRAPH_OUT,
                                    /*unreachable=*/ 0, /*restricted=*/ &Nuv,
                                    /*order=*/ &Isv_min, /*rank=*/ 0,
                                    /*father=*/ 0, /*pred=*/ 0, /*succ=*/ 0,
                                    /*dist=*/ 0, /*callback=*/ 0, /*extra=*/ 0));
            for (isvlen = 0; isvlen < no_of_nodes; isvlen++) {
                if (VECTOR(Isv_min)[isvlen] < 0) {
                    break;
                }
            }
            igraph_vector_int_resize(&Isv_min, isvlen);
            igraph_vector_int_update(Isv, &Isv_min);

            break;
        }
    }

    igraph_vector_int_destroy(&GammaS_vec);
    igraph_vector_int_destroy(&Isv_min);
    igraph_vector_int_destroy(&Nuv);
    IGRAPH_FINALLY_CLEAN(3);

    igraph_vector_int_destroy(&M);
    igraph_vector_bool_destroy(&GammaS);
    igraph_destroy(&domtree);
    igraph_vector_int_destroy(&leftout);
    igraph_destroy(&Sbar);
    igraph_vector_int_destroy(&Sbar_map);
    igraph_vector_int_destroy(&Sbar_invmap);
    IGRAPH_FINALLY_CLEAN(7);

    return IGRAPH_SUCCESS;
}

/* TODO: This is a temporary recursive version */

igraph_error_t igraph_provan_shier_list(
    const igraph_t *graph, igraph_marked_queue_int_t *S,
    igraph_estack_t *T, igraph_integer_t source, igraph_integer_t target,
    igraph_vector_int_list_t *result, igraph_provan_shier_pivot_t *pivot,
    void *pivot_arg
) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_vector_int_t Isv;
    igraph_vector_int_t vec;
    igraph_integer_t v = 0;
    igraph_integer_t i, n;

    IGRAPH_VECTOR_INT_INIT_FINALLY(&Isv, 0);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&vec, 0);

    pivot(graph, S, T, source, target, &v, &Isv, pivot_arg);

    if (igraph_vector_int_empty(&Isv)) {
        if (igraph_marked_queue_int_size(S) != 0 && igraph_marked_queue_int_size(S) != no_of_nodes) {
            IGRAPH_CHECK(igraph_marked_queue_int_as_vector(S, &vec));
            IGRAPH_CHECK(igraph_vector_int_list_push_back_copy(result, &vec));
        }
    } else {
        /* Put v into T */
        IGRAPH_CHECK(igraph_estack_push(T, v));

        /* Go down left in the search tree */
        IGRAPH_CHECK(igraph_provan_shier_list(
            graph, S, T, source, target, result, pivot, pivot_arg));

        /* Take out v from T */
        igraph_estack_pop(T);

        /* Add Isv to S */
        IGRAPH_CHECK(igraph_marked_queue_int_start_batch(S));
        n = igraph_vector_int_size(&Isv);
        for (i = 0; i < n; i++) {
            if (!igraph_marked_queue_int_iselement(S, VECTOR(Isv)[i])) {
                IGRAPH_CHECK(igraph_marked_queue_int_push(S, VECTOR(Isv)[i]));
            }
        }

        /* Go down right in the search tree */

        IGRAPH_CHECK(igraph_provan_shier_list(
            graph, S, T, source, target, result, pivot, pivot_arg));

        /* Take out Isv from S */
        igraph_marked_queue_int_pop_back_batch(S);
    }

    igraph_vector_int_destroy(&vec);
    igraph_vector_int_destroy(&Isv);
    IGRAPH_FINALLY_CLEAN(2);

    return IGRAPH_SUCCESS;
}

/**
 * \function igraph_all_st_cuts
 * List all edge-cuts between two vertices in a directed graph
 *
 * This function lists all edge-cuts between a source and a target
 * vertex. Every cut is listed exactly once. The implemented algorithm
 * is described in JS Provan and DR Shier: A Paradigm for listing
 * (s,t)-cuts in graphs, Algorithmica 15, 351--372, 1996.
 *
 * \param graph The input graph, is must be directed.
 * \param cuts An initialized list of integer vectors, the cuts are stored
 *        here. Each vector will contain the IDs of the edges in
 *        the cut. This argument is ignored if it is a null pointer.
 * \param partition1s An initialized list of integer vectors, the list of
 *        vertex sets generating the actual edge cuts are stored
 *        here. Each vector contains a set of vertex IDs. If X is such
 *        a set, then all edges going from X to the complement of X
 *        form an (s, t) edge-cut in the graph. This argument is
 *        ignored if it is a null pointer.
 * \param source The id of the source vertex.
 * \param target The id of the target vertex.
 * \return Error code.
 *
 * Time complexity: O(n(|V|+|E|)), where |V| is the number of
 * vertices, |E| is the number of edges, and n is the number of cuts.
 */

igraph_error_t igraph_all_st_cuts(const igraph_t *graph,
                       igraph_vector_int_list_t *cuts,
                       igraph_vector_int_list_t *partition1s,
                       igraph_integer_t source,
                       igraph_integer_t target) {

    /* S is a special stack, in which elements are pushed in batches.
       It is then possible to remove the whole batch in one step.

       T is a stack with an is-element operation.
       Every element is included at most once.
    */

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_integer_t no_of_edges = igraph_ecount(graph);
    igraph_marked_queue_int_t S;
    igraph_estack_t T;
    igraph_vector_int_list_t *mypartition1s = partition1s, vpartition1s;
    igraph_vector_int_t cut;
    igraph_integer_t i, nocuts;

    if (!igraph_is_directed(graph)) {
        IGRAPH_ERROR("Listing all s-t cuts only implemented for "
                     "directed graphs", IGRAPH_UNIMPLEMENTED);
    }

    if (!partition1s) {
        mypartition1s = &vpartition1s;
        IGRAPH_CHECK(igraph_vector_int_list_init(mypartition1s, 0));
        IGRAPH_FINALLY(igraph_vector_int_list_destroy, mypartition1s);
    } else {
        igraph_vector_int_list_clear(mypartition1s);
    }

    IGRAPH_CHECK(igraph_marked_queue_int_init(&S, no_of_nodes));
    IGRAPH_FINALLY(igraph_marked_queue_int_destroy, &S);
    IGRAPH_CHECK(igraph_estack_init(&T, no_of_nodes, 0));
    IGRAPH_FINALLY(igraph_estack_destroy, &T);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&cut, 0);

    /* We call it with S={}, T={} */
    IGRAPH_CHECK(igraph_provan_shier_list(graph, &S, &T,
                                          source, target, mypartition1s,
                                          igraph_i_all_st_cuts_pivot,
                                          /*pivot_arg=*/ 0));

    nocuts = igraph_vector_int_list_size(mypartition1s);

    if (cuts) {
        igraph_vector_int_t inS;
        IGRAPH_CHECK(igraph_vector_int_init(&inS, no_of_nodes));
        IGRAPH_FINALLY(igraph_vector_int_destroy, &inS);
        igraph_vector_int_list_clear(cuts);
        IGRAPH_CHECK(igraph_vector_int_list_reserve(cuts, nocuts));
        for (i = 0; i < nocuts; i++) {
            igraph_vector_int_t *part = igraph_vector_int_list_get_ptr(mypartition1s, i);
            igraph_integer_t cutsize = 0;
            igraph_integer_t j, partlen = igraph_vector_int_size(part);
            /* Mark elements */
            for (j = 0; j < partlen; j++) {
                igraph_integer_t v = VECTOR(*part)[j];
                VECTOR(inS)[v] = i + 1;
            }
            /* Check how many edges */
            for (j = 0; j < no_of_edges; j++) {
                igraph_integer_t from = IGRAPH_FROM(graph, j);
                igraph_integer_t to = IGRAPH_TO(graph, j);
                igraph_integer_t pfrom = VECTOR(inS)[from];
                igraph_integer_t pto = VECTOR(inS)[to];
                if (pfrom == i + 1 && pto != i + 1) {
                    cutsize++;
                }
            }
            /* Add the edges */
            IGRAPH_CHECK(igraph_vector_int_resize(&cut, cutsize));
            cutsize = 0;
            for (j = 0; j < no_of_edges; j++) {
                igraph_integer_t from = IGRAPH_FROM(graph, j);
                igraph_integer_t to = IGRAPH_TO(graph, j);
                igraph_integer_t pfrom = VECTOR(inS)[from];
                igraph_integer_t pto = VECTOR(inS)[to];
                if ((pfrom == i + 1 && pto != i + 1)) {
                    VECTOR(cut)[cutsize++] = j;
                }
            }
            /* Add the vector to 'cuts' */
            IGRAPH_CHECK(igraph_vector_int_list_push_back_copy(cuts, &cut));
        }

        igraph_vector_int_destroy(&inS);
        IGRAPH_FINALLY_CLEAN(1);
    }

    igraph_vector_int_destroy(&cut);
    igraph_estack_destroy(&T);
    igraph_marked_queue_int_destroy(&S);
    IGRAPH_FINALLY_CLEAN(3);

    if (!partition1s) {
        igraph_vector_int_list_destroy(mypartition1s);
        IGRAPH_FINALLY_CLEAN(1);
    }

    return IGRAPH_SUCCESS;
}

/* We need to find the minimal active elements of Sbar. I.e. all
   active Sbar elements 'v', s.t. there is no other 'w' active Sbar
   element from which 'v' is reachable. (Not necessarily through
   active vertices.)

   We calculate the in-degree of all vertices in Sbar first. Then we
   look at the vertices with zero in-degree. If these are active,
   then they are minimal. If they are are not active, then we remove
   them from the graph, and check whether they resulted in more
   zero-indegree vertices.
*/

static igraph_error_t igraph_i_all_st_mincuts_minimal(const igraph_t *Sbar,
                                           const igraph_vector_bool_t *active,
                                           const igraph_vector_int_t *invmap,
                                           igraph_vector_int_t *minimal) {

    igraph_integer_t no_of_nodes = igraph_vcount(Sbar);
    igraph_vector_int_t indeg;
    igraph_integer_t i, minsize;
    igraph_vector_int_t neis;

    IGRAPH_VECTOR_INT_INIT_FINALLY(&neis, 0);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&indeg, no_of_nodes);

    IGRAPH_CHECK(igraph_degree(Sbar, &indeg, igraph_vss_all(),
                               IGRAPH_IN, /*loops=*/ 1));

#define ACTIVE(x) (VECTOR(*active)[VECTOR(*invmap)[(x)]])
#define ZEROIN(x) (VECTOR(indeg)[(x)]==0)

    for (i = 0; i < no_of_nodes; i++) {
        if (!ACTIVE(i)) {
            igraph_integer_t j, n;
            IGRAPH_CHECK(igraph_neighbors(Sbar, &neis, i, IGRAPH_OUT));
            n = igraph_vector_int_size(&neis);
            for (j = 0; j < n; j++) {
                igraph_integer_t nei = VECTOR(neis)[j];
                VECTOR(indeg)[nei] -= 1;
            }
        }
    }

    for (minsize = 0, i = 0; i < no_of_nodes; i++) {
        if (ACTIVE(i) && ZEROIN(i)) {
            minsize++;
        }
    }

    IGRAPH_CHECK(igraph_vector_int_resize(minimal, minsize));

    for (minsize = 0, i = 0; i < no_of_nodes; i++) {
        if (ACTIVE(i) && ZEROIN(i)) {
            VECTOR(*minimal)[minsize++] = i;
        }
    }

#undef ACTIVE
#undef ZEROIN

    igraph_vector_int_destroy(&indeg);
    igraph_vector_int_destroy(&neis);
    IGRAPH_FINALLY_CLEAN(2);

    return IGRAPH_SUCCESS;
}

typedef struct igraph_i_all_st_mincuts_data_t {
    const igraph_vector_bool_t *active;
} igraph_i_all_st_mincuts_data_t;

static igraph_error_t igraph_i_all_st_mincuts_pivot(const igraph_t *graph,
                                         const igraph_marked_queue_int_t *S,
                                         const igraph_estack_t *T,
                                         igraph_integer_t source,
                                         igraph_integer_t target,
                                         igraph_integer_t *v,
                                         igraph_vector_int_t *Isv,
                                         void *arg) {

    igraph_i_all_st_mincuts_data_t *data = arg;
    const igraph_vector_bool_t *active = data->active;

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_integer_t i, j;
    igraph_vector_int_t Sbar_map, Sbar_invmap;
    igraph_vector_int_t keep;
    igraph_t Sbar;
    igraph_vector_int_t M;
    igraph_integer_t nomin;

    IGRAPH_UNUSED(source); IGRAPH_UNUSED(target);

    if (igraph_marked_queue_int_size(S) == no_of_nodes) {
        igraph_vector_int_clear(Isv);
        return IGRAPH_SUCCESS;
    }

    /* Create the graph induced by Sbar */
    IGRAPH_VECTOR_INT_INIT_FINALLY(&Sbar_map, 0);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&Sbar_invmap, 0);

    IGRAPH_VECTOR_INT_INIT_FINALLY(&keep, 0);
    for (i = 0; i < no_of_nodes; i++) {
        if (!igraph_marked_queue_int_iselement(S, i)) {
            IGRAPH_CHECK(igraph_vector_int_push_back(&keep, i));
        }
    }

    /* TODO: it is not even necessary to create Sbar explicitly, we
       just need to find the M elements efficiently. See the
       Provan-Shier paper for details. */
    IGRAPH_CHECK(igraph_induced_subgraph_map(graph, &Sbar,
                 igraph_vss_vector(&keep),
                 IGRAPH_SUBGRAPH_AUTO,
                 /* map= */ &Sbar_map,
                 /* invmap= */ &Sbar_invmap));
    IGRAPH_FINALLY(igraph_destroy, &Sbar);

    /* ------------------------------------------------------------- */
    /* Identify the set M of minimal elements that are active */
    IGRAPH_VECTOR_INT_INIT_FINALLY(&M, 0);
    IGRAPH_CHECK(igraph_i_all_st_mincuts_minimal(&Sbar, active,
                 &Sbar_invmap, &M));

    /* ------------------------------------------------------------- */
    /* Now find a minimal element that is not in T */
    igraph_vector_int_clear(Isv);
    nomin = igraph_vector_int_size(&M);
    for (i = 0; i < nomin; i++) {
        igraph_integer_t min = VECTOR(Sbar_invmap)[ VECTOR(M)[i] ];
        if (min != target)
            if (!igraph_estack_iselement(T, min)) {
                break;
            }
    }
    if (i != nomin) {
        /* OK, we found a pivot element. I(S,v) contains all elements
           that can reach the pivot element */
        igraph_vector_int_t Isv_min;
        IGRAPH_VECTOR_INT_INIT_FINALLY(&Isv_min, 0);
        *v = VECTOR(Sbar_invmap)[ VECTOR(M)[i] ];
        /* TODO: restricted == keep ? */
        IGRAPH_CHECK(igraph_bfs(graph, /*root=*/ *v,/*roots=*/ 0,
                                /*mode=*/ IGRAPH_IN, /*unreachable=*/ 0,
                                /*restricted=*/ &keep, /*order=*/ &Isv_min,
                                /*rank=*/ 0, /*father=*/ 0, /*pred=*/ 0,
                                /*succ=*/ 0, /*dist=*/ 0, /*callback=*/ 0,
                                /*extra=*/ 0));
        for (j = 0; j < no_of_nodes; j++) {
            igraph_integer_t u = VECTOR(Isv_min)[j];
            if (u < 0) {
                break;
            }
            if (!igraph_estack_iselement(T, u)) {
                IGRAPH_CHECK(igraph_vector_int_push_back(Isv, u));
            }
        }
        igraph_vector_int_destroy(&Isv_min);
        IGRAPH_FINALLY_CLEAN(1);
    }

    igraph_vector_int_destroy(&M);
    igraph_destroy(&Sbar);
    igraph_vector_int_destroy(&keep);
    igraph_vector_int_destroy(&Sbar_invmap);
    igraph_vector_int_destroy(&Sbar_map);
    IGRAPH_FINALLY_CLEAN(5);

    return IGRAPH_SUCCESS;
}

/**
 * \function igraph_all_st_mincuts
 * All minimum s-t cuts of a directed graph
 *
 * This function lists all edge cuts between two vertices, in a directed graph,
 * with minimum total capacity. Possibly, multiple cuts may have the same total
 * capacity, although there is often only one minimum cut in weighted graphs.
 * It is recommended to supply integer-values capacities. Otherwise, not all
 * minimum cuts may be detected because of numerical roundoff errors.
 * The implemented algorithm is described in JS Provan and DR
 * Shier: A Paradigm for listing (s,t)-cuts in graphs, Algorithmica 15,
 * 351--372, 1996.
 *
 * \param graph The input graph, it must be directed.
 * \param value Pointer to a real number, the value of the minimum cut
 *        is stored here, unless it is a null pointer.
 * \param cuts An initialized pointer vector, the cuts are stored
 *        here. It is a list of pointers to \ref igraph_vector_int_t
 *        objects. Each vector will contain the IDs of the edges in
 *        the cut. This argument is ignored if it is a null pointer.
 *        To free all memory allocated for \c cuts, you need call
 *        \ref igraph_vector_int_destroy() and then \ref igraph_free() on
 *        each element, before destroying the pointer vector itself.
 * \param partition1s An initialized pointer vector, the list of
 *        vertex sets, generating the actual edge cuts, are stored
 *        here. It is a list of pointers to \ref igraph_vector_int_t
 *        objects. Each vector contains a set of vertex IDs. If X is such
 *        a set, then all edges going from X to the complement of X
 *        form an (s,t) edge-cut in the graph. This argument is
 *        ignored if it is a null pointer.
 * \param source The id of the source vertex.
 * \param target The id of the target vertex.
 * \param capacity Vector of edge capacities. All capacities must be
 *        strictly positive. If this is a null pointer, then all edges
 *        are assumed to have capacity one.
 * \return Error code.
 *
 * Time complexity: O(n(|V|+|E|))+O(F), where |V| is the number of
 * vertices, |E| is the number of edges, and n is the number of cuts;
 * O(F) is the time complexity of the maximum flow algorithm, see \ref
 * igraph_maxflow().
 *
 * \example examples/simple/igraph_all_st_mincuts.c
 */

igraph_error_t igraph_all_st_mincuts(const igraph_t *graph, igraph_real_t *value,
                          igraph_vector_int_list_t *cuts,
                          igraph_vector_int_list_t *partition1s,
                          igraph_integer_t source,
                          igraph_integer_t target,
                          const igraph_vector_t *capacity) {

    igraph_integer_t no_of_nodes = igraph_vcount(graph);
    igraph_integer_t no_of_edges = igraph_ecount(graph);
    igraph_vector_t flow;
    igraph_t residual;
    igraph_vector_int_t NtoL;
    igraph_vector_int_t cut;
    igraph_integer_t newsource, newtarget;
    igraph_marked_queue_int_t S;
    igraph_estack_t T;
    igraph_i_all_st_mincuts_data_t pivot_data;
    igraph_vector_bool_t VE1bool;
    igraph_vector_t VE1;
    igraph_integer_t VE1size = 0;
    igraph_integer_t i, nocuts;
    igraph_integer_t proj_nodes;
    igraph_vector_t revmap_ptr, revmap_next;
    igraph_vector_int_list_t closedsets;
    igraph_vector_int_list_t *mypartition1s = partition1s, vpartition1s;
    igraph_maxflow_stats_t stats;

    /* -------------------------------------------------------------------- */
    /* Error checks */
    if (!igraph_is_directed(graph)) {
        IGRAPH_ERROR("S-t cuts can only be listed in directed graphs",
                     IGRAPH_UNIMPLEMENTED);
    }
    if (source < 0 || source >= no_of_nodes) {
        IGRAPH_ERROR("Invalid `source' vertex", IGRAPH_EINVAL);
    }
    if (target < 0 || target >= no_of_nodes) {
        IGRAPH_ERROR("Invalid `target' vertex", IGRAPH_EINVAL);
    }
    if (source == target) {
        IGRAPH_ERROR("`source' and 'target' are the same vertex", IGRAPH_EINVAL);
    }
    if (capacity != NULL && igraph_vector_min(capacity) <= 0)
    {
        IGRAPH_ERROR("Not all capacities are strictly positive.", IGRAPH_EINVAL);
    }

    if (!partition1s) {
        mypartition1s = &vpartition1s;
        IGRAPH_CHECK(igraph_vector_int_list_init(mypartition1s, 0));
        IGRAPH_FINALLY(igraph_vector_int_list_destroy, mypartition1s);
    }

    /* -------------------------------------------------------------------- */
    /* We need to calculate the maximum flow first */
    IGRAPH_VECTOR_INIT_FINALLY(&flow, 0);
    IGRAPH_CHECK(igraph_maxflow(graph, value, &flow, /*cut=*/ 0,
                                /*partition1=*/ 0, /*partition2=*/ 0,
                                /*source=*/ source, /*target=*/ target,
                                capacity, &stats));

    /* -------------------------------------------------------------------- */
    /* Then we need the reverse residual graph */
    IGRAPH_CHECK(igraph_reverse_residual_graph(graph, capacity, &residual,
                 &flow));
    IGRAPH_FINALLY(igraph_destroy, &residual);

    /* -------------------------------------------------------------------- */
    /* We shrink it to its strongly connected components */
    IGRAPH_VECTOR_INT_INIT_FINALLY(&NtoL, 0);
    IGRAPH_CHECK(igraph_connected_components(
        &residual, /*membership=*/ &NtoL, /*csize=*/ 0,
        /*no=*/ &proj_nodes, IGRAPH_STRONG
    ));
    IGRAPH_CHECK(igraph_contract_vertices(&residual, /*mapping=*/ &NtoL,
                                          /*vertex_comb=*/ 0));
    IGRAPH_CHECK(igraph_simplify(&residual, /*multiple=*/ true, /*loops=*/ true,
                                 /*edge_comb=*/ NULL));

    newsource = VECTOR(NtoL)[source];
    newtarget = VECTOR(NtoL)[target];

    /* TODO: handle the newsource == newtarget case */

    /* -------------------------------------------------------------------- */
    /* Determine the active vertices in the projection */
    IGRAPH_VECTOR_INIT_FINALLY(&VE1, 0);
    IGRAPH_CHECK(igraph_vector_bool_init(&VE1bool, proj_nodes));
    IGRAPH_FINALLY(igraph_vector_bool_destroy, &VE1bool);
    for (i = 0; i < no_of_edges; i++) {
        if (VECTOR(flow)[i] > 0) {
            igraph_integer_t from = IGRAPH_FROM(graph, i);
            igraph_integer_t to = IGRAPH_TO(graph, i);
            igraph_integer_t pfrom = VECTOR(NtoL)[from];
            igraph_integer_t pto = VECTOR(NtoL)[to];
            if (!VECTOR(VE1bool)[pfrom]) {
                VECTOR(VE1bool)[pfrom] = 1;
                VE1size++;
            }
            if (!VECTOR(VE1bool)[pto]) {
                VECTOR(VE1bool)[pto] = 1;
                VE1size++;
            }
        }
    }
    IGRAPH_CHECK(igraph_vector_reserve(&VE1, VE1size));
    for (i = 0; i < proj_nodes; i++) {
        if (VECTOR(VE1bool)[i]) {
            igraph_vector_push_back(&VE1, i);
        }
    }

    if (cuts)        {
        igraph_vector_int_list_clear(cuts);
    }
    if (partition1s) {
        igraph_vector_int_list_clear(partition1s);
    }

    /* -------------------------------------------------------------------- */
    /* Everything is ready, list the cuts, using the right PIVOT
       function  */
    IGRAPH_CHECK(igraph_marked_queue_int_init(&S, no_of_nodes));
    IGRAPH_FINALLY(igraph_marked_queue_int_destroy, &S);
    IGRAPH_CHECK(igraph_estack_init(&T, no_of_nodes, 0));
    IGRAPH_FINALLY(igraph_estack_destroy, &T);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&cut, 0);

    pivot_data.active = &VE1bool;

    IGRAPH_VECTOR_INT_LIST_INIT_FINALLY(&closedsets, 0);
    IGRAPH_CHECK(igraph_provan_shier_list(&residual, &S, &T,
                                          newsource, newtarget, &closedsets,
                                          igraph_i_all_st_mincuts_pivot,
                                          &pivot_data));

    /* Convert the closed sets in the contracted graphs to cutsets in the
       original graph */
    IGRAPH_VECTOR_INIT_FINALLY(&revmap_ptr, igraph_vcount(&residual));
    IGRAPH_VECTOR_INIT_FINALLY(&revmap_next, no_of_nodes);
    for (i = 0; i < no_of_nodes; i++) {
        igraph_integer_t id = VECTOR(NtoL)[i];
        VECTOR(revmap_next)[i] = VECTOR(revmap_ptr)[id];
        VECTOR(revmap_ptr)[id] = i + 1;
    }

    /* Create partitions in original graph */
    nocuts = igraph_vector_int_list_size(&closedsets);
    igraph_vector_int_list_clear(mypartition1s);
    IGRAPH_CHECK(igraph_vector_int_list_reserve(mypartition1s, nocuts));
    for (i = 0; i < nocuts; i++) {
        igraph_vector_int_t *supercut = igraph_vector_int_list_get_ptr(&closedsets, i);
        igraph_integer_t j, supercutsize = igraph_vector_int_size(supercut);

        igraph_vector_int_clear(&cut);
        for (j = 0; j < supercutsize; j++) {
            igraph_integer_t vtx = VECTOR(*supercut)[j];
            igraph_integer_t ovtx = VECTOR(revmap_ptr)[vtx];
            while (ovtx != 0) {
                ovtx--;
                IGRAPH_CHECK(igraph_vector_int_push_back(&cut, ovtx));
                ovtx = VECTOR(revmap_next)[ovtx];
            }
        }

        IGRAPH_CHECK(igraph_vector_int_list_push_back_copy(mypartition1s, &cut));

        /* TODO: we could already reclaim the memory taken by 'supercut' here */
    }

    igraph_vector_destroy(&revmap_next);
    igraph_vector_destroy(&revmap_ptr);
    igraph_vector_int_list_destroy(&closedsets);
    IGRAPH_FINALLY_CLEAN(3);

    /* Create cuts in original graph */
    if (cuts) {
        igraph_vector_int_t memb;

        IGRAPH_VECTOR_INT_INIT_FINALLY(&memb, no_of_nodes);
        IGRAPH_CHECK(igraph_vector_int_list_reserve(cuts, nocuts));

        for (i = 0; i < nocuts; i++) {
            igraph_vector_int_t *part = igraph_vector_int_list_get_ptr(mypartition1s, i);
            igraph_integer_t j, n = igraph_vector_int_size(part);

            igraph_vector_int_clear(&cut);
            for (j = 0; j < n; j++) {
                igraph_integer_t vtx = VECTOR(*part)[j];
                VECTOR(memb)[vtx] = i + 1;
            }
            for (j = 0; j < no_of_edges; j++) {
                if (VECTOR(flow)[j] > 0) {
                    igraph_integer_t from = IGRAPH_FROM(graph, j);
                    igraph_integer_t to = IGRAPH_TO(graph, j);
                    if (VECTOR(memb)[from] == i + 1 && VECTOR(memb)[to] != i + 1) {
                        IGRAPH_CHECK(igraph_vector_int_push_back(&cut, j));
                    }
                }
            }

            IGRAPH_CHECK(igraph_vector_int_list_push_back_copy(cuts, &cut));
        }

        igraph_vector_int_destroy(&memb);
        IGRAPH_FINALLY_CLEAN(1);
    }

    igraph_vector_int_destroy(&cut);
    igraph_estack_destroy(&T);
    igraph_marked_queue_int_destroy(&S);
    igraph_vector_bool_destroy(&VE1bool);
    igraph_vector_destroy(&VE1);
    igraph_vector_int_destroy(&NtoL);
    igraph_destroy(&residual);
    igraph_vector_destroy(&flow);
    IGRAPH_FINALLY_CLEAN(8);

    if (!partition1s) {
        igraph_vector_int_list_destroy(mypartition1s);
        IGRAPH_FINALLY_CLEAN(1);
    }

    return IGRAPH_SUCCESS;
}
