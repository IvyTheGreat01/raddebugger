// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Basic Helpers

internal TEX_Topology
tex_topology_make(Vec2S32 dim, R_Tex2DFormat fmt)
{
  TEX_Topology top = {0};
  top.dim.x = (S16)dim.x;
  top.dim.y = (S16)dim.y;
  top.fmt = fmt;
  return top;
}

////////////////////////////////
//~ rjf: Main Layer Initialization

internal void
tex_init(void)
{
  Arena *arena = arena_alloc();
  tex_shared = push_array(arena, TEX_Shared, 1);
  tex_shared->arena = arena;
  tex_shared->slots_count = 1024;
  tex_shared->stripes_count = 64;
  tex_shared->slots = push_array(arena, TEX_Slot, tex_shared->slots_count);
  tex_shared->stripes = push_array(arena, TEX_Stripe, tex_shared->stripes_count);
  tex_shared->stripes_free_nodes = push_array(arena, TEX_Node *, tex_shared->stripes_count);
  for(U64 idx = 0; idx < tex_shared->stripes_count; idx += 1)
  {
    tex_shared->stripes[idx].arena = arena_alloc();
    tex_shared->stripes[idx].rw_mutex = os_rw_mutex_alloc();
    tex_shared->stripes[idx].cv = os_condition_variable_alloc();
  }
  tex_shared->fallback_slots_count = 1024;
  tex_shared->fallback_stripes_count = 64;
  tex_shared->fallback_slots = push_array(arena, TEX_KeyFallbackSlot, tex_shared->fallback_slots_count);
  tex_shared->fallback_stripes = push_array(arena, TEX_Stripe, tex_shared->fallback_stripes_count);
  for(U64 idx = 0; idx < tex_shared->fallback_stripes_count; idx += 1)
  {
    tex_shared->fallback_stripes[idx].arena = arena_alloc();
    tex_shared->fallback_stripes[idx].rw_mutex = os_rw_mutex_alloc();
    tex_shared->fallback_stripes[idx].cv = os_condition_variable_alloc();
  }
  tex_shared->u2x_ring_size = KB(64);
  tex_shared->u2x_ring_base = push_array_no_zero(arena, U8, tex_shared->u2x_ring_size);
  tex_shared->u2x_ring_cv = os_condition_variable_alloc();
  tex_shared->u2x_ring_mutex = os_mutex_alloc();
  tex_shared->xfer_thread_count = Min(4, os_logical_core_count()-1);
  tex_shared->xfer_threads = push_array(arena, OS_Handle, tex_shared->xfer_thread_count);
  for(U64 idx = 0; idx < tex_shared->xfer_thread_count; idx += 1)
  {
    tex_shared->xfer_threads[idx] = os_launch_thread(tex_xfer_thread__entry_point, (void *)idx, 0);
  }
  tex_shared->evictor_thread = os_launch_thread(tex_evictor_thread__entry_point, 0, 0);
}

////////////////////////////////
//~ rjf: Thread Context Initialization

internal void
tex_tctx_ensure_inited(void)
{
  if(tex_tctx == 0)
  {
    Arena *arena = arena_alloc();
    tex_tctx = push_array(arena, TEX_TCTX, 1);
    tex_tctx->arena = arena;
  }
}

////////////////////////////////
//~ rjf: User Clock

internal void
tex_user_clock_tick(void)
{
  ins_atomic_u64_inc_eval(&tex_shared->user_clock_idx);
}

internal U64
tex_user_clock_idx(void)
{
  return ins_atomic_u64_eval(&tex_shared->user_clock_idx);
}

////////////////////////////////
//~ rjf: Scoped Access

internal TEX_Scope *
tex_scope_open(void)
{
  tex_tctx_ensure_inited();
  TEX_Scope *scope = tex_tctx->free_scope;
  if(scope)
  {
    SLLStackPop(tex_tctx->free_scope);
  }
  else
  {
    scope = push_array_no_zero(tex_tctx->arena, TEX_Scope, 1);
  }
  MemoryZeroStruct(scope);
  return scope;
}

internal void
tex_scope_close(TEX_Scope *scope)
{
  for(TEX_Touch *touch = scope->top_touch, *next = 0; touch != 0; touch = next)
  {
    U128 hash = touch->hash;
    next = touch->next;
    U64 slot_idx = hash.u64[1]%tex_shared->slots_count;
    U64 stripe_idx = slot_idx%tex_shared->stripes_count;
    TEX_Slot *slot = &tex_shared->slots[slot_idx];
    TEX_Stripe *stripe = &tex_shared->stripes[stripe_idx];
    OS_MutexScopeR(stripe->rw_mutex)
    {
      for(TEX_Node *n = slot->first; n != 0; n = n->next)
      {
        if(u128_match(hash, n->hash) && MemoryMatchStruct(&touch->topology, &n->topology))
        {
          ins_atomic_u64_dec_eval(&n->scope_ref_count);
          break;
        }
      }
    }
    SLLStackPush(tex_tctx->free_touch, touch);
  }
  SLLStackPush(tex_tctx->free_scope, scope);
}

