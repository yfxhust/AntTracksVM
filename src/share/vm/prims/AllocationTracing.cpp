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
 * File:   AllocationTracing.cpp
 * Author: Philipp Lengauer
 * 
 * Created on October 21, 2014, 10:47 AM
 */

#include "precompiled.hpp"
#include "AllocationTracing.hpp"
#include "AllocatedTypes.hpp"
#include "AllocationSites.hpp"
#include "EventBuffers.hpp"
#include "SymbolsWriter.hpp"
#include "AllocationTracingSelfMonitoring.hpp"
#include "AllocationTracingStackTraces.hpp"
#include "EventSynchronization.hpp"


volatile bool AllocationTracing::active = false;
SymbolsWriter* AllocationTracing::out_symbols = NULL;
ClassDefinitionWriter *AllocationTracing::out_class_definitions = NULL;
EventsWriter* AllocationTracing::out_trace = NULL;
EventsWorkerThread* AllocationTracing::worker = NULL;

void AllocationTracing::init() {
    if(PrintTraceObjects) AllocationTracing_log("activating tracing");

#if defined(TARGET_ARCH_x86)
//everything is fine
#elif defined(TARGET_ARCH_sparc)
#error "SPARC architecture is not supported"
#elif defined(TARGET_ARCH_arm)
#error "ARM architecture is not supported"
#elif defined(TARGET_ARCH_ppc)
#error "PPC architecture is not supported"
#else
#error "unknown architecture"
#endif

#if !defined(_LP64)
    warning("object tracing under 32-bit has been smoke-tested only!");
#endif

#if defined(TARGET_OS_FAMILY_linux)
    //everything is fine
#elif defined(TARGET_OS_FAMILY_aix)
    warning("object tracing under aix is untested!");
#elif defined(TARGET_OS_FAMILY_solaris)
    warning("object tracing under solaris is untested!");
#elif defined(TARGET_OS_FAMILY_windows)
    //everything is fine
#elif defined(TARGET_OS_FAMILY_bsd)
    //everything is fine
#else
    warning("object tracing untested under current operating system!");
#endif
    
    guarantee(!UseSerialGC, "not implemented yet");
    //guarantee(!UseConcMarkSweepGC, "not implemented yet");
    //guarantee(!UseParNewGC, "not implemented yet");
    guarantee(TraceObjectsParallelWorkerThreads == 1, "not implemented yet");
    if(TraceObjectsAllocationStackTraces && TraceObjectsUseNakedSlowPaths) {
        warning("TraceObjectsAllocationStackTraces and TraceObjectsUseNakedSlowPaths set => will ignore TraceObjectsAllocationStackTraces");
    }
    if(!TraceObjectsAsyncIO && (TraceObjectsCompressTrace || TraceObjectsCompressTraceAdaptively)) {
        report_fatal(__FILE__, __LINE__, "Cannot compress without async IO");
    }
    
    assert(!active, "already initialized?");
    
#ifndef _WINDOWS
    srand((unsigned int) (time(NULL) * getpid()));
#else
    srand((unsigned int) (time(NULL)));
#endif

    AllocationTracingSelfMonitoring::init();
    
    out_symbols = new SymbolsWriter();
    out_class_definitions = new ClassDefinitionWriter();
    out_trace = new EventsWriter();
    
    AllocatedTypes::init(out_symbols);
    AllocationSites::init(out_symbols);
    EventBuffers::init();
    AllocationSiteHotnessCounters::init();
    EventsGCRuntime::init();
    
    //worker thread starts here! make this one the last initialization ...
    active = true;
    if(TraceObjectsAsyncIO) {
        worker = new EventsWorkerThread(out_trace);
    } else {
        worker = NULL;
    }
    if(TraceObjectsCompressTrace || TraceObjectsCompressTraceAdaptively) {
        CompressionThread::initCompression();
        CompressionThread::add_CompressionThread(new CompressionThread("CompressorMaster", true));
        
        int i = 1;
        if(TraceObjectsCompressionThreads > 0){
            while(i < TraceObjectsCompressionThreads && i < os::processor_count()){
                CompressionThread::add_CompressionThread(new CompressionThread("CompressorFix", false));
                i++;
            }
        }else{
            int threadNR = log2_long(os::processor_count());
            if(threadNR > 1){
                i = 1;
                while(i < threadNR && i < os::processor_count()){
                    CompressionThread::add_CompressionThread(new CompressionThread("CompressorDyn", false));
                    i++;
                }
            }
        }
    }
    
    if(PrintTraceObjects) {
        char* symbols_name = out_symbols->get_file_name();
        char* class_definitions_name = out_class_definitions->get_file_name();
        char* trace_name = out_trace->get_file_name();
        AllocationTracing_log("tracing activated (symbols = %s, class_definitions = %s, trace = %s)",
                symbols_name, class_definitions_name, trace_name);
        free(symbols_name);
        free(trace_name);
    }
}

