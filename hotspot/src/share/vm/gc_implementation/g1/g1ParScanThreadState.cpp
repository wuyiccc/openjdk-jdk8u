/*
 * Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1OopClosures.inline.hpp"
#include "gc_implementation/g1/g1ParScanThreadState.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oop.pcgc.inline.hpp"
#include "runtime/prefetch.inline.hpp"

G1ParScanThreadState::G1ParScanThreadState(G1CollectedHeap* g1h, uint queue_num, ReferenceProcessor* rp)
  : _g1h(g1h),
    _refs(g1h->task_queue(queue_num)),
    _dcq(&g1h->dirty_card_queue_set()),
    _ct_bs(g1h->g1_barrier_set()),
    _g1_rem(g1h->g1_rem_set()),
    _hash_seed(17), _queue_num(queue_num),
    _term_attempts(0),
    _tenuring_threshold(g1h->g1_policy()->tenuring_threshold()),
    _age_table(false), _scanner(g1h, rp),
    _strong_roots_time(0), _term_time(0) {
  _scanner.set_par_scan_thread_state(this);
  // we allocate G1YoungSurvRateNumRegions plus one entries, since
  // we "sacrifice" entry 0 to keep track of surviving bytes for
  // non-young regions (where the age is -1)
  // We also add a few elements at the beginning and at the end in
  // an attempt to eliminate cache contention
  uint real_length = 1 + _g1h->g1_policy()->young_cset_region_length();
  uint array_length = PADDING_ELEM_NUM +
                      real_length +
                      PADDING_ELEM_NUM;
  _surviving_young_words_base = NEW_C_HEAP_ARRAY(size_t, array_length, mtGC);
  if (_surviving_young_words_base == NULL)
    vm_exit_out_of_memory(array_length * sizeof(size_t), OOM_MALLOC_ERROR,
                          "Not enough space for young surv histo.");
  _surviving_young_words = _surviving_young_words_base + PADDING_ELEM_NUM;
  memset(_surviving_young_words, 0, (size_t) real_length * sizeof(size_t));

  _g1_par_allocator = G1ParGCAllocator::create_allocator(_g1h);

  _dest[InCSetState::NotInCSet]    = InCSetState::NotInCSet;
  // The dest for Young is used when the objects are aged enough to
  // need to be moved to the next space.
  _dest[InCSetState::Young]        = InCSetState::Old;
  _dest[InCSetState::Old]          = InCSetState::Old;

  _start = os::elapsedTime();
}

G1ParScanThreadState::~G1ParScanThreadState() {
  _g1_par_allocator->retire_alloc_buffers();
  delete _g1_par_allocator;
  FREE_C_HEAP_ARRAY(size_t, _surviving_young_words_base, mtGC);
}

void
G1ParScanThreadState::print_termination_stats_hdr(outputStream* const st)
{
  st->print_raw_cr("GC Termination Stats");
  st->print_raw_cr("     elapsed  --strong roots-- -------termination-------"
                   " ------waste (KiB)------");
  st->print_raw_cr("thr     ms        ms      %        ms      %    attempts"
                   "  total   alloc    undo");
  st->print_raw_cr("--- --------- --------- ------ --------- ------ --------"
                   " ------- ------- -------");
}

void
G1ParScanThreadState::print_termination_stats(int i,
                                              outputStream* const st) const
{
  const double elapsed_ms = elapsed_time() * 1000.0;
  const double s_roots_ms = strong_roots_time() * 1000.0;
  const double term_ms    = term_time() * 1000.0;
  const size_t alloc_buffer_waste = _g1_par_allocator->alloc_buffer_waste();
  const size_t undo_waste         = _g1_par_allocator->undo_waste();
  st->print_cr("%3d %9.2f %9.2f %6.2f "
               "%9.2f %6.2f " SIZE_FORMAT_W(8) " "
               SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7) " " SIZE_FORMAT_W(7),
               i, elapsed_ms, s_roots_ms, s_roots_ms * 100 / elapsed_ms,
               term_ms, term_ms * 100 / elapsed_ms, term_attempts(),
               (alloc_buffer_waste + undo_waste) * HeapWordSize / K,
               alloc_buffer_waste * HeapWordSize / K,
               undo_waste * HeapWordSize / K);
}

#ifdef ASSERT
bool G1ParScanThreadState::verify_ref(narrowOop* ref) const {
  assert(ref != NULL, "invariant");
  assert(UseCompressedOops, "sanity");
  assert(!has_partial_array_mask(ref), err_msg("ref=" PTR_FORMAT, p2i(ref)));
  oop p = oopDesc::load_decode_heap_oop(ref);
  assert(_g1h->is_in_g1_reserved(p),
         err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, p2i(ref), p2i(p)));
  return true;
}

bool G1ParScanThreadState::verify_ref(oop* ref) const {
  assert(ref != NULL, "invariant");
  if (has_partial_array_mask(ref)) {
    // Must be in the collection set--it's already been copied.
    oop p = clear_partial_array_mask(ref);
    assert(_g1h->obj_in_cs(p),
           err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, p2i(ref), p2i(p)));
  } else {
    oop p = oopDesc::load_decode_heap_oop(ref);
    assert(_g1h->is_in_g1_reserved(p),
           err_msg("ref=" PTR_FORMAT " p=" PTR_FORMAT, p2i(ref), p2i(p)));
  }
  return true;
}

bool G1ParScanThreadState::verify_task(StarTask ref) const {
  if (ref.is_narrow()) {
    return verify_ref((narrowOop*) ref);
  } else {
    return verify_ref((oop*) ref);
  }
}
#endif // ASSERT

void G1ParScanThreadState::trim_queue() {
  assert(_evac_failure_cl != NULL, "not set");

  StarTask ref;
  do {
    // Drain the overflow stack first, so other threads can steal.
    while (_refs->pop_overflow(ref)) {
      if (!_refs->try_push_to_taskqueue(ref)) {
        dispatch_reference(ref);
      }
    }

    while (_refs->pop_local(ref)) {
      dispatch_reference(ref);
    }
  } while (!_refs->is_empty());
}

HeapWord* G1ParScanThreadState::allocate_in_next_plab(InCSetState const state,
                                                      InCSetState* dest,
                                                      size_t word_sz,
                                                      AllocationContext_t const context) {
  assert(state.is_in_cset_or_humongous(), err_msg("Unexpected state: " CSETSTATE_FORMAT, state.value()));
  assert(dest->is_in_cset_or_humongous(), err_msg("Unexpected dest: " CSETSTATE_FORMAT, dest->value()));

  // Right now we only have two types of regions (young / old) so
  // let's keep the logic here simple. We can generalize it when necessary.
  if (dest->is_young()) {
    HeapWord* const obj_ptr = _g1_par_allocator->allocate(InCSetState::Old,
                                                          word_sz, context);
    if (obj_ptr == NULL) {
      return NULL;
    }
    // Make sure that we won't attempt to copy any other objects out
    // of a survivor region (given that apparently we cannot allocate
    // any new ones) to avoid coming into this slow path.
    _tenuring_threshold = 0;
    dest->set_old();
    return obj_ptr;
  } else {
    assert(dest->is_old(), err_msg("Unexpected dest: " CSETSTATE_FORMAT, dest->value()));
    // no other space to try.
    return NULL;
  }
}

void G1ParScanThreadState::report_promotion_event(InCSetState const dest_state,
                                                  oop const old, size_t word_sz, uint age,
                                                  HeapWord * const obj_ptr,
                                                  AllocationContext_t context) const {
  ParGCAllocBuffer* alloc_buf = _g1_par_allocator->alloc_buffer(dest_state, context);
  if (alloc_buf->contains(obj_ptr)) {
    _g1h->_gc_tracer_stw->report_promotion_in_new_plab_event(old->klass(), word_sz, age,
                                                             dest_state.value() == InCSetState::Old,
                                                             alloc_buf->word_sz());
  } else {
    _g1h->_gc_tracer_stw->report_promotion_outside_plab_event(old->klass(), word_sz, age,
                                                              dest_state.value() == InCSetState::Old);
  }
}

InCSetState G1ParScanThreadState::next_state(InCSetState const state, markOop const m, uint& age) {
  if (state.is_young()) {
    age = !m->has_displaced_mark_helper() ? m->age()
                                          : m->displaced_mark_helper()->age();
    if (age < _tenuring_threshold) {
      return state;
    }
  }
  return dest(state);
}

oop G1ParScanThreadState::copy_to_survivor_space(InCSetState const state,
                                                 oop const old,
                                                 markOop const old_mark) {
  // 计算对象大小
  const size_t word_sz = old->size();
  HeapRegion* const from_region = _g1h->heap_region_containing_raw(old);
  // +1 to make the -1 indexes valid...
  const int young_index = from_region->young_index_in_cset()+1;
  assert( (from_region->is_young() && young_index >  0) ||
         (!from_region->is_young() && young_index == 0), "invariant" );
  const AllocationContext_t context = from_region->allocation_context();

  uint age = 0;
  // 判断对象是要到survivor还是old区, 判断的依据是根据对象age, 这里的ageTable是一个数组, 描述不同的age所用到的总空间,
  // 当发现对象超过晋升的阈值, 或者survivor不能存放的时候需要把对象晋升到老年代分区
  InCSetState dest_state = next_state(state, old_mark, age);
  // 分配一个同样大小的新对象
  HeapWord* obj_ptr = _g1_par_allocator->plab_allocate(dest_state, word_sz, context);

  // PLAB allocations should succeed most of the time, so we'll
  // normally check against NULL once and that's it.
  // 使用PLAB方法直接在PLAB中分配新的对象
  if (obj_ptr == NULL) {
      // 如果分配失败, 则尝试分配一个PLAB或者直接在堆中分配对象, 这里和tlab类似, 先计算是否需要分配一个新的plab, 也是由参数控制,
      // 对于新生代分区, plab的大小为16kb(32位的jvm， 32k为64位jvm), 由YoungPLABSize控制, 对于老年代plab的大小为4KB(32JVM), 由OldPLABSize控制
      // 还有一个参数由ParallelGCBufferWastePct控制, 表示PLAB浪费的比例, 当PLAB剩余的空间小于PLABSize*10%, 即1634或者409字节(新生代/老年代),
      // 可以分配一个新的PLAB, 否则直接在堆中分配, 同样的道理, 如果要分配一个新的PLAB的时候, 需要把PLAB里面碎片的部分填充为dummy对象
    obj_ptr = _g1_par_allocator->allocate_direct_or_new_plab(dest_state, word_sz, context);
    if (obj_ptr == NULL) {
      // 仍然失败, 如果此次尝试是在survivor中, 则再次尝试在老年代区域分配
      // 如果此次尝试是在老年代分配, 则直接报错, 因为上面已经尝试过了
      obj_ptr = allocate_in_next_plab(state, &dest_state, word_sz, context);
      if (obj_ptr == NULL) {
        // This will either forward-to-self, or detect that someone else has
        // installed a forwarding pointer.
        // 还是失败, 说明无法复制对象, 需要把对象头设置为自己
        return _g1h->handle_evacuation_failure_par(this, old);
      }
    }
    if (_g1h->_gc_tracer_stw->should_report_promotion_events()) {
      // The events are checked individually as part of the actual commit
      report_promotion_event(dest_state, old, word_sz, age, obj_ptr, context);
    }
  }

  assert(obj_ptr != NULL, "when we get here, allocation should have succeeded");
#ifndef PRODUCT
  // Should this evacuation fail?
  if (_g1h->evacuation_should_fail()) {
    // Doing this after all the allocation attempts also tests the
    // undo_allocation() method too.
    _g1_par_allocator->undo_allocation(dest_state, obj_ptr, word_sz, context);
    return _g1h->handle_evacuation_failure_par(this, old);
  }
#endif // !PRODUCT

  // We're going to allocate linearly, so might as well prefetch ahead.
  Prefetch::write(obj_ptr, PrefetchCopyIntervalInBytes);
  // 分配
  const oop obj = oop(obj_ptr);
  // 创建前向指针, 如果在中途中有其他线程先进行了复制, 那么它会返回null
  //
  const oop forward_ptr = old->forward_to_atomic(obj);
  if (forward_ptr == NULL) {
  // 如果转发成功, 则执行对象的复制
    Copy::aligned_disjoint_words((HeapWord*) old, obj_ptr, word_sz);
    // 更新age信息和对象头
    if (dest_state.is_young()) {
      if (age < markOopDesc::max_age) {
        age++;
      }
      if (old_mark->has_displaced_mark_helper()) {
        // In this case, we have to install the mark word first,
        // otherwise obj looks to be forwarded (the old mark word,
        // which contains the forward pointer, was copied)
        // 对于重量级锁, 前面的mark word ptr指向的是monitor对象, 其中objectMonitor的第一个字段是oopDes, 所以要先设置old mark 再获得monitor,
        // 最后再更新age
        obj->set_mark(old_mark);
        // 锁对象的mark存储的对应的monitor里面
        markOop new_mark = old_mark->displaced_mark_helper()->set_age(age);
        old_mark->set_displaced_mark_helper(new_mark);
      } else {
        obj->set_mark(old_mark->set_age(age));
      }
      age_table()->add(age, word_sz);
    } else {
      obj->set_mark(old_mark);
    }

    // 把字符串对象送入字符串去重队列, 由去重线程处理
    if (G1StringDedup::is_enabled()) {
      const bool is_from_young = state.is_young();
      const bool is_to_young = dest_state.is_young();
      assert(is_from_young == _g1h->heap_region_containing_raw(old)->is_young(),
             "sanity");
      assert(is_to_young == _g1h->heap_region_containing_raw(obj)->is_young(),
             "sanity");
      G1StringDedup::enqueue_from_evacuation(is_from_young,
                                             is_to_young,
                                             queue_num(),
                                             obj);
    }

    size_t* const surv_young_words = surviving_young_words();
    surv_young_words[young_index] += word_sz;

    // 对于数组对象来说, 由于数组元素对象的长度可能会很长, 超过了ParGCArrayScanChunk(50),
    // 那么这里就把数组原始对象放入到遍历队列中, 而不是直接把数组元素放入到遍历队列中, 防止队列溢出
    if (obj->is_objArray() && arrayOop(obj)->length() >= ParGCArrayScanChunk) {
      // We keep track of the next start index in the length field of
      // the to-space object. The actual length can be found in the
      // length field of the from-space object.
      arrayOop(obj)->set_length(0);
      oop* old_p = set_partial_array_mask(old);
      push_on_queue(old_p);
    } else {
      HeapRegion* const to_region = _g1h->heap_region_containing_raw(obj_ptr);
      _scanner.set_region(to_region);
      // 把obj的每一个Field对象都通过scanner
      // 这里最终会调用klass()->oop_oop_iterate_backwords##nv_suffix(this, blk)
      // G1ParScanClosure::do_oop_nv
      obj->oop_iterate_backwards(&_scanner);
    }
    // 返回复制出的对象的内存地址
    return obj;
  } else {
  // 已经分配过了, 不用再分配了
    _g1_par_allocator->undo_allocation(dest_state, obj_ptr, word_sz, context);
    return forward_ptr;
  }
}
