/* 
 * Copyright (c) 2014, 2015, dynatrace and/or its affiliates. All rights reserved.
 * This file is part of the AntTracks extension for the Hotspot VM. 
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
 * You should have received a copy of the GNU General Public License
 * along with with this work.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * File:   EventsGCRuntime.cpp
 * Author: Philipp Lengauer
 * 
 * Created on April 2, 2014, 1:43 PM
 */

#include "precompiled.hpp"
#include "EventsGCRuntime.hpp"
#include "gc_implementation/parallelScavenge/parallelScavengeHeap.hpp"
#include "gc_implementation/parallelScavenge/parMarkBitMap.hpp"
#include "EventSynchronization.hpp"
#include "AllocationTracingSelfMonitoring.hpp"
#include "AllocationTracing.hpp"
#include "memory/allocation.hpp"
#include "gc_implementation/parNew/parNewGeneration.hpp"
#ifdef ASSERT
#include "AllocationTracingSynchronization.hpp"
#include "AllocationSiteStorage.hpp"
#include "utilities/growableArray.hpp"
#endif

void flushG1Pointers(HeapWord *first_from, HeapWord *first_to, ParMarkBitMap* bitmap, int count){
    HeapWord* from_addr = (HeapWord*) oop(first_from);
    HeapWord* to_addr = (HeapWord*) oop(first_to);
    
    for(int i = 0; i < count; i++) {
        size_t size = bitmap != NULL ? bitmap->obj_size(from_addr) : oop(to_addr)->size();
        //EventsRuntime::fire_gc_move_slow(oop(from_addr), oop(to_addr));
        Thread::current()->get_event_obj_ptrs()->set_meta(EVENTS_GC_MOVE_SLOW, (uintptr_t) (to_addr), true, (uintptr_t) (from_addr), -1, false);
        //Thread::current()->get_event_obj_ptrs()->set_meta(EVENTS_GC_OBJ_PTR, (uintptr_t) (to_addr), true);
        InstanceKlass* klass = (InstanceKlass*)oop(to_addr)->klass();
        OopMapBlock* map           = klass->start_of_nonstatic_oop_maps();            
        OopMapBlock* const end_map = map + klass->nonstatic_oop_map_count();  
        if (UseCompressedOops) {                                               
            while (map < end_map) {
                narrowOop* p = (narrowOop*)oop(to_addr)->obj_field_addr<narrowOop>(map->offset());
                narrowOop* const end = p + (map->count());
                while (p < end) {                                              
                    Thread::current()->add_event_obj_ptr(oopDesc::decode_heap_oop(*p));                                 
                    ++p;                                    
                }                                             
              ++map;                                                             
            }                                                                    
          } else {                                                               
            while (map < end_map) {
                oop* p = (oop*)oop(to_addr)->obj_field_addr<oop>(map->offset());
                oop* const end = p + (map->count());
                while (p < end) {                                              
                    Thread::current()->add_event_obj_ptr(*p);                                 
                    ++p;                                    
                }                                             
              ++map;                                                             
            }                                                                    
        }            
        Thread::current()->get_event_obj_ptrs()->flush();
        from_addr += size;
        to_addr += size;
    }
}


class HandlerObjectClosure : public ObjectClosure {
private:
    void (*handler)(oop obj);
public:
    HandlerObjectClosure(void (*handler)(oop obj)) : handler(handler) {}
    
    void do_object(oop obj) {
        handler(obj);
    }
};

jlong EventsGCRuntime::start = 0;
Arena* EventsGCRuntime::postponed_space_redefines_arena = NULL;
GrowableArray<SpaceRedefinitionInfo*>* EventsGCRuntime::postponed_space_redefines = NULL;

bool EventsGCRuntime::postpone_redefine = false;

size_t EventsGCRuntime::objects_in_eden = 0;

void EventsGCRuntime::init() {
    start = 0;
    postponed_space_redefines_arena = new (mtTracing) Arena(mtTracing);
    postponed_space_redefines = new (postponed_space_redefines_arena) GrowableArray<SpaceRedefinitionInfo*>(postponed_space_redefines_arena, 4, 0, NULL);
}

void EventsGCRuntime::destroy() {
    delete postponed_space_redefines_arena;
    postponed_space_redefines_arena = NULL;
    postponed_space_redefines = NULL;
    assert(_collectioncounter == 0, "uneven number of gc_start_end events!");
}

