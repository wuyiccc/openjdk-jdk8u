/*
 * Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/javaClasses.hpp"
#include "memory/referencePolicy.hpp"
#include "memory/universe.hpp"
#include "runtime/arguments.hpp"
#include "runtime/globals.hpp"

LRUCurrentHeapPolicy::LRUCurrentHeapPolicy() {
  setup();
}

// Capture state (of-the-VM) information needed to evaluate the policy
void LRUCurrentHeapPolicy::setup() {
// SoftRefLRUPolicyMSPerMB的默认值为1000, 其实就是1000ms/MB=1s/MB, 也就是说上次gc后的可用堆大小如果是10mb,
// 那么_max_interval的值就是10s, 根据should_clear_reference()函数的判断逻辑, 软引用至少可以存活10s的时间
  _max_interval = (Universe::get_heap_free_at_last_gc() / M) * SoftRefLRUPolicyMSPerMB;
  assert(_max_interval >= 0,"Sanity check");
}

// The oop passed in is the SoftReference object, and not
// the object the SoftReference points to.
bool LRUCurrentHeapPolicy::should_clear_reference(oop p,
                                                  jlong timestamp_clock) {
  jlong interval = timestamp_clock - java_lang_ref_SoftReference::timestamp(p);
  assert(interval >= 0, "Sanity check");

  // The interval will be zero if the ref was accessed since the last scavenge/gc.
  if(interval <= _max_interval) {
    return false;
  }

  return true;
}

/////////////////////// MaxHeap //////////////////////

LRUMaxHeapPolicy::LRUMaxHeapPolicy() {
  setup();
}

// Capture state (of-the-VM) information needed to evaluate the policy
void LRUMaxHeapPolicy::setup() {
  size_t max_heap = MaxHeapSize;
  max_heap -= Universe::get_heap_used_at_last_gc();
  max_heap /= M;
  // 与current不同的是, 这里计算的是堆的差值
  _max_interval = max_heap * SoftRefLRUPolicyMSPerMB;
  assert(_max_interval >= 0,"Sanity check");
}

// The oop passed in is the SoftReference object, and not
// the object the SoftReference points to.
bool LRUMaxHeapPolicy::should_clear_reference(oop p,
                                             jlong timestamp_clock) {
  // 获取SoftReference.timestamp字段的值, 这个值在调用get方法的时候可能会更新为SoftReference.clock字段的值,
  // 因此timestamp_clock可能与timestamp的值相同
  // timestamp_clock与SoftReference静态字段clock字段的值相同, 他们都保存着上一次gc处理完软引用后的时间,
  jlong interval = timestamp_clock - java_lang_ref_SoftReference::timestamp(p);
  assert(interval >= 0, "Sanity check");
  //
  // The interval will be zero if the ref was accessed since the last scavenge/gc.
  // interval可以表示软引用在多长时间内没有被使用了, 如果内存不紧张, 软引用存活的时间_max_interval可以很长,
  // 如果内存紧张, 那么_max_interval时间会变短, 这样当interval大于等于_max_interval的时候就会被回收了
  if(interval <= _max_interval) {
    return false;
  }

  return true;
}
