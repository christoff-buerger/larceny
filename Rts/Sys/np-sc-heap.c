/* Copyright 1998 Lars T Hansen.
 *
 * $Id$
 *
 * Larceny -- two-generation non-predictive copying dynamic area.
 *
 * The collector sets up the 'old' areas as generation data->gen_no, and
 * the 'young' area as generation data->gen_no+1, to let the existing 
 * write barrier and scanning machinery work.
 */

#define GC_INTERNAL

#include "larceny.h"
#include "memmgr.h"
#include "gc.h"
#include "gclib.h"
#include "los_t.h"
#include "gc_t.h"
#include "young_heap_t.h"
#include "old_heap_t.h"
#include "semispace_t.h"
#include "heap_stats_t.h"
#include "remset_t.h"
#include "static_heap_t.h"

enum action { PROMOTE_TO_OLD, PROMOTE_TO_BOTH, PROMOTE_TO_YOUNG, COLLECT };

typedef struct npsc_data npsc_data_t;

struct npsc_data {
  int gen_no;                 /* generation number of 'old' generation */

  /* Parameters. */
  int size_bytes;             /* Original size */
  int k;                      /* k = current number of steps; k > 0 */
  int j;                      /* j = dividing point; 0 <= j < k */
  int stepsize;               /* bytes */

  /* Policy: j is calculated either as a percentage of free steps, or it's 
     pinned at some value (if possible).  For example, if pin_value == 1
     and there is at least one empty step, then j is set to 1.  
     j_percent and j_pin can't both be -1 at the same time.
     */
  int j_percent;              /* -1 or percentage for calculating j */
  int j_pin;                  /* -1 or value at which to pin j */
  double load_factor;
  double luck;		      /* 0.0 .. 1.0 */
  int lower_limit;            /* 0 or lower limit on the non-predictive area */
  int upper_limit;	      /* 0 or upper limit on the non-predictive area */
  semispace_t *old;	      /* 'old' generation */
  semispace_t *young;	      /* 'young' generation */

  int copied_last_gc_old;     /* bytes */
  int moved_last_gc_old;
  int copied_last_gc_young;   /* bytes */
  int moved_last_gc_young;
};

#define DATA(x)             ((npsc_data_t*)((x)->data))

static old_heap_t *allocate_heap( int gen_no, gc_t *gc );
static void perform_promote_to_old( old_heap_t *heap );
static void perform_promote_to_both( old_heap_t *heap );
static void perform_collect( old_heap_t *heap );
static enum action decision( old_heap_t *heap );
static int used_young( old_heap_t *heap );
static int used_old( old_heap_t *heap );
static int compute_dynamic_size( old_heap_t *heap, int D, int Q );
static int cleanup_scanner( word obj, void *data, unsigned *ignored );

old_heap_t *
create_np_dynamic_area( int gen_no, int *gen_allocd, gc_t *gc, np_info_t *info)
{
  old_heap_t *heap;
  npsc_data_t *data;
  int target_size;

  heap = allocate_heap( gen_no, gc );
  data = DATA(heap);

  *gen_allocd = 2;

  /* We have that size_bytes = stepsize*steps */
  data->size_bytes = roundup_page( info->size_bytes );
  data->stepsize = info->stepsize;

  data->j_pin = -1;
  data->j_percent = 50;
  data->load_factor = info->load_factor;
  data->lower_limit = info->dynamic_min;
  data->upper_limit = info->dynamic_max;
  data->luck = info->luck;

  /* Assume size/L live (steady state) for initial k */
  target_size = 
    compute_dynamic_size( heap,
			  data->size_bytes / data->load_factor,
			  0 );
  data->k = ceildiv( target_size, info->stepsize );

  /* This is an OK initial j if the heap is empty.  If the heap is used
     to load the heap image into, then data_load_area(), below,
     computes a more appropriate j.
     */
  data->j = ceildiv( data->k, 3 );

  annoyingmsg( "NP collector: k=%d j=%d", data->k, data->j );

  data->old = create_semispace( GC_CHUNK_SIZE, gen_no, gen_no );
  data->young = create_semispace( GC_CHUNK_SIZE, gen_no, gen_no+1 );

  heap->maximum = data->stepsize * data->k;
  heap->allocated = 0;

  return heap;
}

void np_gc_parameters( old_heap_t *heap, int *k, int *j )
{
  *k = DATA(heap)->k;
  *j = DATA(heap)->j;
}