jint EventsGCRuntime::fire_gc_start(GCType type, GCCause::Cause cause) {
    static jint id = 0;
    assert(!(type == GCType_Major || type == GCType_Major_Sync) || !is_gc_active(), "can't start mayorGC while other GC is active");

    if(EventSynchronization::should_rotate()) {
        EventSynchronizationType sync_type;
        if(type == GCType_Major) {
            type = GCType_Major_Sync;
            sync_type = OnMajor;
        } else if(TraceObjectsMaxTraceSizeSynchronizeOnMinor && type == GCType_Minor) {
            type = GCType_Minor_Sync;
            sync_type = OnMinor;
        } else {
            sync_type = None;
        }
        if(sync_type != None) {
            EventSynchronization::start_synchronization(sync_type);
        } else {
            EventBuffersFlushAll::flush_all();
        }
        
        if(UseParallelOldGC) {
            ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
            EventsRuntime::fire_space_creation(PARALLEL_GC_OLD_ID, heap->old_gen()->object_space()->bottom(), heap->old_gen()->object_space()->end());
            EventsRuntime::fire_space_creation(PARALLEL_GC_EDEN_ID, heap->young_gen()->eden_space()->bottom(), heap->young_gen()->eden_space()->end());
            EventsRuntime::fire_space_creation(PARALLEL_GC_SURVIVOR_1_ID, heap->young_gen()->survivor_1_space()->bottom(), heap->young_gen()->survivor_1_space()->end());
            EventsRuntime::fire_space_creation(PARALLEL_GC_SURVIVOR_2_ID, heap->young_gen()->survivor_2_space()->bottom(), heap->young_gen()->survivor_2_space()->end());
            EventsRuntime::fire_space_alloc(PARALLEL_GC_OLD_ID, SPACE_MODE_NORMAL, OLD_SPACE);
            EventsRuntime::fire_space_alloc(PARALLEL_GC_EDEN_ID, SPACE_MODE_NORMAL, EDEN_SPACE);
            EventsRuntime::fire_space_alloc(PARALLEL_GC_SURVIVOR_1_ID, SPACE_MODE_NORMAL, SURVIVOR_SPACE);
            EventsRuntime::fire_space_alloc(PARALLEL_GC_SURVIVOR_2_ID, SPACE_MODE_NORMAL, SURVIVOR_SPACE);
        } else if(UseG1GC) {
            G1CollectedHeap* heap = G1CollectedHeap::heap();
            for(uint index = 0; index < heap->num_regions(); index++) {
                HeapRegion* region = heap->region_at(index);
                EventsRuntime::fire_space_creation(index, region->bottom(), region->end());
                SpaceType type;
                SpaceMode mode;
                if(region->is_eden()) {
                    type = EDEN_SPACE;
                    mode = SPACE_MODE_NORMAL;
                } else if(region->is_survivor()) {
                    type = SURVIVOR_SPACE;
                    mode = SPACE_MODE_NORMAL;
                } else if(region->is_old()) {
                    type = OLD_SPACE;
                    mode = SPACE_MODE_NORMAL;
                } else if(region->is_free()) {
                    continue;
                } else if(region->startsHumongous()) {
                    type = OLD_SPACE;
                    mode = SPACE_MODE_HUMONGOUS_START;
                } else if(region->continuesHumongous()) {
                    type = OLD_SPACE;
                    mode = SPACE_MODE_HUMONGOUS_CONTINUES;
                }
                EventsRuntime::fire_space_alloc(index, mode, type);
            }
        } else {
            assert(false, "wtf");
        }
    }
    EventsRuntime::fire_gc_start_end(EVENTS_GC_START, ++id, type, cause);
    fire_postponed_redefinitions();
    
    
    self_monitoring(1) {
        start = AllocationTracing::get_trace_writer()->size();
    }
    self_monitoring(3) {
        objects_in_eden = count_eden_objects();
        if(EventSynchronization::is_synchronizing()) {
            AllocationTracingSelfMonitoring::prepare_for_report_sync_quality();
        } else {
            AllocationTracingSelfMonitoring::report_sync_quality();
        }
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        clear_handled_objects(true);
        synchronized(lock) { // with concurrent collections this may be accessed by multiple threads
            ++_collectioncounter;
        }
        
        if (UseParNewGC && cause == GCCause::_allocation_failure) {
          GenCollectedHeap* gch = GenCollectedHeap::heap();
          Generation* g0 = gch->get_gen(0);
          assert(g0->kind() == Generation::ParNew || g0->kind() == Generation::ASParNew, "Wrong generation!");
          ParNewGeneration* young = (ParNewGeneration*) g0;
          assert(0 == nr_of_objects_traced(young->from()), "not cleaned up properly");
          assert(0 == nr_of_objects_traced(young->to()), "not cleaned up properly");
          assert(0 == nr_of_objects_traced(young->eden()), "not cleaned up properly");
        }
        if (UseConcMarkSweepGC && (cause == GCCause::_cms_initial_mark || cause == GCCause::_cms_final_remark)) {
          GenCollectedHeap* gch = GenCollectedHeap::heap();
          Generation* g1 = gch->get_gen(1);
          assert(g1->kind() == Generation::ASConcurrentMarkSweep || g1->kind() == Generation::ConcurrentMarkSweep, "Wrong generation!");
          ConcurrentMarkSweepGeneration* cmsgen = (ConcurrentMarkSweepGeneration*) g1;
          assert(0 == nr_of_objects_traced(cmsgen->cmsSpace()), "not cleaned up properly");
        }
        
        if(TraceObjectsSaveAllocationSites) {
            verify_allocation_sites();
        }
    }
#endif
    return id;
}

