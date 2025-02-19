/* Parallel Computing Project S2019
 * Eric Johnson, Chris Jones, Harrison Lee
 */

#include <mpi.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <vector>
#include <zoltan.h>

#include "data-structures.h"
#include "pthread-wrappers.h"

using namespace std;

#ifdef __bgq__
#include <hwi/include/bqc/A2_inlines.h>
#else
#define GetTimeBase MPI_Wtime
#endif

#ifdef DEBUG_MODE
#define DEBUG(lvl, fmt, args...)                                               \
  do {                                                                         \
    if ((lvl) <= DEBUG_MODE)                                                   \
      fprintf(                                                                 \
          stderr, "%9.5f DEBUG: %15s():%-4d R%dT%d: " fmt "\n",                \
          ((double)(GetTimeBase() - g_start_cycles) / g_processor_frequency),  \
          __func__, __LINE__, mpi_rank, tid, ##args);                          \
  } while (0)
#define ERROR(fmt, args...)                                                    \
  fprintf(stderr, "ERROR: %15s():%-4d R%dT%d: " fmt "\n", __func__, __LINE__,  \
          mpi_rank, tid, ##args)
#define dump_labels()                                                          \
  do {                                                                         \
    for (local_id i = 0; i < labels.size(); i++) {                             \
      DEBUG(3, "Label %llu: (%lld, %d)", vertices[i].id, labels[i].prev_node,  \
            labels[i].value);                                                  \
    }                                                                          \
  } while (0)
#define dump_flows()                                                           \
  do {                                                                         \
    for (local_id i = 0; i < vertices.size(); i++) {                           \
      const auto &edges = vertices[i].out_edges;                               \
      for (unsigned int j = 0; j < edges.size(); j++) {                        \
        DEBUG(3, "Edge (%llu, %llu): %d/%d", vertices[i].id,                   \
              edges[j].dest_node_id, edges[j].flow, edges[j].capacity);        \
      }                                                                        \
    }                                                                          \
  } while (0)
#else
#define DEBUG(lvl, fmt, args...)                                               \
  do {                                                                         \
  } while (0)
#define ERROR(fmt, args...)                                                    \
  do {                                                                         \
  } while (0)
#define dump_labels()                                                          \
  do {                                                                         \
  } while (0)
#define dump_flows()                                                           \
  do {                                                                         \
  } while (0)
#endif

/************MPI Variables *********************/
int mpi_rank;
int mpi_size;
MPI_Datatype MPI_MESSAGE_TYPE;

struct message_data {
  /// The ID of the node belonging to the sender
  global_id senders_node;
  /// The ID of the node belonging to the receiver
  global_id receivers_node;
  /// The relevant label value (identity depends on message type)
  int value;
  /// The current pass number
  int pass;
};

enum message_tags : int {
  /// Set the label on a node, generated from an incoming edge
  SET_TO_LABEL = 1,
  /// Compute and set the label on a node, generated from an outgoing edge
  COMPUTE_FROM_LABEL,
  /// Another rank found the sink node in step 2, move on to step 3. Pass on to
  /// the next rank.
  SINK_FOUND,
  /// Used during step 3
  UPDATE_FLOW,
  /// Another rank found the source node in step 3; go back to step 1. Pass on
  /// to the next rank.
  SOURCE_FOUND,
  /// Sent to rank 0 after the algorithm finishes, contains the flow through
  /// the graph
  TOTAL_FLOW,
  /// Termination detection tokens
  TOKEN_WHITE,
  TOKEN_RED,
  /// Sent to all ranks by rank 0; should start Allreduce over @c queue_is_empty
  CHECK_TERMINATION,
};

const char *tag2str(int tag) {
  switch (tag) {
  case SET_TO_LABEL:
    return "SET_TO_LABEL";
  case COMPUTE_FROM_LABEL:
    return "COMPUTE_FROM_LABEL";
  case SINK_FOUND:
    return "SINK_FOUND";
  case UPDATE_FLOW:
    return "UPDATE_FLOW";
  case SOURCE_FOUND:
    return "SOURCE_FOUND";
  case TOTAL_FLOW:
    return "TOTAL_FLOW";
  case TOKEN_WHITE:
    return "TOKEN_WHITE";
  case TOKEN_RED:
    return "TOKEN_RED";
  case CHECK_TERMINATION:
    return "CHECK_TERMINATION";
  default:
    return "INVALID_TAG";
  }
}

/********** Timer stuff ************/

double g_time_in_secs = 0;
// on the BG/Q, GetTimeBase returns a cycle number, but MPI_Wtime returns a
// fractional timestamp in seconds. Store the timestamp as a double on other
// machines, so we get higher resolution than 1 second.
#ifdef __bgq__
typedef unsigned long long timebase_t;
double g_processor_frequency = 1600000000.0; // processing speed for BG/Q
#else
typedef double timebase_t;
double g_processor_frequency = 1.0; // processing speed for other machines
#endif
timebase_t g_start_cycles = 0;
timebase_t g_end_cycles = 0;

/************Zoltan Library Variables **********/
struct Zoltan_Struct *zz;
float zoltan_version;

/****************** Globals ********************/

size_t num_threads = 64;
global_id graph_node_count;

// source and sink ids
global_id source_id = -1;
global_id sink_id = -1;

/// Number of threads currently doing work (not waiting for messages or edges)
int working_threads;
/// The current color of this rank
enum message_tags my_color;
/// Whether we currently have the token
bool have_token;
/// The color of the token, if we have it;
enum message_tags token_color;
/// Set if a worker thread has found the queue to be empty.
bool queue_is_empty;

// entries in `vertices` and entries in `labels` must correspond one-to-one
vector<struct vertex> vertices;
vector<struct label> labels;
map<global_id, local_id> global_to_local;
int *global_id_to_rank;
/// Set to true when the sink node is found in step 2.
bool sink_found = false;
/// The thread that should perform step 3 sets this atomically.
int step_3_tid = -1;
/// The current algorithm iteration count
int pass = 1;

/// Set to true when no valid paths can be found through the graph.
bool algorithm_complete = false;

EdgeQueue edge_queue;
Mutex h_lock;
Mutex t_lock;

struct thread_params {
  int tid;
  Barrier &barrier;
};

/**
 * Maps global IDs to local IDs. Needs to be fast for border nodes.
 *
 * @param id The global ID to lookup
 * @return The local ID of the given node, or @c (local_id)-1 if not found.
 */
local_id lookup_global_id(global_id id) {
  auto search = global_to_local.find(id);

  if (search == global_to_local.end())
    return -1;
  return search->second;
}

/*********** Zoltan Query Functions ***************/

// query function, returns the number of objects assigned to the processor
int user_return_num_obj(void *data, int *ierr) {

  // printf("RAN num obj\n");
  // int *result = (int *)data;
  // *result = network.size();

  *ierr = ZOLTAN_OK;
  return vertices.size();
}

// https://cs.sandia.gov/Zoltan/ug_html/ug_query_lb.html#ZOLTAN_OBJ_LIST_FN
void user_return_obj_list(void *data, int num_gid_entries, int num_lid_entries,
                          ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR local_ids,
                          int wgt_dim, float *obj_wgts, int *ierr) {
  // printf("RAN obj list\n");
  for (local_id i = 0; i < vertices.size(); ++i) {
    global_ids[i * num_gid_entries] = vertices[i].id;
    local_ids[i * num_lid_entries] = i;
  }

  *ierr = ZOLTAN_OK;
}

/************Zoltan Graph Query Functions**********/

// Return the number of edges in the given vertex
int user_num_edges(void *data, int num_gid_entries, int num_lid_entries,
                   ZOLTAN_ID_PTR global, ZOLTAN_ID_PTR local, int *ierr) {
  // printf("RAN num edges on node %d, size of %d+%d=%d\n", *local,
  //        vertices[*local].in_edges.size(),
  //        vertices[*local].out_edges.size(),
  //        (vertices[*local].in_edges.size() +
  //         vertices[*local].out_edges.size()));
  *ierr = ZOLTAN_OK;

  return vertices[*local].in_edges.size() + vertices[*local].out_edges.size();
}

// Return list of global ids, processor ids for nodes sharing an edge with the
// given object
void user_return_edge_list(void *data, int num_gid_entries, int num_lid_entries,
                           ZOLTAN_ID_PTR global, ZOLTAN_ID_PTR local,
                           ZOLTAN_ID_PTR nbor_global_id, int *nbor_procs,
                           int wgt_dim, float *ewgts, int *ierr) {
  // printf("-------%d, %d-%d; g:%d,l:%d\n", vertices.size(), num_gid_entries,
  //        num_lid_entries, *global, *local);
  const vertex &curr_vertex = vertices[*local];

  // printf("step 1\n");
  // go through all neighboring edges. in then out edges
  size_t nbor_idx = 0;
  for (size_t i = 0; i < curr_vertex.in_edges.size(); ++i, ++nbor_idx) {
    nbor_global_id[nbor_idx] = curr_vertex.in_edges[i].dest_node_id;
    nbor_procs[nbor_idx] = curr_vertex.in_edges[i].rank_location;
  }
  // printf("step 2\n");

  for (size_t i = 0; i < curr_vertex.out_edges.size(); ++i, ++nbor_idx) {
    nbor_global_id[nbor_idx] = curr_vertex.out_edges[i].dest_node_id;
    nbor_procs[nbor_idx] = curr_vertex.out_edges[i].rank_location;
  }
  // printf("step 3\n");

  *ierr = ZOLTAN_OK;
}

/////////// Zoltan Migration Functions:

struct packed_vert {
  size_t out_count;
  size_t in_count;
  unsigned char data[];
};

void user_pack_vertex(void *data, int num_gid_entries, int num_lid_entries,
                      ZOLTAN_ID_PTR global, ZOLTAN_ID_PTR local, int dest,
                      int size, char *buf, int *ierr) {
  auto *packed = (struct packed_vert *)buf;
  struct vertex &vert = vertices[*local];
  packed->out_count = vert.out_edges.size();
  packed->in_count = vert.in_edges.size();

  size_t out_size = sizeof(struct out_edge[packed->out_count]);
  size_t in_size = sizeof(struct in_edge[packed->in_count]);
  memcpy(packed->data, vert.out_edges.data(), out_size);
  memcpy(packed->data + out_size, vert.in_edges.data(), in_size);
}

void user_unpack_vertex(void *data, int num_gid_entries, ZOLTAN_ID_PTR global,
                        int size, char *bytes, int *ierr) {
  auto *packed = (struct packed_vert *)bytes;
  struct out_edge out_temp = {};
  struct in_edge in_temp = {};
  struct vertex vert = {*global,
                        vector<struct out_edge>(packed->out_count, out_temp),
                        vector<struct in_edge>(packed->in_count, in_temp)};
  size_t out_size = sizeof(struct out_edge[packed->out_count]);
  size_t in_size = sizeof(struct in_edge[packed->in_count]);
  memcpy(vert.out_edges.data(), packed->data, out_size);
  memcpy(vert.in_edges.data(), packed->data + out_size, in_size);

  // update rank_location of all edges
  for (auto it = vert.out_edges.begin(); it != vert.out_edges.end(); ++it) {
    it->rank_location = mpi_rank;
  }
  for (auto it = vert.in_edges.begin(); it != vert.in_edges.end(); ++it) {
    it->rank_location = mpi_rank;
  }

  vertices.push_back(vert);
}

// Copy all needed data for a single object into a communication buffer
// Return the byte size of the object
int user_return_obj_size(void *data, int num_global_ids, ZOLTAN_ID_PTR global,
                         ZOLTAN_ID_PTR local, int *ierr) {

  return sizeof(struct packed_vert) +
         sizeof(struct out_edge[vertices[*local].out_edges.size()]) +
         sizeof(struct in_edge[vertices[*local].in_edges.size()]);
}

/************ Zoltan Query Functions End ***************/

/**
 * Inserts edges between @c vertices[vert_idx] and neighboring unlabelled
 * nodes into the edge queue.
 *
 * @param vert_idx The local index of a newly labelled node.
 */
void insert_edges(local_id vert_idx, int tid) {
  const struct vertex &v = vertices[vert_idx];
  EdgeQueue fragment = EdgeQueue();
  DEBUG(2, "Adding %lu edges to queue", v.out_edges.size() + v.in_edges.size());
  for (unsigned int i = 0; i < v.out_edges.size(); ++i) {
    const out_edge &edge = v.out_edges[i];
    if (edge.rank_location == mpi_rank && labels[edge.vert_index].value != 0) {
      continue; // already has a label, skip it
    }
    if (edge.dest_node_id == labels[vert_idx].prev_node) {
      continue; // we came from here, so skip it
    }
    edge_entry temp = {
        vert_idx, // vertex_index
        true,     // is_outgoing
        i,        // edge_index
    };
    fragment.push(temp);
  }

  for (unsigned int i = 0; i < v.in_edges.size(); ++i) {
    const in_edge &edge = v.in_edges[i];
    if (edge.rank_location == mpi_rank && labels[edge.vert_index].value != 0) {
      continue; // already has a label, skip it
    }
    if (edge.dest_node_id == labels[vert_idx].prev_node) {
      continue; // we came from here, so skip it
    }
    edge_entry temp = {
        vert_idx, // vertex_index
        false,    // is_outgoing
        i,        // edge_index
    };
    fragment.push(temp);
  }
  t_lock.lock();
  fragment.merge_into(edge_queue);
  t_lock.unlock();
}

/**
 * Sets @c sink_found and returns the local id of the sink node if it was
 * found; otherwise returns (local_id)-1.
 *
 * @param entry The edge to process.
 */
local_id handle_out_edge(const struct edge_entry &entry, int tid);

/**
 * Sets @c sink_found and returns the local id of the sink node if it was
 * found; otherwise returns (local_id)-1.
 *
 * @param entry The edge to process.
 */
local_id handle_in_edge(const struct edge_entry &entry, int tid);

/**
 * Returns @c true if @p curr_idx is the sink node and we successfully set its
 * label.
 */
bool set_label(global_id prev_node, int prev_rank, local_id prev_idx,
               local_id curr_idx, int value, int tid);

/**
 * Waits for a message with the given tag and sender, and discard any
 * non-matching messages.
 *
 * @param tag
 * @param sender Defaults to the previous rank.
 */
void wait_and_flush(int tag,
                    int sender = (mpi_rank - 1 + mpi_size) % mpi_size) {
  struct message_data msg = {};
  MPI_Status stat;
  do {
    MPI_Recv(&msg, 1, MPI_MESSAGE_TYPE, MPI_ANY_SOURCE, MPI_ANY_TAG,
             MPI_COMM_WORLD, &stat);
  } while (stat.MPI_TAG != tag || stat.MPI_SOURCE != sender);
}

void *run_algorithm(struct thread_params *params) {
  int tid = params->tid;
  Barrier &barrier = params->barrier;

  while (!algorithm_complete) {
    // synchronize all threads before each iteration
    barrier.wait();

    /*--------*
     | Step 1 |
     *--------*/
    if (tid == 0) {
      // wipe previous labels
      fill(labels.begin(), labels.end(), EMPTY_LABEL);
      // setup globals
      working_threads = 0;
      my_color = TOKEN_WHITE;
      have_token = mpi_rank == 0;
      token_color = TOKEN_WHITE;
      queue_is_empty = false;
      sink_found = false;
      step_3_tid = -1;

      // empty out edge queue
      edge_entry entry = {};
      while (edge_queue.pop(entry))
        ;
      DEBUG(1, "Pass %d:", pass);
      // find source node
      local_id i = lookup_global_id(source_id);
      if (i != (local_id)-1) {
        set_label(source_id, mpi_rank, i, i,
                  numeric_limits<decltype(labels[i].value)>::max(), tid);
      }
    }

    /**
     * In step 3, holds the local index of the node that the backtracking
     * algorithm is currently processing. Local to each thread.
     *
     * Set to @c (local_id)-1 if the current backtracking node is not on this
     * rank.
     */
    local_id bt_idx = -1;
    /// Label value of sink node
    int sink_value = 0;

    // all threads must wait until everything is initialized
    barrier.wait();
    if (tid == 0) {
      DEBUG(1, "------------------ START STEP 2 ------------------");
    }

    /*--------*
     | Step 2 |
     *--------*/
    // Thread 0 handles all incoming messages, while the other threads run the
    // actual algorithm
    if (tid == 0) {
      struct message_data msg = {};
      local_id vert_idx;
      int curr_flow;

      while (!sink_found) {
        // if message tag is SINK_FOUND, set do_step_3 and sink_found to true,
        // so thread 0 on this rank will do step 3.
        MPI_Status stat;
        MPI_Recv(&msg, 1, MPI_MESSAGE_TYPE, MPI_ANY_SOURCE, MPI_ANY_TAG,
                 MPI_COMM_WORLD, &stat);
        __sync_fetch_and_add(&working_threads, 1);
        DEBUG(2, "S2: got msg %s from R%d", tag2str(stat.MPI_TAG),
              stat.MPI_SOURCE);
        switch (stat.MPI_TAG) {
        case SET_TO_LABEL:
          // try to set label of "to" node
          vert_idx = lookup_global_id(msg.receivers_node);
          if (vert_idx == (local_id)-1) {
            ERROR("SET_TO_LABEL sent to wrong rank");
            __sync_fetch_and_sub(&working_threads, 1);
            continue;
          }
          if (msg.pass != pass) {
            ERROR("***** Got old message! *****");
            __sync_fetch_and_sub(&working_threads, 1);
            continue;
          }
          if (set_label(msg.senders_node, stat.MPI_SOURCE, -1, vert_idx,
                        msg.value, tid)) {
            // found sink!
            bt_idx = vert_idx;
            DEBUG(1, "Setting step_3_tid from SET_TO_LABEL...");
            int old_val = __sync_val_compare_and_swap(&step_3_tid, -1, tid);
            if (old_val != -1) {
              ERROR("Thread %d set step_3_tid, but we have bt_idx!", old_val);
            }
            sink_found = true;
          }
          break;
        case COMPUTE_FROM_LABEL:
          // compute and set label of "from" node
          DEBUG(2, "looking up local id");
          vert_idx = lookup_global_id(msg.receivers_node); // from_id
          if (vert_idx == (local_id)-1) {
            ERROR("COMPUTE_FROM_LABEL sent to wrong rank");
            __sync_fetch_and_sub(&working_threads, 1);
            continue;
          }
          if (msg.pass != pass) {
            ERROR("***** Got old message! *****");
            __sync_fetch_and_sub(&working_threads, 1);
            continue;
          }

          // TODO: check this, bug found here in handle_in_edge
          // find edge for the sender's node, and get the flow through it
          curr_flow = 0;
          DEBUG(2, "size of out_edges: %lu",
                vertices[vert_idx].out_edges.size());
          for (auto it = vertices[vert_idx].out_edges.begin();
               it != vertices[vert_idx].out_edges.end(); ++it) {
            if (it->dest_node_id == msg.senders_node) {
              curr_flow = it->flow;
              break;
            }
          }
          if (curr_flow <= 0) {
            __sync_fetch_and_sub(&working_threads, 1);
            continue; // discard edge
          }

          // set label and add edges
          if (set_label(msg.senders_node, stat.MPI_SOURCE, -1, vert_idx,
                        -min(abs(msg.value), curr_flow), tid)) {
            // found sink!
            ERROR("outgoing edge from sink!");
            bt_idx = vert_idx;
            DEBUG(1, "Setting step_3_tid from COMPUTE_FROM_LABEL...");
            int old_val = __sync_val_compare_and_swap(&step_3_tid, -1, tid);
            if (old_val != -1) {
              ERROR("Thread %d set step_3_tid, but we have bt_idx!", old_val);
            }
            sink_found = true;
          }
          break;
        case SINK_FOUND:
          if (mpi_size > 1) {
            DEBUG(1, "Setting step_3_tid from SINK_FOUND...");
            int old_val = __sync_val_compare_and_swap(&step_3_tid, -1, tid);
            if (old_val == -1) {
              DEBUG(1, "We will handle step 3");
            } else {
              DEBUG(1, "Thread %d is handling step 3", old_val);
            }
            sink_found = true;
          } else {
            // running with one rank
            sink_found = true;
            // flush white tokens from own rank
            int flag = 1;
            do {
              MPI_Iprobe(mpi_rank, TOKEN_WHITE, MPI_COMM_WORLD, &flag,
                         MPI_STATUS_IGNORE);
              if (flag) {
                MPI_Recv(NULL, 0, MPI_MESSAGE_TYPE, mpi_rank, TOKEN_WHITE,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);
              }
            } while (flag);
          }
          break;
        case TOKEN_WHITE:
        case TOKEN_RED:
          token_color = (enum message_tags)stat.MPI_TAG;
          if (mpi_rank == 0) {
            if (token_color == TOKEN_WHITE) {
              // check termination: send message to all ranks then Allreduce
              DEBUG(1, "S2: got white token, sending CHECK_TERMINATION to all "
                       "ranks");
              for (int i = 1; i < mpi_size; ++i) {
                MPI_Ssend(NULL, 0, MPI_MESSAGE_TYPE, i, CHECK_TERMINATION,
                          MPI_COMM_WORLD);
              }
              // if result is 0, then all queues are empty, and we are done.
              int empty = queue_is_empty ? 0 : 1;
              int result = 0;
              MPI_Allreduce(&empty, &result, 1, MPI_INT, MPI_SUM,
                            MPI_COMM_WORLD);
              if (result == 0) {
                DEBUG(1, "Algorithm complete!");
                __sync_fetch_and_sub(&working_threads, 1);
                delete params;
                algorithm_complete = true;
                return NULL;
              } else {
                DEBUG(1, "Not all ranks have empty queues, continuing");
              }
            } else {
              // reset token color
              token_color = TOKEN_WHITE;
            }
          }
          DEBUG(1, "S2: we now have the token");
          have_token = true;
          break;
        case CHECK_TERMINATION: {
          // if result is 0, then all queues are empty, and we are done.
          int empty = queue_is_empty ? 0 : 1; // sum should be 0
          int result = 0;
          MPI_Allreduce(&empty, &result, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
          if (result == 0) {
            DEBUG(1, "Algorithm complete!");
            __sync_fetch_and_sub(&working_threads, 1);
            delete params;
            algorithm_complete = true;
            return NULL;
          }
        } break;
        default:
          ERROR("got invalid tag in step 2: %s", tag2str(stat.MPI_TAG));
          break;
        }
        __sync_fetch_and_sub(&working_threads, 1);
      }
    } else {
      struct edge_entry entry = {0, false, 0};
      while (!sink_found) {
        {
          ScopedLock l(h_lock);
          // wait for the queue to become non-empty
          while (!edge_queue.pop(entry) && !sink_found && !algorithm_complete) {
            // TODO: may need a mutex. Acquire before entering while loop.
            queue_is_empty = true;
            if (have_token && working_threads == 0 && !sink_found) {
              // send token
              // note: our color can only be changed after sending the token
              // (done here) or by a running thread. If we are here, then we
              // must be the only running thread.
              if (my_color == TOKEN_RED) {
                token_color = TOKEN_RED;
              }
              // send token to next rank
              have_token = false;
              DEBUG(1, "S2: queue empty, sending %s token to R%d",
                    token_color == TOKEN_WHITE ? "white" : "red",
                    (mpi_rank + 1) % mpi_size);
              MPI_Ssend(NULL, 0, MPI_MESSAGE_TYPE, (mpi_rank + 1) % mpi_size,
                        token_color, MPI_COMM_WORLD);
              my_color = TOKEN_WHITE;
            }
          }
          if (algorithm_complete) {
            DEBUG(1, "Algorithm complete!");
            delete params;
            return NULL;
          }

          __sync_fetch_and_add(&working_threads, 1);
          queue_is_empty = false;
          // release the lock on edge_queue now, so other threads can get edges
        }

        if (sink_found) {
          __sync_fetch_and_sub(&working_threads, 1);
          break;
        }

        // process edge
        if (entry.is_outgoing) {
          bt_idx = handle_out_edge(entry, tid);
        } else {
          bt_idx = handle_in_edge(entry, tid);
        }
        if (bt_idx != (local_id)-1) {
          DEBUG(1, "Found sink node!");
          DEBUG(1, "Setting step_3_tid from worker thread...");
          int old_val = __sync_val_compare_and_swap(&step_3_tid, -1, tid);
          if (old_val != -1) {
            ERROR("Thread %d set step_3_tid, but we have bt_idx!", old_val);
          }
          // tell thread 0 that the sink was found, to make sure it stops
          // before we start step 3. It will also set sink_found, so the other
          // worker threads stop too.
          DEBUG(1, "S2: sending msg SINK_FOUND to R%d (self)", mpi_rank);
          MPI_Ssend(NULL, 0, MPI_MESSAGE_TYPE, mpi_rank, SINK_FOUND,
                    MPI_COMM_WORLD);
          sink_found = true;
          __sync_fetch_and_sub(&working_threads, 1);
          break;
        }
        __sync_fetch_and_sub(&working_threads, 1);
      }
    }

    // make sure all threads finish step 2
    barrier.wait();

    /*--------*
     | Step 3 |
     *--------*/
    // go to the beginning of the loop and wait if not handling step 3.
    if (__sync_fetch_and_add(&step_3_tid, 0) != tid) {
      DEBUG(1, "returning to wait for step 3 to finish");
      continue;
    }

    DEBUG(1, "");
    DEBUG(1, "After step 2:");
    // dump_labels();

    int sink_founds_needed = 1;
    if (bt_idx != (local_id)-1) {
      // we found the sink and started step 3, so we need to get two SINK_FOUNDS
      DEBUG(1, "Setting sink_founds_needed to 2, since we found the sink");
      sink_founds_needed = 2;
      sink_value = labels[bt_idx].value;
    }
    if (mpi_size > 1) {
      while (sink_founds_needed) {
        DEBUG(1, "S3: sending SINK_FOUND to R%d", (mpi_rank + 1) % mpi_size);
        MPI_Ssend(NULL, 0, MPI_MESSAGE_TYPE, (mpi_rank + 1) % mpi_size,
                  SINK_FOUND, MPI_COMM_WORLD);
        DEBUG(1, "S3: waiting for SINK_FOUND to be returned");
        wait_and_flush(SINK_FOUND);
        DEBUG(1, "S3: got SINK_FOUND from R%d",
              (mpi_rank - 1 + mpi_size) % mpi_size);
        --sink_founds_needed;
      }
      // everyone but the finder has to pass the last message on
      if (bt_idx == (local_id)-1) {
        DEBUG(1, "S3: sending SINK_FOUND to R%d", (mpi_rank + 1) % mpi_size);
        MPI_Ssend(NULL, 0, MPI_MESSAGE_TYPE, (mpi_rank + 1) % mpi_size,
                  SINK_FOUND, MPI_COMM_WORLD);
      }
    }

    // flush rest of messages
    int flag = 1;
    do {
      MPI_Status stat;
      MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &stat);
      if (flag) {
        struct message_data msg = {};
        MPI_Recv(&msg, 1, MPI_MESSAGE_TYPE, stat.MPI_SOURCE, stat.MPI_TAG,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      }
    } while (flag);

    DEBUG(1, "entering barrier before step 3");
    MPI_Barrier(MPI_COMM_WORLD);
    DEBUG(1, "================== START STEP 3 ==================");
    DEBUG(1, "My bt_idx is %ld", (ssize_t)bt_idx);

    // start backtracking
    bool wait_for_source_found = false;
    bool step_3_done = false;
    while (!step_3_done) {
      if (bt_idx != (local_id)-1) {
        // update flow in local nodes
        struct label &l = labels[bt_idx];
        DEBUG(1, "S3: processing node %llu", vertices[bt_idx].id);
        // TODO: looping over all edges will be slow for dense graphs
        if (l.value > 0 && l.prev_rank_loc == mpi_rank) {
          // bt_idx is a "from" node and previous node is local
          // let f(y, x) += sink_value
          for (auto it = vertices[l.prev_vert_index].out_edges.begin();
               it != vertices[l.prev_vert_index].out_edges.end(); ++it) {
            if (it->dest_node_id == vertices[bt_idx].id)
              it->flow += sink_value;
          }
        } else if (l.value < 0) {
          // let f(x, y) -= sink_value
          for (auto it = vertices[bt_idx].out_edges.begin();
               it != vertices[bt_idx].out_edges.end(); ++it) {
            if (it->dest_node_id == l.prev_node)
              it->flow -= sink_value;
          }
        }

        // if the previous node is not on this rank, send the next rank an
        // UPDATE_FLOW message, then wait for incoming messages
        if (l.prev_rank_loc != mpi_rank) {
          // previous node is remote, send an UPDATE_FLOW message
          struct message_data msg = {
              vertices[bt_idx].id, // sender's node
              l.prev_node,         // receiver's node
              sink_value,          // label value
              pass,                // current pass
          };
          DEBUG(1, "S3: sending UPDATE_FLOW to R%d", l.prev_rank_loc);
          MPI_Ssend(&msg, 1, MPI_MESSAGE_TYPE, l.prev_rank_loc, UPDATE_FLOW,
                    MPI_COMM_WORLD);
          bt_idx = -1;
        } else {
          // check for source node
          if (bt_idx == l.prev_vert_index && l.prev_node == source_id) {
            // source node was already processed, exit the loop
            wait_for_source_found = mpi_size > 1;
            step_3_done = true;
            continue;
          }
          // otherwise, keep following back-pointers
          bt_idx = l.prev_vert_index;
        }
      } else {
        // wait for incoming messages
        struct message_data msg = {};
        MPI_Status stat;
        MPI_Recv(&msg, 1, MPI_MESSAGE_TYPE, MPI_ANY_SOURCE, MPI_ANY_TAG,
                 MPI_COMM_WORLD, &stat);
        DEBUG(1, "S3: got msg %s from R%d", tag2str(stat.MPI_TAG),
              stat.MPI_SOURCE);
        switch (stat.MPI_TAG) {
        case SOURCE_FOUND:
          // if SOURCE_FOUND is received, break and forward it the next rank
          wait_for_source_found = false;
          step_3_done = true;
          break;
        case UPDATE_FLOW: {
          // find our local node
          sink_value = msg.value;
          local_id vert_idx = lookup_global_id(msg.receivers_node);
          auto it = vertices[vert_idx].out_edges.begin();
          // find the remote node in the local node's edge list
          for (; it != vertices[vert_idx].out_edges.end(); ++it) {
            if (it->dest_node_id == msg.senders_node)
              it->flow += sink_value;
          }
          // if the sender's node is not found in out_edges, then vert_idx
          // must be the "to" node and we don't need to do anything
          bt_idx = vert_idx; // continue with the previous node
        } break;
        case SET_TO_LABEL:
        case COMPUTE_FROM_LABEL:
        case TOKEN_WHITE:
        case TOKEN_RED:
          DEBUG(1, "got old message during step 3 with tag %s",
                tag2str(stat.MPI_TAG));
          break;
        default:
          ERROR("got invalid message during step 3 with tag %s",
                tag2str(stat.MPI_TAG));
          break;
        }
      }
    }

    // send SOURCE_FOUND message to next rank
    if (mpi_size > 1) {
      DEBUG(1, "S3: sending SOURCE_FOUND to R%d", (mpi_rank + 1) % mpi_size);
      MPI_Ssend(NULL, 0, MPI_MESSAGE_TYPE, (mpi_rank + 1) % mpi_size,
                SOURCE_FOUND, MPI_COMM_WORLD);
    }

    // wait to receive the SOURCE_FOUND message from previous rank if
    // necessary
    if (wait_for_source_found) {
      wait_and_flush(SOURCE_FOUND);
      DEBUG(1, "S3: got SOURCE_FOUND from R%d, done with step 3",
            (mpi_rank - 1 + mpi_size) % mpi_size);
    }

    DEBUG(1, "Entering barrier after step 3");
    MPI_Barrier(MPI_COMM_WORLD);
    DEBUG(1, "=================== END STEP 3 ===================");

    DEBUG(1, "After step 3:");
    // dump_flows();
    DEBUG(1, "");
    pass++;
  }

  delete params;
  return NULL;
}

bool set_label(global_id prev_node, int prev_rank, local_id prev_idx,
               local_id curr_idx, int value, int tid) {
  // atomically set label, only if it was unset before
  if (__sync_bool_compare_and_swap(&labels[curr_idx].value, 0, value)) {
    // label was unset before, so go ahead and set prev pointer
    labels[curr_idx].prev_node = prev_node;
    labels[curr_idx].prev_rank_loc = prev_rank;
    labels[curr_idx].prev_vert_index = prev_idx;
    if (vertices[curr_idx].id == sink_id) {
      return true;
    } else {
      // add edges to queue
      insert_edges(curr_idx, tid);
    }
  }
  return false;
}

local_id handle_out_edge(const struct edge_entry &entry, int tid) {
  local_id from_id = entry.vertex_index;
  struct out_edge &edge = vertices[from_id].out_edges[entry.edge_index];

  // always compute label locally
  int flow_diff = edge.capacity - edge.flow;
  if (flow_diff <= 0) {
    return -1; // discard edge
  }

  int label_val = min(abs(labels[from_id].value), flow_diff);
  // check if "to" node is on another rank
  if (edge.rank_location == mpi_rank) {
    // set label and add edges
    if (set_label(vertices[from_id].id, mpi_rank, from_id, edge.vert_index,
                  label_val, tid)) {
      return edge.vert_index;
    }
  } else {
    // send message to the owner of the "to" node
    struct message_data msg = {
        vertices[from_id].id, // sender's node
        edge.dest_node_id,    // receiver's node
        label_val,            // label value
        pass,                 // current pass
    };
    // update this rank's color if necessary
    if (edge.rank_location < mpi_rank) {
      my_color = TOKEN_RED;
    }
    DEBUG(2, "S2: sending msg SET_TO_LABEL to R%d", edge.rank_location);
    MPI_Ssend(&msg, 1, MPI_MESSAGE_TYPE, edge.rank_location, SET_TO_LABEL,
              MPI_COMM_WORLD);
  }
  return -1;
}

local_id handle_in_edge(const struct edge_entry &entry, int tid) {
  local_id to_id = entry.vertex_index;
  struct in_edge &rev_edge = vertices[to_id].in_edges[entry.edge_index];

  // check if "from" node (which holds the flow) is on another rank
  if (rev_edge.rank_location == mpi_rank) {
    local_id from_id = rev_edge.vert_index;
    // find matching edge in out_edges
    int curr_flow = -1;
    // TODO: looping over all edges will be slow for dense graphs
    for (auto it = vertices[from_id].out_edges.begin();
         it != vertices[from_id].out_edges.end(); ++it) {
      if (it->vert_index == to_id) {
        curr_flow = it->flow;
        break;
      }
    }
    if (curr_flow <= 0) {
      return -1; // discard edge
    }

    int label_val = -min(abs(labels[to_id].value), curr_flow);

    // set label and add edges
    if (set_label(vertices[to_id].id, mpi_rank, to_id, from_id, label_val,
                  tid)) {
      ERROR("outgoing edge from sink!");
      return from_id;
    }
  } else {
    // send message to the owner of the "from" node
    struct message_data msg = {
        vertices[to_id].id,    // sender's node
        rev_edge.dest_node_id, // receiver's node
        labels[to_id].value,   // label value
        pass,                  // current pass
    };
    // update this rank's color if necessary
    if (rev_edge.rank_location < mpi_rank) {
      my_color = TOKEN_RED;
    }
    DEBUG(2, "S2: sending msg COMPUTE_FROM_LABEL to R%d",
          rev_edge.rank_location);
    MPI_Ssend(&msg, 1, MPI_MESSAGE_TYPE, rev_edge.rank_location,
              COMPUTE_FROM_LABEL, MPI_COMM_WORLD);
  }
  return -1;
}

int calc_max_flow() {
  Barrier barrier(num_threads);
  pthread_t threads[num_threads];
  struct thread_params shared_params = {-1, barrier};

  // initialize vector of labels
  labels = vector<struct label>(vertices.size(), EMPTY_LABEL);

  // spawn threads
  for (size_t i = 0; i < num_threads; i++) {
    auto *params = new struct thread_params(shared_params);
    params->tid = i;
    pthread_create(&threads[i], NULL, (void *(*)(void *))run_algorithm,
                   (void *)params);
  }
  // wait for threads to finish
  for (size_t i = 0; i < num_threads; ++i) {
    pthread_join(threads[i], NULL);
  }

  cout << "Calculation complete!\n";

  // sum up flow out of source node
  local_id src_idx = lookup_global_id(source_id);
  int total_flow = -1;
  if (src_idx != (local_id)-1) {
    total_flow = 0;
    for (local_id i = 0; i < vertices[src_idx].out_edges.size(); ++i) {
      total_flow += vertices[src_idx].out_edges[i].flow;
    }
  }
  // send to rank 0
  if (mpi_rank == 0) {
    if (total_flow == -1) {
      // need to receive the total flow from another node
      MPI_Status stat;
      MPI_Recv(&total_flow, 1, MPI_INT, MPI_ANY_SOURCE, TOTAL_FLOW,
               MPI_COMM_WORLD, &stat);
    }
    // otherwise, we have already have the flow
  } else {
    if (total_flow != -1) {
      MPI_Ssend(&total_flow, 1, MPI_INT, 0, TOTAL_FLOW, MPI_COMM_WORLD);
      total_flow = -1;
    }
  }

  return total_flow;
}

// Read in an adjacency list file into network
// Return the vertex count, or 0 if there was an error
global_id read_file(const string &filepath) {
  ifstream file(filepath.c_str());
  if (!file)
    return 0;

  // Read first line: number vertices and number edges
  string line;
  if (!getline(file, line))
    return 0;
  global_id num_vertices;
  size_t num_edges;
  istringstream iss_(line);
  iss_ >> num_vertices >> num_edges;

  // Initialize all vertices so that their in and out edges can be added to
  struct vertex temp;
  for (global_id i = 0; i < num_vertices; i++) {
    temp.id = i;
    vertices.push_back(temp);
  }

  // Read every line
  global_id curr_index = 0; // Track the current index
  while (getline(file, line)) {
    // Read in every vertex, capacity pair
    istringstream iss(line);
    global_id connected_vertex;
    int capacity;
    while (iss >> connected_vertex >> capacity) {
      // Create new matching in and out edges
      struct out_edge out_temp = {connected_vertex, 0, (local_id)-1, capacity,
                                  0};
      vertices[curr_index].out_edges.push_back(out_temp);

      struct in_edge in_temp = {(global_id)curr_index, 0, (local_id)-1};
      vertices[connected_vertex].in_edges.push_back(in_temp);
    }

    curr_index += 1;
  }

  return num_vertices;
}

// For now going to assume all ranks will load the entire graph
int main(int argc, char **argv) {
  int mpi_thread_support;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &mpi_thread_support);
  if (mpi_thread_support != MPI_THREAD_MULTIPLE) {
    cout << "Error: MPI_THREAD_MULTIPLE not supported!" << endl;
    MPI_Abort(MPI_COMM_WORLD, 1);
    return 1;
  }
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);

  {
    // create MPI datatype for inter-rank messages
    const int count = 2;
    int block_lengths[count] = {2, 2};
    MPI_Datatype types[count] = {GLOBAL_ID_TYPE, MPI_INT};
    MPI_Aint offsets[count] = {offsetof(message_data, senders_node),
                               offsetof(message_data, value)};
    MPI_Type_create_struct(count, block_lengths, offsets, types,
                           &MPI_MESSAGE_TYPE);
    MPI_Type_commit(&MPI_MESSAGE_TYPE);
  }

  // Initialize Network
  // Root rank will handle partitioning, file reading, broadcasting rank map

  // check arguments
  if (argc != 3) {
    if (mpi_rank == 0)
      cout << "ERROR: Was expecting " << argv[0]
           << " filepath_to_input num_threads" << endl;
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  num_threads = atoi(argv[2]);
  if (mpi_rank == 0) {
    graph_node_count = read_file(argv[1]);
    if (graph_node_count == 0) {
      cout << "Error reading file" << endl;
      MPI_Abort(MPI_COMM_WORLD, 2);
    }
  } else {
    // Nothing for other ranks, wait for partitioning
  }

  printf("rank=%d, size=%d\n", mpi_rank, mpi_size);
  MPI_Bcast(&graph_node_count, 1, GLOBAL_ID_TYPE, 0, MPI_COMM_WORLD);
  // printf("Ready to partition\n");
  printf("graph_node_count: %llu\n", graph_node_count);

  // Zoltan Initialization
  Zoltan_Initialize(argc, argv, &zoltan_version);
  zz = Zoltan_Create(MPI_COMM_WORLD);

  /* Register Zoltan's query functions */
  Zoltan_Set_Fn(zz, ZOLTAN_NUM_OBJ_FN_TYPE, (void (*)())user_return_num_obj,
                NULL);
  Zoltan_Set_Fn(zz, ZOLTAN_OBJ_LIST_FN_TYPE, (void (*)())user_return_obj_list,
                NULL);

  // Graph query functions
  Zoltan_Set_Fn(zz, ZOLTAN_NUM_EDGES_FN_TYPE, (void (*)())user_num_edges, NULL);
  Zoltan_Set_Fn(zz, ZOLTAN_EDGE_LIST_FN_TYPE, (void (*)())user_return_edge_list,
                NULL);

  // Pack/unpack for data migration
  Zoltan_Set_Fn(zz, ZOLTAN_OBJ_SIZE_FN_TYPE, (void (*)())user_return_obj_size,
                NULL);
  Zoltan_Set_Fn(zz, ZOLTAN_PACK_OBJ_FN_TYPE, (void (*)())user_pack_vertex,
                NULL);
  Zoltan_Set_Fn(zz, ZOLTAN_UNPACK_OBJ_FN_TYPE, (void (*)())user_unpack_vertex,
                NULL);

  /* Set Zoltan parameters. */
  Zoltan_Set_Param(zz, "LB_METHOD", "GRAPH");
  Zoltan_Set_Param(zz, "GRAPH_PACKAGE", "Parmetis");
  Zoltan_Set_Param(zz, "LB_APPROACH", "PARTITION");
  Zoltan_Set_Param(zz, "AUTO_MIGRATE",
                   "TRUE"); // Might need user-guided for edges?
  Zoltan_Set_Param(zz, "RETURN_LISTS", "PARTS");
  Zoltan_Set_Param(zz, "DEBUG_LEVEL", "0");

  // Initialize Network
  // Root rank will handle partitioning, file reading, broadcasting rank map

  // Start recording time base for partitioning
  if (mpi_rank == 0) {
    g_start_cycles = GetTimeBase();
  }

  // Basing on https://cs.sandia.gov/Zoltan/ug_html/ug_examples_lb.html
  int num_changes; // Set to 1/True if decomposition was changed
  int num_imported, num_exported, *import_processors, *export_processors;
  int *import_to_parts, *export_to_parts;
  int num_gid_entries, num_lid_entries;
  ZOLTAN_ID_PTR import_global_ids, export_global_ids;
  ZOLTAN_ID_PTR import_local_ids, export_local_ids;
  // Parameters essentially: global info(output), import info, export info
  // printf("r%d: Entering lb partition. n=%d\n", mpi_rank, network.size());
  Zoltan_LB_Partition(zz, &num_changes, &num_gid_entries, &num_lid_entries,
                      &num_imported, &import_global_ids, &import_local_ids,
                      &import_processors, &import_to_parts, &num_exported,
                      &export_global_ids, &export_local_ids, &export_processors,
                      &export_to_parts);
  // Also handles data migration as AUTO_MIGRATE was set to true
  // Don't need this, so go ahead and free it now
  Zoltan_LB_Free_Part(&import_global_ids, &import_local_ids, &import_processors,
                      &import_to_parts);

  MPI_Barrier(MPI_COMM_WORLD);

  // printf("r%d: imported %d, exported %d. num_changes=%d Final size=%lu; g/l
  // id "
  //        "entries:%d, %d\n",
  //        mpi_rank, num_imported, num_exported, num_changes, vertices.size(),
  //        num_gid_entries, num_lid_entries);
  // for (local_id i = 0; i < vertices.size(); i++) {
  //   if (num_exported == 0) {
  //     printf("r%d: vertices[%lu]=%llu. %lu in, %lu out.\n", mpi_rank, i,
  //            vertices[i].id, vertices[i].in_edges.size(),
  //            vertices[i].out_edges.size());
  //   } else {
  //     printf("r%d: vertices[%lu]=%llu. %lu in, %lu out; exported to rank
  //     %d\n",
  //            mpi_rank, i, vertices[i].id, vertices[i].in_edges.size(),
  //            vertices[i].out_edges.size(), export_processors[i]);
  //   }
  // }

  // Process the map of where vertices went and remove exported vertices
  if (mpi_rank == 0) {
    global_id_to_rank = export_processors;

    for (long long i = vertices.size() - 1; i >= 0; i--) { // Iterate in reverse
      // Remove from this rank if it was exported
      if (export_processors[i] != mpi_rank) {
        // printf(
        //     "r%d: removing exported network[%lld]=%llu. Was exported to
        //     %d\n", mpi_rank, i, vertices[i].id, export_processors[i]);
        vertices.erase(vertices.begin() + i);
      }
    }
  } else {
    global_id_to_rank = new int[graph_node_count];
  }
  // MPI_Barrier(MPI_COMM_WORLD);
  // MPI_Bcast(&total_network_size, 1, MPI_INT, 0, MPI_COMM_WORLD);
  // printf("r%d: Next?\n", mpi_rank);
  // Broadcast array of export_processors essentially.
  // Indices represent vertex IDs, values represent the rank they're on
  MPI_Bcast(global_id_to_rank, graph_node_count, MPI_INT, 0, MPI_COMM_WORLD);

  // Print out all contents for testing
  // for (local_id i = 0; i < vertices.size(); i++) {
  //   printf("r%d: id=%llu; in_size=%lu, out_size=%lu\n", mpi_rank,
  //          vertices[i].id, vertices[i].in_edges.size(),
  //          vertices[i].out_edges.size());
  // }

  // construct global-to-local ID lookup
  for (local_id i = 0; i < vertices.size(); ++i) {
    global_to_local[vertices[i].id] = i;
  }

  // update all local indices and rank locations in all edges
  for (auto v_it = vertices.begin(); v_it != vertices.end(); ++v_it) {
    for (auto it = v_it->out_edges.begin(); it != v_it->out_edges.end(); ++it) {
      // update rank location of the "to" node
      it->rank_location = global_id_to_rank[it->dest_node_id];
      if (it->rank_location == mpi_rank) {
        // "to" node is on this rank, store local index
        it->vert_index = global_to_local[it->dest_node_id];
      }
    }
    for (auto it = v_it->in_edges.begin(); it != v_it->in_edges.end(); ++it) {
      // update rank location of the "from" node
      it->rank_location = global_id_to_rank[it->dest_node_id];
      if (it->rank_location == mpi_rank) {
        // "from" node is on this rank, store local index
        it->vert_index = global_to_local[it->dest_node_id];
      }
    }
  }

  // Stop timer
  if (mpi_rank == 0) {
    g_end_cycles = GetTimeBase();
    g_time_in_secs =
        ((double)(g_end_cycles - g_start_cycles) / g_processor_frequency);
    cout << "Partition time: " << g_time_in_secs << endl;
  }

  // Other stuff to fill out? TODO check

  /* Ready to begin algorithm! */

  source_id = 0;
  sink_id = graph_node_count - 1;

  // Start recording time base
  g_start_cycles = GetTimeBase();

  // Run algorithm
  int max_flow = calc_max_flow();

  // Stop timer
  if (mpi_rank == 0) {
    g_end_cycles = GetTimeBase();
    g_time_in_secs =
        ((double)(g_end_cycles - g_start_cycles) / g_processor_frequency);
  }

  if (mpi_rank == 0) {
    cout << "\nMax flow: " << max_flow << endl;
    cout << "Runtime: " << g_time_in_secs << endl;
  } else {
    delete[] global_id_to_rank;
  }

  /*Begin closing/freeing things*/
  Zoltan_LB_Free_Part(&export_global_ids, &export_local_ids, &export_processors,
                      &export_to_parts);

  Zoltan_Destroy(&zz);

  MPI_Finalize();
  return 0;
}
