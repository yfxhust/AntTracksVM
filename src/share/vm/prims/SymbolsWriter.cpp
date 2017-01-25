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
 * File:   SymbolsWriter.cpp
 * Author: vmb
 * 
 * Created on February 25, 2014, 11:51 AM
 */

#include "precompiled.hpp"
#include "SymbolsWriter.hpp"
#include "AllocatedTypes.hpp"
#include "AllocationSites.hpp"
#include "oops/method.hpp"
#include "memory/resourceArea.hpp"
#include "AllocationTracingSynchronization.hpp"
#include "oops/instanceMirrorKlass.hpp"

#define ANCHOR_FLAG_INDEX 0
#define POINTERS_FLAG_INDEX 1
#define FRAGMENTED_HEAP_FLAG_INDEX 2
char const l = 'L';
char const semicolon = ';';
char const nul = ' ';

#ifdef _WINDOWS
#define snprintf _snprintf
#endif

//#define SPECIFY_NAME

SymbolsWriter::SymbolsWriter() : AllocationTracingIO(TraceObjectsSymbolsFile){
    init();
}

void SymbolsWriter::write_header() {
    jint flags = 0;
    if(TraceObjectsInsertAnchors) {
        flags |= (1 << ANCHOR_FLAG_INDEX);
    }
    //if(!UseG1GC){
        if(TraceObjectsPointers) {
            flags |= (1 << POINTERS_FLAG_INDEX);
        }
    //}
    if (UseConcMarkSweepGC) {
        flags |= (1 << FRAGMENTED_HEAP_FLAG_INDEX);
    } else {
        flags &= ~(1 << FRAGMENTED_HEAP_FLAG_INDEX);
    }
    write(&flags, sizeof(jint));
    write(&HeapWordSize, sizeof(jint));
    
#ifdef SPECIFY_NAME
    const char* trace = TraceObjectsTraceFile;
    size_t length = strlen(trace);
    int start = length - 1;
    while(start >= 0 && trace[start] != '/') start--;
    start++;
    write(trace + start, length - start + 1); // + 1 because of 0 byte
#else
    const char trace = '\0';
    write(&trace, 1);
#endif
    
    for(jint cause_id = 0; cause_id < (int) GCCause::_last_gc_cause; cause_id++) {
        GCCause::Cause cause = static_cast<GCCause::Cause>(cause_id);
        jbyte magic_byte = MAGIC_BYTE_GC_CAUSE;
        const char* name = GCCause::to_string(cause);
        bool common = cause == GCCause::_allocation_failure || cause == GCCause::_adaptive_size_policy || cause == GCCause::_g1_inc_collection_pause;
        jbyte kind = common ? 1 : 0;
        write(&magic_byte, sizeof(jbyte) * 1);
        write(&cause_id, sizeof(jint));
        write(name, (strlen(name) + 1) * sizeof(char));
        write(&kind, sizeof(jbyte));
    }
}

int SymbolsWriter::get_file_type() {
    return FILE_TYPE_SYMBOLS;
}

void SymbolsWriter::write_symbols(SymbolsSite symbol){
    ResourceMark rm(Thread::current());
    
    synchronized(get_lock()) {
        size_t size_before = size();
        
        jbyte magic_byte = MAGIC_BYTE_SITE;
        write(&magic_byte, sizeof(jbyte) * 1);
        if(is_big_allocation_site(symbol.allocation_site_id)){ //if big allocSite
            jshort hbyte_allocSite = symbol.allocation_site_id >> 8;
            write(&hbyte_allocSite, sizeof(jshort)); //write 2 highest bytes first
            write(&symbol.allocation_site_id, sizeof(jbyte) * 1); //then write lowbyte
        } else { //if small allocSite
            write(&symbol.allocation_site_id, SIZE_OF_ALLOCATION_SITE_IDENTIFIER_SMALL * 1);
        }
        
        write(&symbol.call_sites->length, sizeof(jint));
        for(size_t index = 0; index < symbol.call_sites->length; index++) {
            CallSite* site = symbol.call_sites->elements + index;
            char* signature;
            if(site->method != NULL) { 
                signature = site->method->name_and_sig_as_C_string();
            } else {
                const char* format = "$$Recursion.repeat_%i_last_frames_n_times()V";
                signature = (char*) Thread::current()->resource_area()->Amalloc(strlen(format) * 2);
                sprintf(signature, format, site->bytecode_index);
            }
            write(signature, sizeof(char) * strlen(signature));
            write(&site->bytecode_index, sizeof(jint) * 1);
        }
        
        write(&symbol.allocated_type_id, sizeof(AllocatedTypeIdentifier) * 1);
    
        flush();
        
        self_monitoring(1) {
            AllocationTracingSelfMonitoring::report_symbol_written(size() - size_before);
        }
    }
}