void EventsGCRuntime::fire_gc_end(GCType type, jint id, GCCause::Cause cause, bool failed, bool gc_only) {
#ifdef ASSERT
    assert(!TraceObjectsGCEnableParanoidAssertions || is_gc_active(), "has to be");
#endif
    EventsRuntime::fire_gc_start_end(EVENTS_GC_END, id, type, cause, failed, gc_only);
    bool was_sync = EventSynchronization::is_synchronizing();
    if(was_sync) {
        EventSynchronization::stop_synchronization();
    }
    fire_postponed_redefinitions();
    
    self_monitoring(1) {
        EventBuffersFlushAll::wait_for_all_serialized();
        jlong size = AllocationTracing::get_trace_writer()->size() - start;
        switch(type) {
            case GCType_Minor: if(was_sync) AllocationTracingSelfMonitoring::report_minor_sync(size); else AllocationTracingSelfMonitoring::report_minor_gc(size); break;
            case GCType_Major: if(was_sync) AllocationTracingSelfMonitoring::report_major_sync(size); else AllocationTracingSelfMonitoring::report_major_gc(size); break;
            default: assert(false, "here be dragons");
        }
    }
    self_monitoring(3) {
        if(objects_in_eden > 0) {
            double survivor_ratio = 1.0 * count_eden_survivors() / objects_in_eden;
            AllocationTracingSelfMonitoring::report_survivor_ratio(survivor_ratio);
        }
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        synchronized(lock) {
            --_collectioncounter;
            assert(_collectioncounter >= 0, "Double gc_end events?");
        }
        if(!failed) {
            if(UseParallelOldGC) {
                ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
                if(type == GCType_Minor) {
                    assert(heap->young_gen()->eden_space()->is_empty(), "eden must be empty");
                    verify_all_objects_handled_of(heap->young_gen()->from_space());
                    assert(heap->young_gen()->to_space()->is_empty(), "survivor to must be empty");
                } else if (type == GCType_Major) {
                    verify_all_objects_handled_in(heap);
                } else {
                    assert(false, "what are we collecting?");
                }
            } else if (UseG1GC) {
                G1CollectedHeap* heap = G1CollectedHeap::heap();
                if(type == GCType_Major) {
                    verify_all_objects_handled_in(heap);
                } else {
                    verify_all_objects_handled();                    
                }
            } else if (UseConcMarkSweepGC || UseParNewGC) {
                GenCollectedHeap* gch = GenCollectedHeap::heap();
                assert(gch->n_gens() >= 2, "Will use index 0 and 1 below");
                if (GCCause::_cms_initial_mark == cause) { // cms init end (force space merge)
                    Generation* g1 = gch->get_gen(1);
                    assert(g1->kind() == Generation::ConcurrentMarkSweep || g1->kind() == Generation::ASConcurrentMarkSweep, "Wrong generation!");
                    ConcurrentMarkSweepGeneration* cmsgen = (ConcurrentMarkSweepGeneration*) g1;
                    assert(0 == nr_of_objects_traced(cmsgen->cmsSpace()), "sanity check");
                    synchronized(lock) {
                      assert(handled_objects->Size() == 0, "ParNew did not clean up properly");
                    }
                } else if (GCCause::_cms_final_remark == cause) { // cms end
                    Generation* g1 = gch->get_gen(1);
                    assert(g1->kind() == Generation::ConcurrentMarkSweep || g1->kind() == Generation::ASConcurrentMarkSweep, "Wrong generation!");
                    ConcurrentMarkSweepGeneration* cmsgen = (ConcurrentMarkSweepGeneration*) g1;
                    // we can not verify the objects for the CMS, as we need
                    // to be at a safepoint to do that (due to concurrent allocations)
                    //verify_all_objects_handled_of(cmsgen->cmsSpace());
                    clear_objects_in(cmsgen->cmsSpace());
                } else if (GCCause::_no_gc == cause) {// cms compaction end
                    assert(!is_gc_active(), "sanity check");
                    Generation* g1 = gch->get_gen(1);
                    assert(g1->kind() == Generation::ConcurrentMarkSweep || g1->kind() == Generation::ASConcurrentMarkSweep, "Wrong generation!");
                    ConcurrentMarkSweepGeneration* cmsgen = (ConcurrentMarkSweepGeneration*) g1;
                    {
                      // temporarily disable allocations
                      MutexLockerEx ml(cmsgen->cmsSpace()->freelistLock(), Mutex::_no_safepoint_check_flag);
                      verify_all_objects_handled_of(cmsgen->cmsSpace());
                    }
                    synchronized(lock) {
                      assert(handled_objects->Size() == 0, "should be");
                    }
                } else { // parnew end
                    Generation* g0 = gch->get_gen(0);
                    assert(g0->kind() == Generation::ParNew || g0->kind() == Generation::ASParNew, "Wrong generation!");
                    ParNewGeneration* young = (ParNewGeneration*) g0;
                    // during promotion failures eden may still contain objects
                    assert(failed || young->eden()->is_empty(), "eden must be empty!");
                    assert(young->to()->is_empty(), "survivor must be emtpy!");
                    /* manually clear all affected spaces */
                    clear_objects_in(young->to());
                    /* now we can verify */
                    verify_all_objects_handled_of(young->eden());
                    verify_all_objects_handled_of(young->from());
                    
                    if (is_gc_active()) {
                      // clear objects during ongoing CMS collection, to not interfere
                      // with with verification
                      handled_objects->Clear();
                    }
                }
            } else {
                assert(false, "here be dragons");
            }
            if(TraceObjectsSaveAllocationSites) {
                verify_allocation_sites();
            }
        }
        clear_handled_objects(false);
        // at the end of final remark, we expect handled_objects to be empty
        assert(!(GCCause::_cms_final_remark == cause && !is_gc_active()) || handled_objects->Size() == 0, "WTF?");
    }
#endif
}