static void collect( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);
  gc_t *gc = heap->collector;
  los_t *los = gc->los;
  int old_before_gc, old_los_before_gc, young_before_gc, young_los_before_gc;
  int type;

  annoyingmsg( "" );
  annoyingmsg( "Non-predictive dynamic area: garbage collection. " );
  annoyingmsg( "  Live old: %d   Live young: %d  k: %d  j: %d",
	       used_old( heap ), used_young( heap ), data->k, data->j );

  ss_sync( data->old );
  ss_sync( data->young );
  old_before_gc = data->old->used;
  old_los_before_gc = los_bytes_used( gc->los, data->gen_no );
  young_before_gc = data->young->used;
  young_los_before_gc = los_bytes_used( los, data->gen_no+1 );

  switch (type = decision( heap )) {
    case PROMOTE_TO_OLD   : perform_promote_to_old( heap ); break;
    case PROMOTE_TO_BOTH  : perform_promote_to_both( heap ); break;
    case PROMOTE_TO_YOUNG : perform_promote_to_both( heap ); break;
    case COLLECT          : perform_collect( heap );  break;
    default               : panic_abort( "Impossible." );
  }

  ss_sync( data->young );
  ss_sync( data->old );
  if (type == COLLECT) {
    data->copied_last_gc_old = data->old->used - young_before_gc;
    data->moved_last_gc_old =
      los_bytes_used( los, data->old->gen_no ) - young_los_before_gc;
    data->copied_last_gc_young = 0;
    data->moved_last_gc_young = 0;
  }
  else {
    data->copied_last_gc_young = data->young->used - young_before_gc;
    data->moved_last_gc_young =
      los_bytes_used( los, data->gen_no+1 ) - young_los_before_gc;
    data->copied_last_gc_old = data->old->used - old_before_gc;
    data->moved_last_gc_old = 
      los_bytes_used( los, data->gen_no ) - old_los_before_gc;
  }

  annoyingmsg( "Non-predictive dynamic area: collection finished." );
}

/* Cautious strategy: 
     Let X be the amount of memory allocated in all ephmeral generations.
     Let No be the amount of memory available in the 'old' generation.
     Let Ny be the amount of memory available in the 'young' generation.
     if X <= No then
       promote from ephemeral generations to 'old'
     else if No is small then
       promote from ephemeral generations to 'young'
     else if X <= No+Ny then
       promote from ephemeral generations to 'old' and 'young'
     else
       collect
   */
static enum action decision( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);
  gc_t *gc = heap->collector;
  int X, No, Ny, i;

  /* Gather data */

  X = gc->young_area->allocated;
  for ( i = 0 ; i < gc->ephemeral_area_count ; i++ )
    X += gc->ephemeral_area[i]->allocated;

  No = data->stepsize * (data->k - data->j) - used_old( heap );
  Ny = data->stepsize * data->j - used_young( heap );

  if (X <= No)
    return PROMOTE_TO_OLD;
  else if (No < PAGESIZE)
   return PROMOTE_TO_YOUNG;
  else if (X <= No + Ny)
    return PROMOTE_TO_BOTH;
  else
    return COLLECT;
}

static void perform_promote_to_old( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);

  annoyingmsg( "  Promoting into old area." );
  stats_gc_type( data->gen_no, STATS_PROMOTE );

  gclib_stopcopy_promote_into( heap->collector, data->old );
  rs_clear( heap->collector->remset[ data->gen_no ] );
}

static void perform_promote_to_both( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);
  los_t *los = heap->collector->los;
  int x, young_available, old_available;

  annoyingmsg( "  Promoting to both old and young." );
  stats_gc_type( data->gen_no+1, STATS_PROMOTE );

  young_available = data->j*data->stepsize - used_young( heap );
  old_available = (data->k-data->j)*data->stepsize - used_old( heap );

  gclib_stopcopy_promote_into_np( heap->collector,
				  data->old,
				  data->young,
				  old_available,
				  young_available);

  rs_clear( heap->collector->remset[ data->gen_no ] );
  rs_assimilate( heap->collector->remset[ heap->collector->np_remset ],
		 heap->collector->remset[ data->gen_no+1 ] );
  rs_clear( heap->collector->remset[ data->gen_no+1 ] );
  
  /* This adjustment is only required when a large object has been promoted
     into the 'old' space and overflowed it.
     */
  x = used_old( heap );
  while ((data->k - data->j)*data->stepsize < x)
    data->j--;

  if (data->j < 0) {
    data->k = ceildiv( x, data->stepsize );
    data->j = 0;
    annoyingmsg( "Extending NP area to accomodate large object overflow: "
		 "k=%d, j=%d.",
		 data->k, data->j );
  }
}