internal void
tex_scope_touch_node__stripe_r_guarded(TEX_Scope *scope, TEX_Node *node)
{
  TEX_Touch *touch = tex_tctx->free_touch;
  ins_atomic_u64_inc_eval(&node->scope_ref_count);
  ins_atomic_u64_eval_assign(&node->last_time_touched_us, os_now_microseconds());
  ins_atomic_u64_eval_assign(&node->last_user_clock_idx_touched, tex_user_clock_idx());
  if(touch != 0)
  {
    SLLStackPop(tex_tctx->free_touch);
  }
  else
  {
    touch = push_array_no_zero(tex_tctx->arena, TEX_Touch, 1);
  }
  MemoryZeroStruct(touch);
  touch->hash = node->hash;
  touch->topology = node->topology;
  SLLStackPush(scope->top_touch, touch);
}

////////////////////////////////
//~ rjf: Cache Lookups

internal R_Handle
tex_texture_from_key_hash_topology(TEX_Scope *scope, U128 key, U128 hash, TEX_Topology topology)
{
  R_Handle handle = {0};
  if(!u128_match(u128_zero(), hash))
  {
    U64 slot_idx = hash.u64[1]%tex_shared->slots_count;
    U64 stripe_idx = slot_idx%tex_shared->stripes_count;
    TEX_Slot *slot = &tex_shared->slots[slot_idx];
    TEX_Stripe *stripe = &tex_shared->stripes[stripe_idx];
    B32 found = 0;
    B32 stale = 0;
    OS_MutexScopeR(stripe->rw_mutex)
    {
      for(TEX_Node *n = slot->first; n != 0; n = n->next)
      {
        if(u128_match(hash, n->hash) && MemoryMatchStruct(&topology, &n->topology))
        {
          handle = n->texture;
          found = !r_handle_match(r_handle_zero(), handle);
          tex_scope_touch_node__stripe_r_guarded(scope, n);
          break;
        }
      }
    }
    B32 node_is_new = 0;
    if(!found)
    {
      OS_MutexScopeW(stripe->rw_mutex)
      {
        TEX_Node *node = 0;
        for(TEX_Node *n = slot->first; n != 0; n = n->next)
        {
          if(u128_match(hash, n->hash) && MemoryMatchStruct(&topology, &n->topology))
          {
            node = n;
            break;
          }
        }
        if(node == 0)
        {
          node = tex_shared->stripes_free_nodes[stripe_idx];
          if(node)
          {
            SLLStackPop(tex_shared->stripes_free_nodes[stripe_idx]);
          }
          else
          {
            node = push_array_no_zero(stripe->arena, TEX_Node, 1);
          }
          MemoryZeroStruct(node);
          DLLPushBack(slot->first, slot->last, node);
          node->hash = hash;
          MemoryCopyStruct(&node->topology, &topology);
          node_is_new = 1;
        }
      }
    }
    if(node_is_new)
    {
      tex_u2x_enqueue_req(key, hash, topology, max_U64);
    }
    if(r_handle_match(handle, r_handle_zero()))
    {
      U128 fallback_hash = {0};
      U64 fallback_slot_idx = key.u64[1]%tex_shared->fallback_slots_count;
      U64 fallback_stripe_idx = fallback_slot_idx%tex_shared->fallback_stripes_count;
      TEX_KeyFallbackSlot *fallback_slot = &tex_shared->fallback_slots[fallback_slot_idx];
      TEX_Stripe *fallback_stripe = &tex_shared->fallback_stripes[fallback_stripe_idx];
      OS_MutexScopeR(fallback_stripe->rw_mutex) for(TEX_KeyFallbackNode *n = fallback_slot->first; n != 0; n = n->next)
      {
        if(u128_match(key, n->key))
        {
          fallback_hash = n->hash;
          break;
        }
      }
      if(!u128_match(fallback_hash, u128_zero()))
      {
        U64 retry_slot_idx = fallback_hash.u64[1]%tex_shared->slots_count;
        U64 retry_stripe_idx = retry_slot_idx%tex_shared->stripes_count;
        TEX_Slot *retry_slot = &tex_shared->slots[retry_slot_idx];
        TEX_Stripe *retry_stripe = &tex_shared->stripes[retry_stripe_idx];
        OS_MutexScopeR(retry_stripe->rw_mutex)
        {
          for(TEX_Node *n = retry_slot->first; n != 0; n = n->next)
          {
            if(u128_match(fallback_hash, n->hash) && MemoryMatchStruct(&topology, &n->topology))
            {
              handle = n->texture;
              tex_scope_touch_node__stripe_r_guarded(scope, n);
              break;
            }
          }
        }
      }
    }
  }
  return handle;
}