void EventsGCRuntime::fire_postponed_redefinitions() {
    for(int i = 0; i < postponed_space_redefines->length(); i++) {
        SpaceRedefinitionInfo* redef = postponed_space_redefines->at(i);
        EventsRuntime::fire_space_redefine(redef->index, redef->bottom, redef->end);
        free(redef);
    }
    postponed_space_redefines->clear();
}

void EventsGCRuntime::fire_gc_info(bool is_major, jint id) {
    if(!is_major) {
        if(UseParallelGC) {            
            ParallelScavengeHeap* heap = ParallelScavengeHeap::heap();
            HeapWord* fromBottom = heap->young_gen()->from_space()->bottom();
            HeapWord* toBottom = heap->young_gen()->to_space()->bottom(); 
                        
            uint survivor_id;
            if(fromBottom < toBottom) survivor_id = PARALLEL_GC_SURVIVOR_1_ID;
            else if (toBottom < fromBottom) survivor_id = PARALLEL_GC_SURVIVOR_2_ID;
            else assert(false, "from and to survivor cannot be at same address!");
            
            EventsRuntime::fire_gc_info(PARALLEL_GC_EDEN_ID, id);
            //EventsRuntime::fire_gc_info(PARALLEL_GC_SURVIVOR_1_ID);
            //EventsRuntime::fire_gc_info(PARALLEL_GC_SURVIVOR_2_ID);
            EventsRuntime::fire_gc_info(survivor_id, id);
        } else if(UseG1GC) {
            for(HeapRegion* region = G1CollectedHeap::heap()->g1_policy()->collection_set(); region != NULL; region = region->next_in_collection_set()) {
                EventsRuntime::fire_gc_info(region->hrm_index(), id);
            }
        } else if (UseParNewGC) {
            GenCollectedHeap* gch = GenCollectedHeap::heap();
            assert(gch->n_gens() >= 1, "Will use index 0 below");
            Generation* g0 = gch->get_gen(0);
            assert(g0->kind() == Generation::ParNew || g0->kind() == Generation::ASParNew, "Wrong generation kind!");
            ParNewGeneration* png = (ParNewGeneration*) g0;
            HeapWord* fromBottom = png->from()->bottom();
            HeapWord* toBottom = png->to()->bottom();
            
            uint survivor_id;
            if(fromBottom < toBottom) survivor_id = PARALLEL_GC_SURVIVOR_1_ID;
            else if (toBottom < fromBottom) survivor_id = PARALLEL_GC_SURVIVOR_2_ID;
            else assert(false, "from and to survivor cannot be at same address!");
            
            EventsRuntime::fire_gc_info(PARALLEL_GC_EDEN_ID, id);
            EventsRuntime::fire_gc_info(survivor_id, id);
        } else {
            assert(false, "wtf");
        }
        EventsRuntime::fire_sync();        
    } else if (UseConcMarkSweepGC) { // CMS is old-gen only
        EventsRuntime::fire_gc_info(CMS_GC_OLD_ID, id);
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions && UseG1GC && !is_major) {
        G1CollectedHeap* heap = G1CollectedHeap::heap();
        for(HeapRegion* region = heap->g1_policy()->collection_set(); region != NULL; region = region->next_in_collection_set()) {
            add_handled_objects(region, false, true);
        }
    }
#endif
}

