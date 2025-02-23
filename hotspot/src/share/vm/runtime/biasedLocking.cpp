/*
 * Copyright (c) 2005, 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "oops/klass.inline.hpp"
#include "oops/markOop.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/task.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vm_operations.hpp"
#include "jfr/support/jfrThreadId.hpp"
#include "jfr/jfrEvents.hpp"

static bool _biased_locking_enabled = false;
BiasedLockingCounters BiasedLocking::_counters;

static GrowableArray<Handle>*  _preserved_oop_stack  = NULL;
static GrowableArray<markOop>* _preserved_mark_stack = NULL;

static void enable_biased_locking(Klass* k) {
  k->set_prototype_header(markOopDesc::biased_locking_prototype());
}

class VM_EnableBiasedLocking: public VM_Operation {
 private:
  bool _is_cheap_allocated;
 public:
  VM_EnableBiasedLocking(bool is_cheap_allocated) { _is_cheap_allocated = is_cheap_allocated; }
  VMOp_Type type() const          { return VMOp_EnableBiasedLocking; }
  Mode evaluation_mode() const    { return _is_cheap_allocated ? _async_safepoint : _safepoint; }
  bool is_cheap_allocated() const { return _is_cheap_allocated; }

  void doit() {
    // Iterate the system dictionary enabling biased locking for all
    // currently loaded classes
    SystemDictionary::classes_do(enable_biased_locking);
    // Indicate that future instances should enable it as well
    _biased_locking_enabled = true;

    if (TraceBiasedLocking) {
      tty->print_cr("Biased locking enabled");
    }
  }

  bool allow_nested_vm_operations() const        { return false; }
};


// One-shot PeriodicTask subclass for enabling biased locking
class EnableBiasedLockingTask : public PeriodicTask {
 public:
  EnableBiasedLockingTask(size_t interval_time) : PeriodicTask(interval_time) {}

  virtual void task() {
    // Use async VM operation to avoid blocking the Watcher thread.
    // VM Thread will free C heap storage.
    VM_EnableBiasedLocking *op = new VM_EnableBiasedLocking(true);
    VMThread::execute(op);

    // Reclaim our storage and disenroll ourself
    delete this;
  }
};


void BiasedLocking::init() {
  // If biased locking is enabled, schedule a task to fire a few
  // seconds into the run which turns on biased locking for all
  // currently loaded classes as well as future ones. This is a
  // workaround for startup time regressions due to a large number of
  // safepoints being taken during VM startup for bias revocation.
  // Ideally we would have a lower cost for individual bias revocation
  // and not need a mechanism like this.
  if (UseBiasedLocking) {
    if (BiasedLockingStartupDelay > 0) {
      EnableBiasedLockingTask* task = new EnableBiasedLockingTask(BiasedLockingStartupDelay);
      task->enroll();
    } else {
      VM_EnableBiasedLocking op(false);
      VMThread::execute(&op);
    }
  }
}


bool BiasedLocking::enabled() {
  return _biased_locking_enabled;
}

// Returns MonitorInfos for all objects locked on this thread in youngest to oldest order
static GrowableArray<MonitorInfo*>* get_or_compute_monitor_info(JavaThread* thread) {
  GrowableArray<MonitorInfo*>* info = thread->cached_monitor_info();
  if (info != NULL) {
    return info;
  }

  info = new GrowableArray<MonitorInfo*>();

  // It's possible for the thread to not have any Java frames on it,
  // i.e., if it's the main thread and it's already returned from main()
  if (thread->has_last_Java_frame()) {
    RegisterMap rm(thread);
    for (javaVFrame* vf = thread->last_java_vframe(&rm); vf != NULL; vf = vf->java_sender()) {
      GrowableArray<MonitorInfo*> *monitors = vf->monitors();
      if (monitors != NULL) {
        int len = monitors->length();
        // Walk monitors youngest to oldest
        for (int i = len - 1; i >= 0; i--) {
          MonitorInfo* mon_info = monitors->at(i);
          if (mon_info->eliminated()) continue;
          oop owner = mon_info->owner();
          if (owner != NULL) {
            info->append(mon_info);
          }
        }
      }
    }
  }

  thread->set_cached_monitor_info(info);
  return info;
}

// After the call, *biased_locker will be set to obj->mark()->biased_locker() if biased_locker != NULL,
// AND it is a living thread. Otherwise it will not be updated, (i.e. the caller is responsible for initialization).
static BiasedLocking::Condition revoke_bias(oop obj, bool allow_rebias, bool is_bulk, JavaThread* requesting_thread, JavaThread** biased_locker) {
  markOop mark = obj->mark();
  if (!mark->has_bias_pattern()) {
    if (TraceBiasedLocking) {
      ResourceMark rm;
      tty->print_cr("  (Skipping revocation of object of type %s because it's no longer biased)",
                    obj->klass()->external_name());
    }
    return BiasedLocking::NOT_BIASED;
  }

  uint age = mark->age();
  // 构建一个偏向模式的markOop(101)
  markOop   biased_prototype = markOopDesc::biased_locking_prototype()->set_age(age);
  // 构建一个无锁模式的markOop(001)
  markOop unbiased_prototype = markOopDesc::prototype()->set_age(age);

  if (TraceBiasedLocking && (Verbose || !is_bulk)) {
    ResourceMark rm;
    tty->print_cr("Revoking bias of object " INTPTR_FORMAT " , mark " INTPTR_FORMAT " , type %s , prototype header " INTPTR_FORMAT " , allow rebias %d , requesting thread " INTPTR_FORMAT,
                  p2i((void *)obj), (intptr_t) mark, obj->klass()->external_name(), (intptr_t) obj->klass()->prototype_header(), (allow_rebias ? 1 : 0), (intptr_t) requesting_thread);
  }

  JavaThread* biased_thread = mark->biased_locker();
  if (biased_thread == NULL) {
    // Object is anonymously biased. We can get here if, for
    // example, we revoke the bias due to an identity hash code
    // being computed for an object.
    if (!allow_rebias) {
      obj->set_mark(unbiased_prototype);
    }
    if (TraceBiasedLocking && (Verbose || !is_bulk)) {
      tty->print_cr("  Revoked bias of anonymously-biased object");
    }
    return BiasedLocking::BIAS_REVOKED;
  }

  // Handle case where the thread toward which the object was biased has exited
  bool thread_is_alive = false;
  if (requesting_thread == biased_thread) {
    thread_is_alive = true;
  } else {
    for (JavaThread* cur_thread = Threads::first(); cur_thread != NULL; cur_thread = cur_thread->next()) {
      if (cur_thread == biased_thread) {
        thread_is_alive = true;
        break;
      }
    }
  }
  // 如果线程已经不存活了
  if (!thread_is_alive) {
    if (allow_rebias) {
      // 如果可重偏向则将对象设置为匿名偏向状态
      obj->set_mark(biased_prototype);
    } else {
      // 否则设置为无锁状态
      obj->set_mark(unbiased_prototype);
    }
    if (TraceBiasedLocking && (Verbose || !is_bulk)) {
      tty->print_cr("  Revoked bias of object biased toward dead thread");
    }
    return BiasedLocking::BIAS_REVOKED;
  }

  // Thread owning bias is alive.
  // Check to see whether it currently owns the lock and, if so,
  // write down the needed displaced headers to the thread's stack.
  // Otherwise, restore the object's header either to the unlocked
  // or unbiased state.
  // 如果还有线程存活, 则遍历线程栈中所有的lock record
  GrowableArray<MonitorInfo*>* cached_monitor_info = get_or_compute_monitor_info(biased_thread);
  BasicLock* highest_lock = NULL;
  for (int i = 0; i < cached_monitor_info->length(); i++) {
    MonitorInfo* mon_info = cached_monitor_info->at(i);
    // 如果能找到对应的lock record, 说明偏向所有者正在持有锁
    if (mon_info->owner() == obj) {
      if (TraceBiasedLocking && Verbose) {
        tty->print_cr("   mon_info->owner (" PTR_FORMAT ") == obj (" PTR_FORMAT ")",
                      p2i((void *) mon_info->owner()),
                      p2i((void *) obj));
      }
      // Assume recursive case and fix up highest lock later
      // 升级为轻量级锁, 修改栈中所有关联该所的lock record
      // 先处理所有锁重入的情况, 轻量级锁的displaced mark word为null (00), 表示锁重入
      markOop mark = markOopDesc::encode((BasicLock*) NULL);
      highest_lock = mon_info->lock();
      highest_lock->set_displaced_header(mark);
    } else {
      if (TraceBiasedLocking && Verbose) {
        tty->print_cr("   mon_info->owner (" PTR_FORMAT ") != obj (" PTR_FORMAT ")",
                      p2i((void *) mon_info->owner()),
                      p2i((void *) obj));
      }
    }
  }
  if (highest_lock != NULL) {
    // Fix up highest lock to contain displaced header and point
    // object at it
    highest_lock->set_displaced_header(unbiased_prototype);
    // Reset object header to point to displaced mark.
    // Must release storing the lock address for platforms without TSO
    // ordering (e.g. ppc).
    obj->release_set_mark(markOopDesc::encode(highest_lock));
    assert(!obj->mark()->has_bias_pattern(), "illegal mark state: stack lock used bias bit");
    if (TraceBiasedLocking && (Verbose || !is_bulk)) {
      tty->print_cr("  Revoked bias of currently-locked object");
    }
  } else {
    if (TraceBiasedLocking && (Verbose || !is_bulk)) {
      tty->print_cr("  Revoked bias of currently-unlocked object");
    }
    // 偏向锁的所有者(线程)没有正在持有锁

    if (allow_rebias) {
      // 设置为匿名偏向状态
      obj->set_mark(biased_prototype);
    } else {
      // 否则设置为无锁状态
      // Store the unlocked value into the object's header.
      obj->set_mark(unbiased_prototype);
    }
  }

#if INCLUDE_JFR
  // If requested, return information on which thread held the bias
  if (biased_locker != NULL) {
    *biased_locker = biased_thread;
  }
#endif // INCLUDE_JFR

  return BiasedLocking::BIAS_REVOKED;
}


enum HeuristicsResult {
  HR_NOT_BIASED    = 1,
  HR_SINGLE_REVOKE = 2,
  HR_BULK_REBIAS   = 3,
  HR_BULK_REVOKE   = 4
};


static HeuristicsResult update_heuristics(oop o, bool allow_rebias) {
  markOop mark = o->mark();
  // 如果不是偏向锁模式, 直接返回
  if (!mark->has_bias_pattern()) {
    return HR_NOT_BIASED;
  }

  // Heuristics to attempt to throttle the number of revocations.
  // Stages:
  // 1. Revoke the biases of all objects in the heap of this type,
  //    but allow rebiasing of those objects if unlocked.
  // 2. Revoke the biases of all objects in the heap of this type
  //    and don't allow rebiasing of these objects. Disable
  //    allocation of objects of that type with the bias bit set.
  // 获取锁对象的klass数据信息
  Klass* k = o->klass();
  // 当前时间
  jlong cur_time = os::javaTimeMillis();
  // 该类上一次批量重偏向的时间
  jlong last_bulk_revocation_time = k->last_biased_lock_bulk_revocation_time();
  // 该类单个偏向撤销的计数
  int revocation_count = k->biased_lock_revocation_count();
  // 按默认参数来说:
  // 如果撤销计数>=20 && 撤销计数 < 40 && 距上次批量撤销的时间>=25s, 就会重置计数
  if ((revocation_count >= BiasedLockingBulkRebiasThreshold) &&
      (revocation_count <  BiasedLockingBulkRevokeThreshold) &&
      (last_bulk_revocation_time != 0) &&
      (cur_time - last_bulk_revocation_time >= BiasedLockingDecayTime)) {
    // This is the first revocation we've seen in a while of an
    // object of this type since the last time we performed a bulk
    // rebiasing operation. The application is allocating objects in
    // bulk which are biased toward a thread and then handing them
    // off to another thread. We can cope with this allocation
    // pattern via the bulk rebiasing mechanism so we reset the
    // klass's revocation count rather than allow it to increase
    // monotonically. If we see the need to perform another bulk
    // rebias operation later, we will, and if subsequently we see
    // many more revocation operations in a short period of time we
    // will completely disable biasing for this type.
    k->set_biased_lock_revocation_count(0);
    revocation_count = 0;
  }

  // Make revocation count saturate just beyond BiasedLockingBulkRevokeThreshold
  if (revocation_count <= BiasedLockingBulkRevokeThreshold) {
    // 自增计数
    revocation_count = k->atomic_incr_biased_lock_revocation_count();
  }

  if (revocation_count == BiasedLockingBulkRevokeThreshold) {
    // 此时, 如果达到批量撤销阈值, 则进行批量撤销
    return HR_BULK_REVOKE;
  }

  if (revocation_count == BiasedLockingBulkRebiasThreshold) {
    // 如果达到批量重偏向阈值, 则进行批量重偏向
    return HR_BULK_REBIAS;
  }
  // 否则, 仅进行单个对象的撤销偏向
  return HR_SINGLE_REVOKE;
}


static BiasedLocking::Condition bulk_revoke_or_rebias_at_safepoint(oop o,
                                                                   bool bulk_rebias,
                                                                   bool attempt_rebias_of_object,
                                                                   JavaThread* requesting_thread) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be done at safepoint");

  if (TraceBiasedLocking) {
    tty->print_cr("* Beginning bulk revocation (kind == %s) because of object "
                  INTPTR_FORMAT " , mark " INTPTR_FORMAT " , type %s",
                  (bulk_rebias ? "rebias" : "revoke"),
                  p2i((void *) o), (intptr_t) o->mark(), o->klass()->external_name());
  }

  jlong cur_time = os::javaTimeMillis();
  // 将这次重偏向时间写入klass的元数据中, 作为下次触发批量重偏向或批量撤销的启发条件之一
  o->klass()->set_last_biased_lock_bulk_revocation_time(cur_time);


  Klass* k_o = o->klass();
  Klass* klass = k_o;

  if (bulk_rebias) {
    // 进行批量重偏向
    // Use the epoch in the klass of the object to implicitly revoke
    // all biases of objects of this data type and force them to be
    // reacquired. However, we also need to walk the stacks of all
    // threads and update the headers of lightweight locked objects
    // with biases to have the current epoch.

    // If the prototype header doesn't have the bias pattern, don't
    // try to update the epoch -- assume another VM operation came in
    // and reset the header to the unbiased state, which will
    // implicitly cause all existing biases to be revoked
    // 类的默认对象头开启了偏向模型, 才能批量重偏向, 如果不是偏向模式则不能进入这一代码块
    if (klass->prototype_header()->has_bias_pattern()) {
      // 自增类的epoch 锁偏向时间
      int prev_epoch = klass->prototype_header()->bias_epoch();
      // 对klass记录的epoch进行自增
      klass->set_prototype_header(klass->prototype_header()->incr_bias_epoch());
      // 获取类自增后的epoch
      int cur_epoch = klass->prototype_header()->bias_epoch();

      // Now walk all threads' stacks and adjust epochs of any biased
      // and locked objects of this data type we encounter
      // 遍历所有的线程
      for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {
        GrowableArray<MonitorInfo*>* cached_monitor_info = get_or_compute_monitor_info(thr);
        // 遍历所有线程的锁记录
        for (int i = 0; i < cached_monitor_info->length(); i++) {
          MonitorInfo* mon_info = cached_monitor_info->at(i);
          oop owner = mon_info->owner();
          markOop mark = owner->mark();

          // 找到所有当前类的偏向锁对象
          if ((owner->klass() == k_o) && mark->has_bias_pattern()) {
            // We might have encountered this object already in the case of recursive locking
            assert(mark->bias_epoch() == prev_epoch || mark->bias_epoch() == cur_epoch, "error in bias epoch adjustment");
            // 更新该类的偏向锁对象的epoch与类的epoch保持一致
            owner->set_mark(mark->set_bias_epoch(cur_epoch));
          }
        }
      }
    }

    // At this point we're done. All we have to do is potentially
    // adjust the header of the given object to revoke its bias.
    // 这一步调用撤销偏向的方法, 在这里将对象设置为匿名偏向状态(101)
    revoke_bias(o, attempt_rebias_of_object && klass->prototype_header()->has_bias_pattern(), true, requesting_thread, NULL);
  } else {
    if (TraceBiasedLocking) {
      ResourceMark rm;
      tty->print_cr("* Disabling biased locking for type %s", klass->external_name());
    }

    // Disable biased locking for this data type. Not only will this
    // cause future instances to not be biased, but existing biased
    // instances will notice that this implicitly caused their biases
    // to be revoked.
    // 批量撤销逻辑
    // 首先禁用klass存储的对象头中的可偏向属性
    // markOopDesc::prototype() 返回的是一个关闭偏向模式的prototype
    klass->set_prototype_header(markOopDesc::prototype());

    // Now walk all threads' stacks and forcibly revoke the biases of
    // any locked and biased objects of this data type we encounter.
    // 遍历所有线程的栈, 撤销该类正在被持有的偏向锁为轻量级锁
    for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {
      GrowableArray<MonitorInfo*>* cached_monitor_info = get_or_compute_monitor_info(thr);
      for (int i = 0; i < cached_monitor_info->length(); i++) {
        MonitorInfo* mon_info = cached_monitor_info->at(i);
        oop owner = mon_info->owner();
        markOop mark = owner->mark();
        if ((owner->klass() == k_o) && mark->has_bias_pattern()) {
          // 这里allow_rebias=false, 设置为不可重偏向
          revoke_bias(owner, false, true, requesting_thread, NULL);
        }
      }
    }

    // Must force the bias of the passed object to be forcibly revoked
    // as well to ensure guarantees to callers
    // 当前锁对象可能未被任何线程持有
    // 所以这里单独进行撤销, 以确保调用方的撤销语义
    revoke_bias(o, false, true, requesting_thread, NULL);
  }

  if (TraceBiasedLocking) {
    tty->print_cr("* Ending bulk revocation");
  }

  BiasedLocking::Condition status_code = BiasedLocking::BIAS_REVOKED;
  // 如果满足条件, 则直接将锁重偏向于当前线程
  if (attempt_rebias_of_object &&
      o->mark()->has_bias_pattern() &&
      klass->prototype_header()->has_bias_pattern()) {
    // 重新设置markOop的数据结构, threadId为请求线程的id, 设置age, epoch版本
    markOop new_mark = markOopDesc::encode(requesting_thread, o->mark()->age(),
                                           klass->prototype_header()->bias_epoch());
    // 将新的markOop设置给obj锁对象
    o->set_mark(new_mark);
    status_code = BiasedLocking::BIAS_REVOKED_AND_REBIASED;
    if (TraceBiasedLocking) {
      tty->print_cr("  Rebiased object toward thread " INTPTR_FORMAT, (intptr_t) requesting_thread);
    }
  }

  assert(!o->mark()->has_bias_pattern() ||
         (attempt_rebias_of_object && (o->mark()->biased_locker() == requesting_thread)),
         "bug in bulk bias revocation");

  return status_code;
}


static void clean_up_cached_monitor_info() {
  // Walk the thread list clearing out the cached monitors
  for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {
    thr->set_cached_monitor_info(NULL);
  }
}


class VM_RevokeBias : public VM_Operation {
protected:
  Handle* _obj;
  GrowableArray<Handle>* _objs;
  JavaThread* _requesting_thread;
  BiasedLocking::Condition _status_code;
  traceid _biased_locker_id;

public:
  VM_RevokeBias(Handle* obj, JavaThread* requesting_thread)
    : _obj(obj)
    , _objs(NULL)
    , _requesting_thread(requesting_thread)
    , _status_code(BiasedLocking::NOT_BIASED)
    , _biased_locker_id(0) {}

  VM_RevokeBias(GrowableArray<Handle>* objs, JavaThread* requesting_thread)
    : _obj(NULL)
    , _objs(objs)
    , _requesting_thread(requesting_thread)
    , _status_code(BiasedLocking::NOT_BIASED)
    , _biased_locker_id(0) {}

  virtual VMOp_Type type() const { return VMOp_RevokeBias; }

  virtual bool doit_prologue() {
    // Verify that there is actual work to do since the callers just
    // give us locked object(s). If we don't find any biased objects
    // there is nothing to do and we avoid a safepoint.
    if (_obj != NULL) {
      markOop mark = (*_obj)()->mark();
      if (mark->has_bias_pattern()) {
        return true;
      }
    } else {
      for ( int i = 0 ; i < _objs->length(); i++ ) {
        markOop mark = (_objs->at(i))()->mark();
        if (mark->has_bias_pattern()) {
          return true;
        }
      }
    }
    return false;
  }

  virtual void doit() {
    if (_obj != NULL) {
      if (TraceBiasedLocking) {
        tty->print_cr("Revoking bias with potentially per-thread safepoint:");
      }

      JavaThread* biased_locker = NULL;
      _status_code = revoke_bias((*_obj)(), false, false, _requesting_thread, &biased_locker);
#if INCLUDE_JFR
      if (biased_locker != NULL) {
        _biased_locker_id = JFR_THREAD_ID(biased_locker);
      }
#endif // INCLUDE_JFR

      clean_up_cached_monitor_info();
      return;
    } else {
      if (TraceBiasedLocking) {
        tty->print_cr("Revoking bias with global safepoint:");
      }
      BiasedLocking::revoke_at_safepoint(_objs);
    }
  }

  BiasedLocking::Condition status_code() const {
    return _status_code;
  }

  traceid biased_locker() const {
    return _biased_locker_id;
  }
};


class VM_BulkRevokeBias : public VM_RevokeBias {
private:
  bool _bulk_rebias;
  bool _attempt_rebias_of_object;

public:
  VM_BulkRevokeBias(Handle* obj, JavaThread* requesting_thread,
                    bool bulk_rebias,
                    bool attempt_rebias_of_object)
    : VM_RevokeBias(obj, requesting_thread)
    , _bulk_rebias(bulk_rebias)
    , _attempt_rebias_of_object(attempt_rebias_of_object) {}

  virtual VMOp_Type type() const { return VMOp_BulkRevokeBias; }
  virtual bool doit_prologue()   { return true; }

  virtual void doit() {
    _status_code = bulk_revoke_or_rebias_at_safepoint((*_obj)(), _bulk_rebias, _attempt_rebias_of_object, _requesting_thread);
    clean_up_cached_monitor_info();
  }
};


BiasedLocking::Condition BiasedLocking::revoke_and_rebias(Handle obj, bool attempt_rebias, TRAPS) {
  assert(!SafepointSynchronize::is_at_safepoint(), "must not be called while at safepoint");

  // We can revoke the biases of anonymously-biased objects
  // efficiently enough that we should not cause these revocations to
  // update the heuristics because doing so may cause unwanted bulk
  // revocations (which are expensive) to occur.
  markOop mark = obj->mark();
  if (mark->is_biased_anonymously() && !attempt_rebias) {
    // We are probably trying to revoke the bias of this object due to
    // an identity hash code computation. Try to revoke the bias
    // without a safepoint. This is possible if we can successfully
    // compare-and-exchange an unbiased header into the mark word of
    // the object, meaning that no other thread has raced to acquire
    // the bias of the object.
    markOop biased_value       = mark;
    markOop unbiased_prototype = markOopDesc::prototype()->set_age(mark->age());
    markOop res_mark = (markOop) Atomic::cmpxchg_ptr(unbiased_prototype, obj->mark_addr(), mark);
    if (res_mark == biased_value) {
      return BIAS_REVOKED;
    }
  } else if (mark->has_bias_pattern()) {
    Klass* k = obj->klass();
    markOop prototype_header = k->prototype_header();
    if (!prototype_header->has_bias_pattern()) {
      // This object has a stale bias from before the bulk revocation
      // for this data type occurred. It's pointless to update the
      // heuristics at this point so simply update the header with a
      // CAS. If we fail this race, the object's bias has been revoked
      // by another thread so we simply return and let the caller deal
      // with it.
      markOop biased_value       = mark;
      markOop res_mark = (markOop) Atomic::cmpxchg_ptr(prototype_header, obj->mark_addr(), mark);
      assert(!(*(obj->mark_addr()))->has_bias_pattern(), "even if we raced, should still be revoked");
      return BIAS_REVOKED;
    } else if (prototype_header->bias_epoch() != mark->bias_epoch()) {
      // The epoch of this biasing has expired indicating that the
      // object is effectively unbiased. Depending on whether we need
      // to rebias or revoke the bias of this object we can do it
      // efficiently enough with a CAS that we shouldn't update the
      // heuristics. This is normally done in the assembly code but we
      // can reach this point due to various points in the runtime
      // needing to revoke biases.
      if (attempt_rebias) {
        assert(THREAD->is_Java_thread(), "");
        markOop biased_value       = mark;
        markOop rebiased_prototype = markOopDesc::encode((JavaThread*) THREAD, mark->age(), prototype_header->bias_epoch());
        markOop res_mark = (markOop) Atomic::cmpxchg_ptr(rebiased_prototype, obj->mark_addr(), mark);
        if (res_mark == biased_value) {
          return BIAS_REVOKED_AND_REBIASED;
        }
      } else {
        markOop biased_value       = mark;
        markOop unbiased_prototype = markOopDesc::prototype()->set_age(mark->age());
        markOop res_mark = (markOop) Atomic::cmpxchg_ptr(unbiased_prototype, obj->mark_addr(), mark);
        if (res_mark == biased_value) {
          return BIAS_REVOKED;
        }
      }
    }
  }

  HeuristicsResult heuristics = update_heuristics(obj(), attempt_rebias);
  if (heuristics == HR_NOT_BIASED) {
    return NOT_BIASED;
  } else if (heuristics == HR_SINGLE_REVOKE) {
    Klass *k = obj->klass();
    markOop prototype_header = k->prototype_header();
    if (mark->biased_locker() == THREAD &&
        prototype_header->bias_epoch() == mark->bias_epoch()) {
      // A thread is trying to revoke the bias of an object biased
      // toward it, again likely due to an identity hash code
      // computation. We can again avoid a safepoint in this case
      // since we are only going to walk our own stack. There are no
      // races with revocations occurring in other threads because we
      // reach no safepoints in the revocation path.
      // Also check the epoch because even if threads match, another thread
      // can come in with a CAS to steal the bias of an object that has a
      // stale epoch.
      ResourceMark rm;
      if (TraceBiasedLocking) {
        tty->print_cr("Revoking bias by walking my own stack:");
      }
      EventBiasedLockSelfRevocation event;
      BiasedLocking::Condition cond = revoke_bias(obj(), false, false, (JavaThread*) THREAD, NULL);
      ((JavaThread*) THREAD)->set_cached_monitor_info(NULL);
      assert(cond == BIAS_REVOKED, "why not?");
      if (event.should_commit()) {
        event.set_lockClass(k);
        event.commit();
      }
      return cond;
    } else {
      EventBiasedLockRevocation event;
      VM_RevokeBias revoke(&obj, (JavaThread*) THREAD);
      VMThread::execute(&revoke);
      if (event.should_commit() && (revoke.status_code() != NOT_BIASED)) {
        event.set_lockClass(k);
        // Subtract 1 to match the id of events committed inside the safepoint
        event.set_safepointId(SafepointSynchronize::safepoint_counter() - 1);
        event.set_previousOwner(revoke.biased_locker());
        event.commit();
      }
      return revoke.status_code();
    }
  }

  assert((heuristics == HR_BULK_REVOKE) ||
         (heuristics == HR_BULK_REBIAS), "?");
  EventBiasedLockClassRevocation event;
  VM_BulkRevokeBias bulk_revoke(&obj, (JavaThread*) THREAD,
                                (heuristics == HR_BULK_REBIAS),
                                attempt_rebias);
  VMThread::execute(&bulk_revoke);
  if (event.should_commit()) {
    event.set_revokedClass(obj->klass());
    event.set_disableBiasing((heuristics != HR_BULK_REBIAS));
    // Subtract 1 to match the id of events committed inside the safepoint
    event.set_safepointId(SafepointSynchronize::safepoint_counter() - 1);
    event.commit();
  }
  return bulk_revoke.status_code();
}


void BiasedLocking::revoke(GrowableArray<Handle>* objs) {
  assert(!SafepointSynchronize::is_at_safepoint(), "must not be called while at safepoint");
  if (objs->length() == 0) {
    return;
  }
  VM_RevokeBias revoke(objs, JavaThread::current());
  VMThread::execute(&revoke);
}


void BiasedLocking::revoke_at_safepoint(Handle h_obj) {
  assert(SafepointSynchronize::is_at_safepoint(), "must only be called while at safepoint");
  oop obj = h_obj();
  HeuristicsResult heuristics = update_heuristics(obj, false);
  if (heuristics == HR_SINGLE_REVOKE) {
    revoke_bias(obj, false, false, NULL, NULL);
  } else if ((heuristics == HR_BULK_REBIAS) ||
             (heuristics == HR_BULK_REVOKE)) {
    // 进行批量撤销/批量重偏向逻辑
    bulk_revoke_or_rebias_at_safepoint(obj, (heuristics == HR_BULK_REBIAS), false, NULL);
  }
  clean_up_cached_monitor_info();
}


void BiasedLocking::revoke_at_safepoint(GrowableArray<Handle>* objs) {
  assert(SafepointSynchronize::is_at_safepoint(), "must only be called while at safepoint");
  int len = objs->length();
  for (int i = 0; i < len; i++) {
    oop obj = (objs->at(i))();
    HeuristicsResult heuristics = update_heuristics(obj, false);
    if (heuristics == HR_SINGLE_REVOKE) {
      revoke_bias(obj, false, false, NULL, NULL);
    } else if ((heuristics == HR_BULK_REBIAS) ||
               (heuristics == HR_BULK_REVOKE)) {
      bulk_revoke_or_rebias_at_safepoint(obj, (heuristics == HR_BULK_REBIAS), false, NULL);
    }
  }
  clean_up_cached_monitor_info();
}


void BiasedLocking::preserve_marks() {
  if (!UseBiasedLocking)
    return;

  assert(SafepointSynchronize::is_at_safepoint(), "must only be called while at safepoint");

  assert(_preserved_oop_stack  == NULL, "double initialization");
  assert(_preserved_mark_stack == NULL, "double initialization");

  // In order to reduce the number of mark words preserved during GC
  // due to the presence of biased locking, we reinitialize most mark
  // words to the class's prototype during GC -- even those which have
  // a currently valid bias owner. One important situation where we
  // must not clobber a bias is when a biased object is currently
  // locked. To handle this case we iterate over the currently-locked
  // monitors in a prepass and, if they are biased, preserve their
  // mark words here. This should be a relatively small set of objects
  // especially compared to the number of objects in the heap.
  _preserved_mark_stack = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<markOop>(10, true);
  _preserved_oop_stack = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<Handle>(10, true);

  ResourceMark rm;
  Thread* cur = Thread::current();
  for (JavaThread* thread = Threads::first(); thread != NULL; thread = thread->next()) {
    if (thread->has_last_Java_frame()) {
      RegisterMap rm(thread);
      for (javaVFrame* vf = thread->last_java_vframe(&rm); vf != NULL; vf = vf->java_sender()) {
        GrowableArray<MonitorInfo*> *monitors = vf->monitors();
        if (monitors != NULL) {
          int len = monitors->length();
          // Walk monitors youngest to oldest
          for (int i = len - 1; i >= 0; i--) {
            MonitorInfo* mon_info = monitors->at(i);
            if (mon_info->owner_is_scalar_replaced()) continue;
            oop owner = mon_info->owner();
            if (owner != NULL) {
              markOop mark = owner->mark();
              if (mark->has_bias_pattern()) {
                _preserved_oop_stack->push(Handle(cur, owner));
                _preserved_mark_stack->push(mark);
              }
            }
          }
        }
      }
    }
  }
}


void BiasedLocking::restore_marks() {
  if (!UseBiasedLocking)
    return;

  assert(_preserved_oop_stack  != NULL, "double free");
  assert(_preserved_mark_stack != NULL, "double free");

  int len = _preserved_oop_stack->length();
  for (int i = 0; i < len; i++) {
    Handle owner = _preserved_oop_stack->at(i);
    markOop mark = _preserved_mark_stack->at(i);
    owner->set_mark(mark);
  }

  delete _preserved_oop_stack;
  _preserved_oop_stack = NULL;
  delete _preserved_mark_stack;
  _preserved_mark_stack = NULL;
}


int* BiasedLocking::total_entry_count_addr()                   { return _counters.total_entry_count_addr(); }
int* BiasedLocking::biased_lock_entry_count_addr()             { return _counters.biased_lock_entry_count_addr(); }
int* BiasedLocking::anonymously_biased_lock_entry_count_addr() { return _counters.anonymously_biased_lock_entry_count_addr(); }
int* BiasedLocking::rebiased_lock_entry_count_addr()           { return _counters.rebiased_lock_entry_count_addr(); }
int* BiasedLocking::revoked_lock_entry_count_addr()            { return _counters.revoked_lock_entry_count_addr(); }
int* BiasedLocking::fast_path_entry_count_addr()               { return _counters.fast_path_entry_count_addr(); }
int* BiasedLocking::slow_path_entry_count_addr()               { return _counters.slow_path_entry_count_addr(); }


// BiasedLockingCounters

int BiasedLockingCounters::slow_path_entry_count() {
  if (_slow_path_entry_count != 0) {
    return _slow_path_entry_count;
  }
  int sum = _biased_lock_entry_count   + _anonymously_biased_lock_entry_count +
            _rebiased_lock_entry_count + _revoked_lock_entry_count +
            _fast_path_entry_count;

  return _total_entry_count - sum;
}

void BiasedLockingCounters::print_on(outputStream* st) {
  tty->print_cr("# total entries: %d", _total_entry_count);
  tty->print_cr("# biased lock entries: %d", _biased_lock_entry_count);
  tty->print_cr("# anonymously biased lock entries: %d", _anonymously_biased_lock_entry_count);
  tty->print_cr("# rebiased lock entries: %d", _rebiased_lock_entry_count);
  tty->print_cr("# revoked lock entries: %d", _revoked_lock_entry_count);
  tty->print_cr("# fast path lock entries: %d", _fast_path_entry_count);
  tty->print_cr("# slow path lock entries: %d", slow_path_entry_count());
}