////////////////////////////////
//~ rjf: Transfer Threads

internal B32
tex_u2x_enqueue_req(U128 key, U128 hash, TEX_Topology top, U64 endt_us)
{
  B32 good = 0;
  OS_MutexScope(tex_shared->u2x_ring_mutex) for(;;)
  {
    U64 unconsumed_size = tex_shared->u2x_ring_write_pos-tex_shared->u2x_ring_read_pos;
    U64 available_size = tex_shared->u2x_ring_size-unconsumed_size;
    if(available_size >= sizeof(key)+sizeof(hash)+sizeof(top))
    {
      good = 1;
      tex_shared->u2x_ring_write_pos += ring_write_struct(tex_shared->u2x_ring_base, tex_shared->u2x_ring_size, tex_shared->u2x_ring_write_pos, &key);
      tex_shared->u2x_ring_write_pos += ring_write_struct(tex_shared->u2x_ring_base, tex_shared->u2x_ring_size, tex_shared->u2x_ring_write_pos, &hash);
      tex_shared->u2x_ring_write_pos += ring_write_struct(tex_shared->u2x_ring_base, tex_shared->u2x_ring_size, tex_shared->u2x_ring_write_pos, &top);
      break;
    }
    if(os_now_microseconds() >= endt_us)
    {
      break;
    }
    os_condition_variable_wait(tex_shared->u2x_ring_cv, tex_shared->u2x_ring_mutex, endt_us);
  }
  if(good)
  {
    os_condition_variable_broadcast(tex_shared->u2x_ring_cv);
  }
  return good;
}

internal void
tex_u2x_dequeue_req(U128 *key_out, U128 *hash_out, TEX_Topology *top_out)
{
  OS_MutexScope(tex_shared->u2x_ring_mutex) for(;;)
  {
    U64 unconsumed_size = tex_shared->u2x_ring_write_pos-tex_shared->u2x_ring_read_pos;
    if(unconsumed_size >= sizeof(*key_out)+sizeof(*hash_out)+sizeof(*top_out))
    {
      tex_shared->u2x_ring_read_pos += ring_read_struct(tex_shared->u2x_ring_base, tex_shared->u2x_ring_size, tex_shared->u2x_ring_read_pos, key_out);
      tex_shared->u2x_ring_read_pos += ring_read_struct(tex_shared->u2x_ring_base, tex_shared->u2x_ring_size, tex_shared->u2x_ring_read_pos, hash_out);
      tex_shared->u2x_ring_read_pos += ring_read_struct(tex_shared->u2x_ring_base, tex_shared->u2x_ring_size, tex_shared->u2x_ring_read_pos, top_out);
      break;
    }
    os_condition_variable_wait(tex_shared->u2x_ring_cv, tex_shared->u2x_ring_mutex, max_U64);
  }
  os_condition_variable_broadcast(tex_shared->u2x_ring_cv);
}