void EventsGCRuntime::fire_gc_failed(bool is_major) {
    // assertion removed to let CMS fail during HeapCompaction
    //assert(!is_major, "just checking");
    EventBuffersFlushAll::flush_all();
    EventsRuntime::fire_sync(true);
    if(UseParallelOldGC) {
        PSYoungGen* yg = ParallelScavengeHeap::heap()->young_gen();
        EventsRuntime::fire_gc_failed(PARALLEL_GC_EDEN_ID);
        EventsRuntime::fire_gc_failed(yg->from_space() == yg->survivor_1_space() ? PARALLEL_GC_SURVIVOR_1_ID : PARALLEL_GC_SURVIVOR_2_ID);
    } else if (UseG1GC) {
        for(HeapRegion* region = G1CollectedHeap::heap()->g1_policy()->collection_set(); region != NULL; region = region->next_in_collection_set()) {
            if(region->evacuation_failed()) {
                EventsRuntime::fire_gc_failed(region->hrm_index());
            }
        }
    } else if (is_major && UseConcMarkSweepGC) {
        EventsRuntime::fire_gc_failed(CMS_GC_OLD_ID);
    } else if (!is_major && UseParNewGC) {
        EventsRuntime::fire_gc_failed(PARALLEL_GC_EDEN_ID);
        EventsRuntime::fire_gc_failed(PARALLEL_GC_SURVIVOR_1_ID);
        EventsRuntime::fire_gc_failed(PARALLEL_GC_SURVIVOR_2_ID);
    } else {
        assert(false, "wtf");
    }
}

void EventsGCRuntime::fire_plab_alloc(HeapWord* addr, size_t size) {
    EventsRuntime::fire_plab_alloc(addr, size);
}

void EventsGCRuntime::fire_plab_flushed(HeapWord* addr, oop filler) { 
    //actually, this method doesn't fire an event but is only used for consistency checking
    EventsGCRuntime::fire_gc_filler_alloc(filler);
}

