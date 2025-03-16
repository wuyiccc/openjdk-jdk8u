/*
 * Copyright (c) 1997, 2010, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_AGETABLE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_AGETABLE_HPP

#include "oops/markOop.hpp"
#include "oops/oop.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "runtime/perfData.hpp"

/* Copyright (c) 1992-2009 Oracle and/or its affiliates, and Stanford University.
   See the LICENSE file for license information. */

// Age table for adaptive feedback-mediated tenuring (scavenging)
//
// Note: all sizes are in oops
// 为动态对象年龄判断提供数据
class ageTable VALUE_OBJ_CLASS_SPEC {
  friend class VMStructs;

 public:
  // constants
  enum { table_size = markOopDesc::max_age + 1 };

  // instance variables
  // 数组成员代表某个年龄代对象的总大小, 表长度为 15(年轻代最大年龄) + 1, 需要留一个0号位表示一次都未移动的对象
  // 不过实际上不会统计sizes[0], 因为sizes[0]的值一直为0
  // 这里统计的原因主要是考虑到对象不一定非要等待达到15之后才进入老年代,
  // 为了更好的适应不同程度的内存状况, 虚拟机并不总是要求对象的年龄必须达到MaxTenuringThreshold才能晋升到老年代,
  // 如果在survivor空间中小于等于某个年龄的所有对象空间的总和大于survivor空间的一半, 年龄大于或等于该年龄的对象就可以
  // 直接进入老年代, 而无需等到MaxTenuringThreshold中要求的年龄
  size_t sizes[table_size];

  // constructor.  "global" indicates that this is the global age table
  // (as opposed to gc-thread-local)
  ageTable(bool global = true);

  // clear table
  void clear();

  // add entry
  void add(oop p, size_t oop_size) {
    add(p->age(), oop_size);
  }

  void add(uint age, size_t oop_size) {
    assert(age > 0 && age < table_size, "invalid age of object");
    sizes[age] += oop_size;
  }

  // Merge another age table with the current one.  Used
  // for parallel young generation gc.
  void merge(ageTable* subTable);
  void merge_par(ageTable* subTable);

  // calculate new tenuring threshold based on age information
  uint compute_tenuring_threshold(size_t survivor_capacity, GCTracer &tracer);

 private:
  PerfVariable* _perf_sizes[table_size];
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_AGETABLE_HPP
