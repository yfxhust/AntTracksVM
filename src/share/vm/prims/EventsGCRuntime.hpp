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
 * File:   EventsGCRuntime.hpp
 * Author: Philipp Lengauer
 *
 * Created on April 2, 2014, 1:43 PM
 */

#ifndef EVENTSGCRUNTIME_HPP
#define	EVENTSGCRUNTIME_HPP

#include "EventsRuntime.hpp"
#include "../gc_interface/gcCause.hpp"
#include "gc_implementation/shared/mutableSpace.hpp"
#include "AllocationTracingDefinitions.hpp"
#include "EventSynchronization.hpp"
#include "../gc_implementation/parallelScavenge/parMarkBitMap.hpp"

class ParMarkBitMap;

struct SpaceRedefinitionInfo {
    uint index;
    HeapWord* bottom;
    HeapWord* end;
};

void flushG1Pointers(HeapWord *first_from, HeapWord *first_to, ParMarkBitMap* bitmap, int count);

class EventsGCRuntime : public AllStatic {
    friend class PostponeSpaceRedefineBlock;
private:
    static bool postpone_redefine;
    static jlong start;
    static Arena* postponed_space_redefines_arena;
    static GrowableArray<SpaceRedefinitionInfo*>* postponed_space_redefines;
    static size_t objects_in_eden;
public:
    static void init();
    static void destroy();
    
    static jint fire_gc_start(GCType type, GCCause::Cause cause);
    static void fire_gc_end(GCType type, jint id, GCCause::Cause cause, bool failed = false, bool gc_only = false);
    static void fire_gc_info(bool is_major, jint id);
    static void fire_gc_failed(bool is_major);
    static void fire_plab_alloc(HeapWord* addr, size_t size);
    static void fire_plab_flushed(HeapWord* addr, oop filler);
    static inline void fire_gc_keep_alive(oop addr, int size = 0, bool is_mark_valid = true);
    static inline void fire_gc_keep_alive_region(HeapWord* from, HeapWord* to, bool are_marks_valid = true);
    static inline void fire_gc_move_slow(oop from, oop to, int size = 0);
    static void fire_gc_move_region(oop from, oop to, jint num_of_objects, ParMarkBitMap* bitmap);
    static inline void fire_gc_move_fast(oop from, oop to, SpaceType to_addr);
    static inline void fire_gc_filler_alloc(oop filler);
    static inline void fire_gc_move_ptr(EventType event, uintptr_t referee, uintptr_t from_addr, SpaceType to_space);
    static inline void fire_gc_ptr(EventType event, uintptr_t obj, bool obj_is_inside_heap);

    static void sync();

public:
    static void schedule_space_redefine(uint index, HeapWord* bottom, HeapWord* end, bool just_expanded = false);
    static void schedule_space_redefine(uint index, HeapWord* bottom, HeapWord* end, HeapWord* bottom_old, HeapWord* end_old) {
        schedule_space_redefine(index, bottom, end, bottom <= bottom_old && end_old <= end);
    }
private:
    static void fire_postponed_redefinitions();
    
    static size_t count_eden_objects();
    static size_t count_eden_survivors();
#ifdef ASSERT
private:
    static Monitor* lock;
    static Arena* arena;
    static Dict* handled_objects;
    volatile static int32 _collectioncounter;
    static void add_handled_object(oop src, oop dest, bool allow_to_be_handled_again = false);
    static void add_handled_objects(HeapRegion* region, bool is_prepared, bool allow_to_be_handled_again = false);
    static void assert_handled_object(oop obj);
    static void assert_handled_object_space(oop obj);
    static bool clear_handled_objects(bool should_be_empty);
    static void clear_objects_in(Space* space);
    static void verify_all_objects_handled();
    static void verify_all_objects_handled_of(Space* space);
    static void verify_all_objects_handled_of(MutableSpace* space);
    static void verify_all_objects_handled_in(CollectedHeap* heap);
    
    static uint32 nr_of_objects_traced(Space* space);
    
    static void verify_allocation_sites();

public:
    static void dump_region(HeapWord* from, HeapWord* to);
    
    static bool is_gc_active();
#endif
};

inline void EventsGCRuntime::fire_gc_keep_alive(oop addr, int size, bool is_mark_valid) {   
    if(EventSynchronization::is_synchronizing()) {
        EventsRuntime::fire_sync_obj(addr, size, is_mark_valid);
    } else {
        EventsRuntime::fire_gc_move_slow(addr);
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        add_handled_object(addr, addr);
    }
#endif
}

inline void EventsGCRuntime::fire_gc_keep_alive_region(HeapWord* from, HeapWord* to, bool are_marks_valid) {
    for(HeapWord* obj = from; obj < to; obj = obj + oop(obj)->size()) {
        assert(oop(obj)->is_oop(false), "wtf");
        EventsGCRuntime::fire_gc_keep_alive(oop(obj), 0, are_marks_valid);
    }
}

inline void EventsGCRuntime::fire_gc_move_slow(oop from, oop to, int size) {   
    if(EventSynchronization::is_synchronizing()) {
        EventsRuntime::fire_sync_obj(from, to, size, true);
    } else {
        EventsRuntime::fire_gc_move_slow(from, to);
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        add_handled_object(from, to);
    }
#endif
}

inline void EventsGCRuntime::fire_gc_move_fast(oop from, oop to, SpaceType to_space) {
    if(EventSynchronization::is_synchronizing()) {
        EventsRuntime::fire_sync_obj(from, to, 0, true);
    } else {
        EventsRuntime::fire_gc_move_fast(from, to_space);
    }
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        add_handled_object(from, to);
    }