void EventsGCRuntime::fire_gc_move_region(oop from, oop to, jint num_of_objects, ParMarkBitMap* bitmap) {
    if(EventSynchronization::is_synchronizing()) {
        HeapWord* from_addr = (HeapWord*) from;
        HeapWord* to_addr = (HeapWord*) to;
        for(int i = 0; i < num_of_objects; i++) {
            size_t size;
            if (bitmap != NULL && bitmap->region_start() <= from_addr && from_addr < bitmap->region_end()) {
                size = bitmap->obj_size(from_addr);
            } else {
                size = oop(to_addr)->size();
            }
            EventsRuntime::fire_sync_obj(oop(from_addr), oop(to_addr), (int) size, true);
            from_addr += size;
            to_addr += size;
        }
    } else {
        EventsRuntime::fire_gc_move_region(from, to, num_of_objects);
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        HeapWord* from_addr = (HeapWord*) from;
        HeapWord* to_addr = (HeapWord*) to;
        for(int i = 0; i < num_of_objects; i++) {
            size_t size;
            if (bitmap != NULL && bitmap->region_start() <= from_addr && from_addr < bitmap->region_end()) {
                size = bitmap->obj_size(from_addr);
            } else {
                size = oop(to_addr)->size();
            }
            add_handled_object(oop(from_addr), oop(to_addr));
            from_addr += size;
            to_addr += size;
        }
    }
#endif
}

void EventsGCRuntime::sync() {
    EventsRuntime::fire_sync();
}

size_t EventsGCRuntime::count_eden_objects() {
    size_t count = 0;
    if(UseParallelOldGC) {
        MutableSpace* space = ParallelScavengeHeap::heap()->young_gen()->eden_space();
        for(HeapWord* cursor = space->bottom(); cursor < space->top(); cursor += oop(cursor)->size()) {
            count++;
        }
    } else if(UseG1GC) {
        for(uint index = 0; index < G1CollectedHeap::heap()->num_regions(); index++) {
            HeapRegion* region = G1CollectedHeap::heap()->region_at(index);
            if(region->is_eden()) {
                for(HeapWord* cursor = region->bottom(); cursor < region->top(); cursor += oop(cursor)->size()) {
                    count++;
                }
            }
        }
    } else if (UseParNewGC) {
        GenCollectedHeap* gch = GenCollectedHeap::heap();
        Generation* g0 = gch->get_gen(0);
        assert(g0->kind() == Generation::ParNew, "Wrong generation!");
        ParNewGeneration* young = (ParNewGeneration*) g0;
        for(HeapWord* cursor = young->eden()->bottom(); cursor < young->eden()->top(); cursor+= oop(cursor)->size()) {
            count++;
        }
    } else {
        assert(false, "wtf");
    }
    return count;
}

size_t EventsGCRuntime::count_eden_survivors() {
    size_t count = 0;
    if(UseParallelOldGC) {
        MutableSpace* space = ParallelScavengeHeap::heap()->young_gen()->from_space();
        for(HeapWord* cursor = space->bottom(); cursor < space->top(); cursor += oop(cursor)->size()) {
            if(oop(cursor)->age() == 1) {
                count++;
            }
        }
    } else if (UseG1GC) {
        for(uint index = 0; index < G1CollectedHeap::heap()->num_regions(); index++) {
            HeapRegion* region = G1CollectedHeap::heap()->region_at(index);
            if(region->is_survivor()) {
                for(HeapWord* cursor = region->bottom(); cursor < region->top(); cursor += oop(cursor)->size()) {
                    if(oop(cursor)->age() == 1) {
                        count++;
                    }
                }
            }
        }
    } else if (UseParNewGC) {
        GenCollectedHeap* gch = GenCollectedHeap::heap();
        assert(gch->n_gens() >= 1, "Will use index 0 below");
        Generation* g0 = gch->get_gen(0);
        assert(g0->kind() == Generation::ParNew, "Wrong generation!");
        ParNewGeneration* young = (ParNewGeneration*) g0;
        for(HeapWord* cursor = young->from()->bottom(); cursor < young->from()->top(); cursor+= oop(cursor)->size()) {
            if(oop(cursor)->age() == 1) count++;
        }
    } else {
        assert(false, "wtf");        
    }
    return count;
}

#ifdef ASSERT
Monitor* EventsGCRuntime::lock = new Monitor(Mutex::native, "EventsGCRuntime Paranoid Assertions Lock", true);
Arena* EventsGCRuntime::arena = NULL;
Dict* EventsGCRuntime::handled_objects = NULL;
volatile int32 EventsGCRuntime::_collectioncounter = 0;