internal void
tex_xfer_thread__entry_point(void *p)
{
  TCTX tctx_ = {0};
  tctx_init_and_equip(&tctx_);
  for(;;)
  {
    HS_Scope *scope = hs_scope_open();
    
    //- rjf: decode
    U128 key = {0};
    U128 hash = {0};
    TEX_Topology top = {0};
    tex_u2x_dequeue_req(&key, &hash, &top);
    
    //- rjf: unpack hash
    U64 slot_idx = hash.u64[1]%tex_shared->slots_count;
    U64 stripe_idx = slot_idx%tex_shared->stripes_count;
    TEX_Slot *slot = &tex_shared->slots[slot_idx];
    TEX_Stripe *stripe = &tex_shared->stripes[stripe_idx];
    
    //- rjf: take task
    B32 got_task = 0;
    OS_MutexScopeR(stripe->rw_mutex)
    {
      for(TEX_Node *n = slot->first; n != 0; n = n->next)
      {
        if(u128_match(n->hash, hash) && MemoryMatchStruct(&top, &n->topology))
        {
          got_task = !ins_atomic_u32_eval_cond_assign(&n->is_working, 1, 0);
          break;
        }
      }
    }
    
    //- rjf: hash -> data
    String8 data = {0};
    if(got_task)
    {
      data = hs_data_from_hash(scope, hash);
    }
    
    //- rjf: data * topology -> texture
    R_Handle texture = {0};
    if(got_task && top.dim.x != 0 && top.dim.y != 0 && data.size >= (U64)top.dim.x*(U64)top.dim.y*r_tex2d_format_bytes_per_pixel_table[top.fmt])
    {
      texture = r_tex2d_alloc(R_Tex2DKind_Static, v2s32(top.dim.x, top.dim.y), top.fmt, data.str);
    }
    
    //- rjf: commit results to cache
    if(got_task) OS_MutexScopeW(stripe->rw_mutex)
    {
      for(TEX_Node *n = slot->first; n != 0; n = n->next)
      {
        if(u128_match(n->hash, hash) && MemoryMatchStruct(&top, &n->topology))
        {
          n->texture = texture;
          ins_atomic_u32_eval_assign(&n->is_working, 0);
          ins_atomic_u64_inc_eval(&n->load_count);
          break;
        }
      }
    }
    
    //- rjf: commit this key/hash pair to fallback cache
    if(got_task && !u128_match(key, u128_zero()) && !u128_match(hash, u128_zero()))
    {
      U64 fallback_slot_idx = key.u64[1]%tex_shared->fallback_slots_count;
      U64 fallback_stripe_idx = fallback_slot_idx%tex_shared->fallback_stripes_count;
      TEX_KeyFallbackSlot *fallback_slot = &tex_shared->fallback_slots[fallback_slot_idx];
      TEX_Stripe *fallback_stripe = &tex_shared->fallback_stripes[fallback_stripe_idx];
      OS_MutexScopeW(fallback_stripe->rw_mutex)
      {
        TEX_KeyFallbackNode *node = 0;
        for(TEX_KeyFallbackNode *n = fallback_slot->first; n != 0; n = n->next)
        {
          if(u128_match(n->key, key))
          {
            node = n;
            break;
          }
        }
        if(node == 0)
        {
          node = push_array(fallback_stripe->arena, TEX_KeyFallbackNode, 1);
          SLLQueuePush(fallback_slot->first, fallback_slot->last, node);
        }
        node->key = key;
        node->hash = hash;
      }
    }
    
    hs_scope_close(scope);
  }
}

////////////////////////////////
//~ rjf: Evictor Threads

internal void
tex_evictor_thread__entry_point(void *p)
{
  for(;;)
  {
    U64 check_time_us = os_now_microseconds();
    U64 check_time_user_clocks = tex_user_clock_idx();
    U64 evict_threshold_us = 10*1000000;
    U64 evict_threshold_user_clocks = 10;
    for(U64 slot_idx = 0; slot_idx < tex_shared->slots_count; slot_idx += 1)
    {
      U64 stripe_idx = slot_idx%tex_shared->stripes_count;
      TEX_Slot *slot = &tex_shared->slots[slot_idx];
      TEX_Stripe *stripe = &tex_shared->stripes[stripe_idx];
      B32 slot_has_work = 0;
      OS_MutexScopeR(stripe->rw_mutex)
      {
        for(TEX_Node *n = slot->first; n != 0; n = n->next)
        {
          if(n->scope_ref_count == 0 &&
             n->last_time_touched_us+evict_threshold_us <= check_time_us &&
             n->last_user_clock_idx_touched+evict_threshold_user_clocks <= check_time_user_clocks &&
             n->load_count != 0 &&
             n->is_working == 0)
          {
            slot_has_work = 1;
            break;
          }
        }
      }
      if(slot_has_work) OS_MutexScopeW(stripe->rw_mutex)
      {
        for(TEX_Node *n = slot->first, *next = 0; n != 0; n = next)
        {
          next = n->next;
          if(n->scope_ref_count == 0 &&
             n->last_time_touched_us+evict_threshold_us <= check_time_us &&
             n->last_user_clock_idx_touched+evict_threshold_user_clocks <= check_time_user_clocks &&
             n->load_count != 0 &&
             n->is_working == 0)
          {
            DLLRemove(slot->first, slot->last, n);
            if(!r_handle_match(n->texture, r_handle_zero()))
            {
              r_tex2d_release(n->texture);
            }
            SLLStackPush(tex_shared->stripes_free_nodes[stripe_idx], n);
          }
        }
      }
      os_sleep_milliseconds(5);
    }
    os_sleep_milliseconds(1000);
  }
}