#endif
}

inline void EventsGCRuntime::fire_gc_filler_alloc(oop filler) {
    //actually, this method doesn't fire an event but is only used for consistency checking
#ifdef ASSERT
    if(TraceObjectsGCEnableParanoidAssertions) {
        add_handled_object(filler, filler, true);
    }
#endif
}

inline void EventsGCRuntime::fire_gc_move_ptr(EventType event, uintptr_t referee, uintptr_t from_addr, SpaceType to_space){
    HeapWord* from = (HeapWord*) from_addr;
    HeapWord* to = (HeapWord*) referee;
    
    if(event == EVENTS_GC_MOVE_FAST){
        EventsRuntime::fire_gc_move_fast_ptr(from, to, to_space);
    } else {
        EventsRuntime::fire_gc_move_slow_ptr(event, from, to);
    }
#ifdef ASSERT
    if (TraceObjectsGCEnableParanoidAssertions) {
        add_handled_object((oop) from, (oop) to);
    }
#endif
}


inline void EventsGCRuntime::fire_gc_ptr(EventType event, uintptr_t obj, bool obj_is_inside_heap){
    if(event == EVENTS_GC_ROOT_PTR){
        EventsRuntime::fire_gc_root_ptr(event, obj, obj_is_inside_heap);
    } else {
        EventsRuntime::fire_gc_obj_ptr(event, (HeapWord*) obj);        
    }
}

class GCMove : public StackObj {
private:
    ParMarkBitMap* bitmap;
    jint count;
    HeapWord *first_from, *first_to;
    HeapWord *next_from, *next_to;
#ifdef ASSERT
    jint moves;
    jint calls;
#endif
    
    inline void flush() {
        if(count > 0) {
            if(TraceObjectsPointers && UseG1GC){
                flushG1Pointers(first_from, first_to, bitmap, count);
            }
            else{
                EventsGCRuntime::fire_gc_move_region(oop(first_from), oop(first_to), count, bitmap);
            }
#ifdef ASSERT
            moves += count;
#endif
            count = 0;
            first_from = NULL;
            first_to = NULL;
            next_from = NULL;
            next_to = NULL;
        }
    }
    
public:
    inline GCMove(ParMarkBitMap* bitmap = NULL) : bitmap(bitmap), count(0), first_from(NULL), first_to(NULL), next_from(NULL), next_to(NULL)
#ifdef ASSERT
    , moves(0), calls(0)
#endif
    {}
    
    inline void fire(oop from, oop to, size_t size) {
        fire((HeapWord*) from, (HeapWord*) to, size);
    }
    
    inline ~GCMove() {
        if(TraceObjectsGC) {
            flush();
            assert(count == 0, "just checking");
            assert(calls == moves, "just checking");
        }
    }
    
    inline void fire(HeapWord* from, HeapWord* to, size_t size) {
        if(TraceObjectsGC) {
            assert(from != NULL, "just checking");
            assert(to != NULL, "just checking");
            assert(size > 0, "just checking");
            if (bitmap != NULL && bitmap->region_start() <= from && from < bitmap->region_end()) {
                bitmap->mark_obj(from, size);
            }
            /* bitmap != NULL implies that (from within bitmapregion implies that size == oop(to)->size() +- MinObj) */
            /* check comment from oopDesc::size_given_klass (oop.inline.hpp) for why we use MinObjAlignment */
            assert(bitmap == NULL
                        || (!(bitmap->region_start() <= from && from < bitmap->region_end())
                            || (size + MinObjAlignment >= (size_t) oop(to)->size()
                            ||  size - MinObjAlignment <= (size_t) oop(to)->size())), "just checking");
#ifdef ASSERT
            calls++;
#endif
            if(next_from == from && next_to == to) {
                //nothing to do, just move along ...
                assert(count > 0, "just checking");
            } else {
                // The following assumption does not seem right, therefor assert removed: a single thread can only traverse left to right, i.e, move objects left
                // assert(next_from < from || next_to < to, "heap may only be traversed left to right");
                flush();
                first_from = from;
                first_to = to;
                next_from = first_from;
                next_to = first_to;
            }
            next_from = next_from + size;
            next_to = next_to + size;
            count++;
            if(0 < TraceObjectsGCMoveRegionALot && TraceObjectsGCMoveRegionALot <= count) {
                flush();
            }
        }
    }
};

class SpaceRedefinition : public StackObj {
private:
    uint index;
    MutableSpace* space;
    HeapWord *bottom, *end;
public:
    inline  SpaceRedefinition(uint index, MutableSpace* space) : index(index), space(space), bottom(space->bottom()), end(space->end()) {}
    inline ~SpaceRedefinition() {
        if(TraceObjects && (bottom != space->bottom() || end != space->end())) {
            EventsGCRuntime::schedule_space_redefine(index, space->bottom(), space->end(), bottom, end);
        }
    }
};

class PostponeSpaceRedefineBlock : public BlockControl {
public:
    inline PostponeSpaceRedefineBlock(int dummy) {
        EventsGCRuntime::postpone_redefine = true;
    }
    
    inline ~PostponeSpaceRedefineBlock() {
        EventsGCRuntime::postpone_redefine = false;
    }
};

#define postpone_space_redefine _block_(PostponeSpaceRedefineBlock, 0)
#endif	/* EVENTSGCRUNTIME_HPP */