#define ObjectHandledStatus void*
#define OBJECT_UNHANDLED (NULL)
#define OBJECT_HANDLED_MAY_AGAIN ((void*) 0x1)
#define OBJECT_HANDLED ((void*) 0x2)

void EventsGCRuntime::add_handled_object(oop from, oop to, bool allow_to_be_handled_again) {
    if (!is_gc_active()) return; // simply ignore, because we don't know if everything has been initialized yet
    bool reverted = UseG1GC && !G1CollectedHeap::heap()->_full_collection;
    oop obj = reverted ?  from : to;

    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    assert(obj != NULL, "just checking");
    assert(is_gc_active(), "just checking");
    ObjectHandledStatus prev_status;
    synchronized(lock) {
        prev_status = handled_objects->Insert(obj, allow_to_be_handled_again ? OBJECT_HANDLED_MAY_AGAIN : OBJECT_HANDLED);
    }
    if(reverted) {
        assert(prev_status == OBJECT_UNHANDLED || prev_status == OBJECT_HANDLED_MAY_AGAIN, "object not in a collected region");
    } else {
        assert(prev_status == OBJECT_UNHANDLED || prev_status == OBJECT_HANDLED_MAY_AGAIN, "object already handled (event fired twice for same object?)");
    }
}

void EventsGCRuntime::add_handled_objects(HeapRegion* region, bool is_prepared, bool allow_to_be_handled_again) {
    if(region->continuesHumongous()) return;
    HeapWord* q = region->bottom();
    HeapWord* t = region->top();
    while(q < t) {
        if(oop(q)->is_gc_marked()) {
            add_handled_object(oop(q), oop(q), allow_to_be_handled_again);
            q = q + oop(q)->size();
        } else {
            if(is_prepared) {
                q = (HeapWord*) oop(q)->mark()->decode_pointer();
                if(q == NULL) return;
            } else {
                q = q + oop(q)->size();
            }
        }
    }
}

void EventsGCRuntime::assert_handled_object(oop obj) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    synchronized(lock) {
        assert((*handled_objects)[obj] != OBJECT_UNHANDLED, "object in collected space although no move event has been sent");
        handled_objects->Delete(obj);
    }
}

void EventsGCRuntime::assert_handled_object_space(oop obj) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    assert(obj->is_oop(), "expected oop");
    synchronized(lock) {
      // skip alignment allocations (parNew uses int[])
      if (obj->mark() != markOopDesc::prototype()) {
        ObjectHandledStatus prev_status = (*handled_objects)[obj];
        assert(prev_status != OBJECT_UNHANDLED, "object in collected space although no move event has been sent");
      }
      handled_objects->Delete(obj);
    }
    handled_objects->Delete(obj);
}

bool EventsGCRuntime::clear_handled_objects(bool should_be_empty) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    bool reti = false;
    synchronized(lock) {
        if(arena == NULL) {
            arena = new(mtOther) Arena(mtOther);
        }
        if(handled_objects == NULL) {
            handled_objects = new(arena) Dict(cmpkey, hashptr, arena);
        }
        if (!is_gc_active()) {
            assert(!should_be_empty || handled_objects->Size() == 0, "?");
            handled_objects->Clear();
            reti = true;
        }
    }
    assert(is_gc_active() || reti, "handled_objects should have been cleared!");
    return reti;
}

uint32 EventsGCRuntime::nr_of_objects_traced(Space* space) {
  assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
  uint32 reti = 0;
  
  synchronized(lock) {
    for(DictI i(handled_objects); i.test(); ++i) {
      if (space->is_in(i._key)) reti++;
    }
  }
  return reti;
}

class VerifyObjectHandledClosure : public ObjectClosure {
private:
    void (*verify)(oop obj);
public:
    VerifyObjectHandledClosure(void (*verify)(oop obj)) : verify(verify) {}
    
    void do_object(oop obj) {
        verify(obj);
    }
};

void EventsGCRuntime::verify_all_objects_handled() {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    jlong count = 0;
    for(DictI i(handled_objects); i.test(); ++i) {
        if(i._value != OBJECT_HANDLED) count++;
    }
    assert(count == 0, "object(s) unhandled");
}