static void perform_collect( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);
  los_t *los = heap->collector->los;
  gc_t *gc = heap->collector;
  int free_steps, target_size, young_before, young_los_before, luck_steps;

  ss_sync( data->young );
  young_before = data->young->used;
  young_los_before = los_bytes_used( los, data->gen_no+1 );

  annoyingmsg( "  Full garbage collection." );
  stats_gc_type( data->gen_no, STATS_COLLECT );

  gclib_stopcopy_collect_np( gc, data->young );
  rs_clear( gc->remset[ data->gen_no ] );
  rs_clear( gc->remset[ data->gen_no+1 ] );
  rs_clear( gc->remset[ gc->np_remset ] );

  /* Manipulate the semispaces: young becomes old, old is deallocated */
  ss_free( data->old );
  data->old = data->young;

  ss_set_gen_no( data->old, data->gen_no );
  assert( los_bytes_used( los, data->gen_no ) == 0 );
  los_append_and_clear_list( los, 
			     los->object_lists[ data->gen_no+1 ], 
			     data->gen_no);

  data->young = create_semispace( GC_CHUNK_SIZE, data->gen_no, data->gen_no+1);

  /* Compute new k and j, and other policy parameters */
  /* Young is empty */
  /* What should the new k be?
   *
   * At the time of the next gc, whatever's in 1..j will not be copied,
   * so no space need be set aside for it.  
   *
   * However, when the new heap size is computed, only the load factor
   * L is taken into account, so space is reserved for the live storage
   * from 1..j.  The amount of space so reserved is j*stepsize (because
   * we assume that all of 1..j is live).  If j is not close to 0, this can
   * be a considerable amount of space.  Also, assuming that 1..j is live,
   * then as 1..j approaches heapsize/L, live data in j+1..k will approach 0,
   * requiring copyspace that also approaches 0, whereas reserved copyspace
   * approaches heapsize/L.
   *
   * We can use the space in two ways:
   *  - Keep it as part of copyspace, effectively reducing L.  Since
   *    efficiency is improved as L increases, this is an undesirable
   *    solution, but it's easy.
   *  - Split the space among the steps j+1..k and copyspace in a
   *    proportion based on L; i.e., increase k.  This uses memory
   *    appropriately, but is harder to implement because the choices
   *    of j, k, and heap size are interdependent.
   *
   * Below, I use the former method, for simplicity, but note the use
   * of the luck parameter to adjust k, below.
   */
  ss_sync( data->old );
  target_size =
    compute_dynamic_size( heap,
			  data->old->used,
			  los_bytes_used( los, data->gen_no ) );
  data->k = ceildiv( target_size, data->stepsize );

  free_steps = (data->k * data->stepsize - used_old( heap )) / data->stepsize;
  if (free_steps < 0) {
    /* Soft overflow in a fixed heap. */
    /* It might be more reasonable to give up at this point. */
    free_steps = 0;
  }

  if (data->j_percent >= 0)
    data->j = (free_steps * data->j_percent) / 100;
  else if (free_steps >= data->j_pin)
    data->j = data->j_pin;
  else {
    data->j = free_steps / 2;
    supremely_annoyingmsg( "  Could not pin j at %d; chose %d instead",
			   data->j_pin, data->j );
  }

  /* I know what you're thinking, punk. You're thinking, did he fire six 
     shots or only five?  Well in all the excitement I've forgotten myself.
     So you have to ask yourself, do I feel lucky?  Well, do you, punk?

     (Thanks to Arthur for the quote.)
     */
  luck_steps = (int)(data->j*data->luck);
  data->k += luck_steps;

  annoyingmsg( "  Adjusting parameters: k=%d j=%d, luck=%d", 
	       data->k, data->j, luck_steps );
  assert( data->j >= 0 );
}

