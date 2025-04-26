/*
 * Copyright (c) 2005, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_OOPS_OOP_PCGC_INLINE_HPP
#define SHARE_VM_OOPS_OOP_PCGC_INLINE_HPP

#include "utilities/macros.hpp"
#if INCLUDE_ALL_GCS
#include "gc_implementation/parNew/parNewGeneration.hpp"
#include "gc_implementation/parallelScavenge/parallelScavengeHeap.hpp"
#include "gc_implementation/parallelScavenge/psCompactionManager.hpp"
#include "gc_implementation/parallelScavenge/psParallelCompact.hpp"
#include "gc_implementation/parallelScavenge/psScavenge.hpp"
#include "gc_implementation/parallelScavenge/psScavenge.inline.hpp"
#endif // INCLUDE_ALL_GCS

inline void oopDesc::update_contents(ParCompactionManager* cm) {
  // The klass field must be updated before anything else
  // can be done.
  DEBUG_ONLY(Klass* original_klass = klass());

  Klass* new_klass = klass();
  if (!new_klass->oop_is_typeArray()) {
    // It might contain oops beyond the header, so take the virtual call.
    new_klass->oop_update_pointers(cm, this);
  }
  // Else skip it.  The TypeArrayKlass in the header never needs scavenging.
}

inline void oopDesc::follow_contents(ParCompactionManager* cm) {
  assert (PSParallelCompact::mark_bitmap()->is_marked(this),
    "should be marked");
  klass()->oop_follow_contents(cm, this);
}

inline oop oopDesc::forward_to_atomic(oop p) {
  assert(ParNewGeneration::is_legal_forward_ptr(p),
         "illegal forwarding pointer value.");
  markOop oldMark = mark();
  //这里利用新对象的地址构建处一个markOopDesc (ptr|11)
  // 这里其实是将p地址的值作为了markOopDesc, 本质上markOopDesc也是一个uintptr_t类型, 所以直接存储p地址是没问题了
  // 然后因为地址对齐的缘故, p地址的低2位一定为0(可以用来存储其他数据), 所以将这2位设置为1(设置标记位), 代表已复制
  markOop forwardPtrMark = markOopDesc::encode_pointer_as_mark(p);
  markOop curMark;

  assert(forwardPtrMark->decode_pointer() == p, "encoding must be reversable");
  assert(sizeof(markOop) == sizeof(intptr_t), "CAS below requires this.");

  while (!oldMark->is_marked()) {
    // 写入被复制对象的mark
    curMark = (markOop)Atomic::cmpxchg_ptr(forwardPtrMark, &_mark, oldMark);
    assert(is_forwarded(), "object should have been forwarded");
    if (curMark == oldMark) {
      return NULL;
    }
    // If the CAS was unsuccessful then curMark->is_marked()
    // should return true as another thread has CAS'd in another
    // forwarding pointer.
    oldMark = curMark;
  }
  return forwardee();
}

#endif // SHARE_VM_OOPS_OOP_PCGC_INLINE_HPP