void SymbolsWriter::write_symbols(SymbolsSimpleSite symbol) {
    synchronized(get_lock()) {
        size_t size_before = size();

        jbyte magic_byte = MAGIC_BYTE_SIMPLE_SITE;
        write(&magic_byte, sizeof(jbyte) * 1);
        if(is_big_allocation_site(symbol.allocation_site_id)){ //if big allocSite
            jshort hbyte_allocSite = symbol.allocation_site_id >> 8;
            write(&hbyte_allocSite, sizeof(jshort)); //write 2 highest bytes first 
            write(&symbol.allocation_site_id, sizeof(jbyte) * 1); //then write lowbyte
        } else { //if small allocSite
            write(&symbol.allocation_site_id, SIZE_OF_ALLOCATION_SITE_IDENTIFIER_SMALL * 1);
        }
        
        write(symbol.signature, sizeof(char) * strlen(symbol.signature) + 1);
        write(&symbol.allocated_type_id, sizeof(AllocatedTypeIdentifier) * 1);
        flush();
        
        self_monitoring(1) {
            AllocationTracingSelfMonitoring::report_symbol_written(size() - size_before);
        }
    }
}

void SymbolsWriter::write_symbols(SymbolsType symbol, oop o, Klass* k){
    ResourceMark rm(Thread::current());

    synchronized(get_lock()) {
        size_t size_before = size();
        
        jbyte magic_byte = MAGIC_BYTE_TYPE;
        write(&magic_byte, sizeof(jbyte) * 1);
        write(&symbol.allocated_type_id, sizeof(AllocatedTypeIdentifier) * 1);
        
        const char* signature = symbol.allocated_type->signature_name();
        write(signature, strlen(signature));        
        write(&symbol.allocated_type_size, sizeof(jint) * 1);
        
        if(o != NULL && k != NULL){
            assert(o->is_instanceMirror(), "Where does the oop come from?");
            magic_byte = MAGIC_BYTE_MIRROR_INFO;
            signature = k->signature_name();
            
            write(&magic_byte, sizeof(byte) * 1);
            write(signature, strlen(signature));
            if(k->oop_is_instance()){
                write_fields((InstanceKlass*) k, false);
                magic_byte = MAGIC_BYTE_TYPE_SUPER_INFO;
                InstanceKlass* super = (InstanceKlass*) symbol.allocated_type;
                do {
                    super = super->superklass();
                    if(super!=NULL) {
                        write(&magic_byte, sizeof(byte) * 1);
                        write_fields(super, false);
                    }
                } while(super != NULL);   
            }
            
        } else
            if(symbol.allocated_type->oop_is_instance()){
            magic_byte = MAGIC_BYTE_TYPE_INFO;
            write(&magic_byte, sizeof(byte) * 1);
            write_fields((InstanceKlass*) symbol.allocated_type, true);
            
            magic_byte = MAGIC_BYTE_TYPE_SUPER_INFO;
            InstanceKlass* super = (InstanceKlass*) symbol.allocated_type;
            do {
                super = super->superklass();
                if(super!=NULL) {
                    
                    write(&magic_byte, sizeof(byte) * 1);
                    write_fields(super, true);
                }
            } while(super != NULL);     
        } 
        flush();
        
        self_monitoring(1) {
            AllocationTracingSelfMonitoring::report_symbol_written(size() - size_before);
        }
    }
    
    
}

void SymbolsWriter::write_fields(InstanceKlass* type, bool reverse){
    instanceKlassHandle handle (Thread::current(), type);
    int count = handle->java_fields_count();
    int i = reverse ? count - 1 : 0;
    FieldInfo* info;
    ConstantPool* constants = handle->constants();
        
    char buf[256];   
    write(&count, sizeof(jint) * 1);                 
     
    while(reverse? i >= 0 : i < count){
        write(&i, sizeof(jint) * 1);                 // used as offset
                
        info = handle->field_info(i);
        reverse ? i-- : i++;
        
        Symbol* signature = info->signature(constants);                
        signature->as_klass_external_name(buf, sizeof(buf));
        int len = (int)strlen(buf);     
        write(buf, sizeof(char) * len);
                
        Symbol* name = info->name(constants);
        name->as_klass_external_name(buf, sizeof(buf));
        
        len = (int)strlen(buf);
        write(buf, sizeof(char) * len);        
        write(&nul, sizeof(char) * 1);
        
        int flags = info->access_flags();
        write(&flags, sizeof(jshort) * 1);    
    }
}