static void stats( old_heap_t *heap, int generation, heap_stats_t *stats )
{
  npsc_data_t *data = DATA(heap);
  int live_los;

  if (generation == data->gen_no) {
    live_los = los_bytes_used( heap->collector->los, data->gen_no );
    stats->np_old = 1;
    stats->copied_last_gc = data->copied_last_gc_old;
    stats->moved_last_gc = data->moved_last_gc_old;
    stats->target = data->stepsize * (data->k-data->j);
    stats->live = used_old( heap );
    stats->semispace1 = data->old->allocated + live_los;

    data->copied_last_gc_old = 0;
    data->moved_last_gc_old = 0;
  }
  else {
    live_los = los_bytes_used( heap->collector->los, data->gen_no+1 );
    stats->np_young = 1;
    stats->copied_last_gc = data->copied_last_gc_young;
    stats->moved_last_gc = data->moved_last_gc_young;
    stats->target = data->stepsize * data->j;
    stats->live = used_young( heap );
    stats->semispace1 = data->young->allocated + live_los;

    data->copied_last_gc_young = 0;
    data->moved_last_gc_young = 0;
  }

  stats->np_j = data->j;
  stats->np_k = data->k;
}

static void before_collection( old_heap_t *heap )
{
  heap->allocated = used_old( heap ) + used_young( heap );
  heap->maximum = DATA(heap)->stepsize * DATA(heap)->k;
}

static void after_collection( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);
  gc_t *gc = heap->collector;

  heap->allocated = used_old( heap ) + used_young( heap );
  heap->maximum = data->stepsize * data->k;

  annoyingmsg( "  Generation %d (non-predictive old):  Size=%d, Live=%d, "
	       "Remset live=%d",
	       data->old->gen_no,
	       data->stepsize * (data->k - data->j), used_old( heap ),
	       gc->remset[ data->old->gen_no ]->live );
  annoyingmsg( "  Generation %d (non-predictive young):  Size=%d, Live=%d, "
	       "Remset live=%d",
	       data->young->gen_no,
	       data->stepsize * data->j, used_young( heap ),
	       gc->remset[ data->young->gen_no ]->live );
  annoyingmsg( "  Non-predictive parameters: k=%d, j=%d, Remset live=%d",
	       data->k, data->j, gc->remset[ gc->np_remset ]->live );
}

static void set_policy( old_heap_t *heap, int op, int value )
{
  npsc_data_t *data = DATA(heap);

  switch (op) {
  case GCCTL_J_FIXED : /* j-fixed */
    data->j_pin = value;
    if (data->j > value) data->j = value;  /* Hack. */
    break;
  case GCCTL_J_PERCENT : /* j-percent */
    data->j_pin = -1;
    data->j_percent = value;
    break;
  }
}

static word *data_load_area( old_heap_t *heap, int nbytes )
{
  npsc_data_t *data = DATA(heap);
  int n;

  assert( nbytes > 0 );
  assert( nbytes % BYTE_ALIGNMENT == 0 );

  n = ss_allocate_and_insert_block( data->old, nbytes );
  return data->old->chunks[ n ].bot;
}

static int used_young( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);

  ss_sync( data->young );
  return
   data->young->used + los_bytes_used( heap->collector->los, data->gen_no+1 );
}

static int used_old( old_heap_t *heap )
{
  npsc_data_t *data = DATA(heap);

  ss_sync( data->old );
  return 
   data->old->used + los_bytes_used( heap->collector->los, data->gen_no );
}

/* Given the amount of live collectable small data (D) and large data (Q),
   return the total amount of memory that may be allocated (including
   that which is currently allocated) before the next collection.
   */
static int compute_dynamic_size( old_heap_t *heap, int D, int Q )
{
  static_heap_t *s = heap->collector->static_area;
  int S = (s ? s->allocated : 0);
  double L = DATA(heap)->load_factor;
  int upper_limit = DATA(heap)->upper_limit;
  int lower_limit = DATA(heap)->lower_limit;

  return gc_compute_dynamic_size( D, S, Q, L, lower_limit, upper_limit );
}

static old_heap_t *allocate_heap( int gen_no, gc_t *gc )
{
  old_heap_t *heap;
  npsc_data_t *data;

  data = (npsc_data_t*)must_malloc( sizeof( npsc_data_t ) );
  heap = create_old_heap_t( "npsc/2/variable",
			    HEAPCODE_OLD_2SPACE_NP,
			    0,               /* initialize */
			    collect,
			    before_collection,
			    after_collection,
			    stats,
			    data_load_area,
			    0,               /* FIXME: load_prepare */
			    0,               /* FIXME: load_data */
			    set_policy,
			    data
			   );
  heap->collector = gc;

  data->gen_no = gen_no;
  data->copied_last_gc_old = 0;
  data->moved_last_gc_old = 0;
  data->copied_last_gc_young = 0;
  data->moved_last_gc_young = 0;

  return heap;
}

/* eof */