void AllocationTracing::destroy() {
    if(PrintTraceObjects) AllocationTracing_log("deactivating tracing");
    
    assert(active, "not initialized?");
    
    assert(SafepointSynchronize::is_at_safepoint(), "must be");
    
    EventBuffersFlushAll::flush_all();
    EventBuffersFlushAll::wait_for_all_serialized();
    
    if(TraceObjectsCheckAllocationSitesOnExit) {
        AllocationSites::check_consistency();
    }
    
    if(CompressionThread::isCompressionUsed() && TraceObjectsAsyncIO && (TraceObjectsCompressTrace || TraceObjectsCompressTraceAdaptively)){
        CompressionThread::interrupt_and_join();
        CompressionThread::clear_CompressionThreads();
    }

    if((PrintTraceObjects || TraceObjectsSelfMonitoringDumpOnExit) && TraceObjectsSelfMonitoring > 0) {
        AllocationTracingSelfMonitoring::dump();
    }
    
    CompressionThread::destroyCompressionAssets();

    if(worker != NULL) {
        worker->interrupt_and_join();
        worker = NULL; //threads always delete themselves
    }

    EventsGCRuntime::destroy();

    AllocationSiteHotnessCounters::destroy();
    EventBuffers::destroy();
    AllocationSites::destroy();
    AllocatedTypes::destroy();
    
    AllocationTracingSelfMonitoring::destroy();
    
    delete out_symbols; out_symbols = NULL;
    delete out_class_definitions; out_class_definitions = NULL;
    delete out_trace; out_trace = NULL;

    active = false;
    
    if(PrintTraceObjects) AllocationTracing_log("tracing deactivated");
}

class ToggleObjectTracing: public VM_Operation {
    public:
        ToggleObjectTracing() : VM_Operation() {}
        
        ~ToggleObjectTracing() {
            doit_epilogue();
        }
        
        virtual VMOp_Type type() const { return VMOp_ToggleTraceObjects; }
        
        virtual Mode evaluation_mode() const { return _safepoint; }
        
        virtual bool is_cheap_allocated() const { return false; }
        
        virtual bool doit_prologue(){
            Heap_lock->lock();
            return true;
        }
        
        virtual void doit_epilogue() {
            if(Heap_lock->is_locked()) {
                Heap_lock->unlock();
            }
        };
        
        virtual void doit() {
            if(TraceObjects) {
                AllocationTracing::destroy_when_running();
            } else {
                AllocationTracing::init_when_running();
            }
        }
};

void AllocationTracing::begin_init_when_running() {
    if(!TraceObjects) {
        ToggleObjectTracing op = ToggleObjectTracing();
        VMThread::execute(&op);
    }
}

void AllocationTracing::begin_destroy_when_running() {
    if(TraceObjects) {
        ToggleObjectTracing op = ToggleObjectTracing();
        VMThread::execute(&op);
    }    
}

void AllocationTracing::init_when_running() {
    assert(TraceObjectsToggleAtRunTime, "necessary");
    assert(Thread::current()->is_VM_thread(), "necessary");
    
    TraceObjects = true;
    
    AllocationTracing::init();
    AllocationTracingSelfMonitoring::reset();

    //destroy entire code cache
    {
        CodeCache::mark_all_nmethods_for_deoptimization();
        ResourceMark rm;
        DeoptimizationMarker dm;
        Deoptimization::deoptimize_dependents();
        CodeCache::make_marked_nmethods_not_entrant();
    }

    //sync GC
    {
        EventSynchronization::force_rotate();
        GCCauseSetter _(Universe::heap(), GCCause::_allocation_profiler);
        PSParallelCompact::invoke(false);
    }
}

void AllocationTracing::destroy_when_running() {
    assert(TraceObjectsToggleAtRunTime, "necessary");
    assert(Thread::current()->is_VM_thread(), "necessary");
    
    TraceObjects = false;
    
    AllocationTracing::destroy();
    
    //destroy entire code cache
    {
        CodeCache::mark_all_nmethods_for_deoptimization();
        ResourceMark rm;
        DeoptimizationMarker dm;
        Deoptimization::deoptimize_dependents();
        CodeCache::make_marked_nmethods_not_entrant();
    }
}

void AllocationTracing::log_humongous_allocation(size_t bytes) {
    Thread* thread = Thread::current();
    ResourceMark rm(thread);
    AllocationTracing_log_header();
    AllocationTracing_log_line("humongous allocation (%lib) @", bytes);
    if(thread->is_Java_thread()) {
        vframeStream stream = vframeStream((JavaThread*) thread);
        while(!stream.at_end()) {
            AllocationTracing_log_line("\t %s:%i", stream.method()->name_and_sig_as_C_string(), stream.bci());
            stream.next();
        }
    } else {
        AllocationTracing_log_line("<< no Java thread (%s), cannot walk stack >>", thread->name());
    }
    AllocationTracing_log_footer();
}