void EventsGCRuntime::verify_all_objects_handled_of(Space* space) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    //this lock is actually not necessary but it speeds up things a lot because the assert_handled_object doesn't have to lock/unlock all the time
    synchronized(lock) {
        VerifyObjectHandledClosure closure = VerifyObjectHandledClosure(assert_handled_object_space);
        space->object_iterate(&closure);
        for(DictI i(handled_objects); i.test(); ++i) {
            assert(!space->is_in(i._key), "some objects are not in the heap although move events have been sent");
        }
    }
}

void EventsGCRuntime::verify_all_objects_handled_of(MutableSpace* space) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    //this lock is actually not necessary but it speeds up things a lot because the assert_handled_object doesn't have to lock/unlock all the time
    synchronized(lock) {
        VerifyObjectHandledClosure closure = VerifyObjectHandledClosure(assert_handled_object);
        space->object_iterate(&closure);
        for(DictI i(handled_objects); i.test(); ++i) {
            assert(!space->contains(i._key), "some objects are not in the heap although move events have been sent");
        }
    }
}

void EventsGCRuntime::verify_all_objects_handled_in(CollectedHeap* heap) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    //this lock is actually not necessary but it speeds up things a lot because the assert_handled_object doesn't have to lock/unlock all the time
    synchronized(lock) {
        VerifyObjectHandledClosure closure = VerifyObjectHandledClosure(assert_handled_object);
        heap->object_iterate(&closure);
        assert(handled_objects->Size() == 0, "some objects are not in the heap although move events have been sent");
    }
}

class VerifyAllocationSiteClosure : public ObjectClosure {
public:
    VerifyAllocationSiteClosure() {}
    
    void do_object(oop obj) {
        assert(AllocationSiteStorage::load(Thread::current(), obj) != ALLOCATION_SITE_IDENTIFIER_UNKNOWN, "no valid allocation site stored");
    }
};

void EventsGCRuntime::verify_allocation_sites() {
    //TODO: we do not know whether the allocation site is valid here, so the check currently does not work...
    //VerifyAllocationSiteClosure closure;
    //Universe::heap()->object_iterate(&closure);
}

void EventsGCRuntime::dump_region(HeapWord* from, HeapWord* to) {
    ResourceMark rm(Thread::current());
    HeapWord* cur = from;
    while(cur < to) {
        oopDesc* obj = (oopDesc*) cur;
        assert(obj->is_oop(false), "wtf");
        const char* type = obj->klass()->internal_name();
        long addr = ((intptr_t) obj) - ((intptr_t) from);
        int size = obj->size() * HeapWordSize;
        tty->print("%s @ %li (%i b)\n", type, addr, size);
        cur += obj->size();
    }
}

void EventsGCRuntime::clear_objects_in(Space* space) {
    assert(TraceObjectsGCEnableParanoidAssertions, "who is calling?");
    Arena* todelArena = new(mtOther) Arena(mtOther);
    GrowableArray<HeapWord*>* todelete = new(todelArena) GrowableArray<HeapWord*>(todelArena, 2, 0, NULL);
    //this lock is actually not necessary but it speeds up things a lot because the assert_handled_object doesn't have to lock/unlock all the time
    synchronized(lock) {
      for(DictI i(handled_objects); i.test(); ++i) {
        if (space->is_in(i._key))
          todelete->push((HeapWord*) i._key);
      }
      
      GrowableArrayIterator<HeapWord*> end = todelete->end();
      for(GrowableArrayIterator<HeapWord*> i=todelete->begin(); i != end; ++i) {
          assert((*handled_objects)[*i] != OBJECT_UNHANDLED, "wtf?");
          handled_objects->Delete(*i);
      }
    }
    delete todelArena;
}

bool EventsGCRuntime::is_gc_active() {
    bool ret = false;
    synchronized(lock) {
        assert(_collectioncounter >= 0, "more gc_ends than gc_starts?");
        ret = _collectioncounter != 0;
    }
    return ret;
}

#endif

void EventsGCRuntime::schedule_space_redefine(uint index, HeapWord* bottom, HeapWord* end, bool just_expanded) {
    if(!postpone_redefine) {
        assert(just_expanded, "just checking");
        EventsRuntime::fire_space_redefine(index, bottom, end);
    } else {
        SpaceRedefinitionInfo* redef = (SpaceRedefinitionInfo*) malloc(sizeof(SpaceRedefinitionInfo));
        redef->index = index;
        redef->bottom = bottom;
        redef->end = end;
        postponed_space_redefines->append(redef);
    }
}