/*
Copyright 2015 refractionPOINT

Licensed under the Apache License, Version 2.0 ( the "License" );
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define RPAL_FILE_ID                  99

#include <rpal/rpal.h>
#include <librpcm/librpcm.h>
#include "collectors.h"
#include <notificationsLib/notificationsLib.h>
#include <processLib/processLib.h>
#include <rpHostCommonPlatformLib/rTags.h>


#define _TIMEOUT_BETWEEN_MEM_REGIONS        (100)
#define _TIMEOUT_BETWEEN_CONSTANT_PROCESSS  (5*1000)
#define _MAX_CPU_WAIT                       (60)
#define _CPU_WATERMARK                      (50)


typedef struct
{
    RU64 base;
    RU64 size;
} _MemRange;

static
RBOOL
    isMemInModule
    (
        RU64 memBase,
        RU64 memSize,
        _MemRange* memRanges,
        RU32 nRanges
    )
{
    RBOOL isInMod = FALSE;
    RU32 i = 0;

    if( NULL != memRanges )
    {
        i = rpal_binsearch_array_closest( memRanges, 
                                          nRanges, 
                                          sizeof( _MemRange ), 
                                          &memBase, 
                                          (rpal_ordering_func)rpal_order_RU64,
                                          TRUE );
        if( (RU32)(-1) != i )
        {
            if( IS_WITHIN_BOUNDS( NUMBER_TO_PTR( memBase ), 
                                  memSize, 
                                  NUMBER_TO_PTR( memRanges[ i ].base ), 
                                  memRanges[ i ].size ) )
            {
                isInMod = TRUE;
            }
        }
    }

    return isInMod;
}

static
RBOOL
    assembleRanges
    (
        rList mods,
        _MemRange** pRanges,
        RU32* pNRanges
    )
{
    RBOOL isSuccess = FALSE;
    _MemRange* memRanges = NULL;
    rSequence mod = NULL;
    RU64 base = 0;
    RU64 size = 0;
    RU32 i = 0;

    if( rpal_memory_isValid( mods ) &&
        NULL != pRanges &&
        NULL != pNRanges )
    {
        if( NULL != ( memRanges = rpal_memory_alloc( sizeof( _MemRange ) *
                                                     rList_getNumElements( mods ) ) ) )
        {
            rList_resetIterator( mods );

            while( rList_getSEQUENCE( mods, RP_TAGS_DLL, &mod ) )
            {
                if( rSequence_getPOINTER64( mod, RP_TAGS_BASE_ADDRESS, &base ) &&
                    rSequence_getRU64( mod, RP_TAGS_MEMORY_SIZE, &size ) )
                {
                    memRanges[ i ].base = base;
                    memRanges[ i ].size = size;
                    i++;
                }
            }

            if( rpal_sort_array( memRanges, 
                                 i, 
                                 sizeof( _MemRange ), 
                                 (rpal_ordering_func)rpal_order_RU64 ) )
            {
                isSuccess = TRUE;
                *pRanges = memRanges;
                *pNRanges = i;
            }
        }
    }

    return isSuccess;
}

static
RPVOID
    lookForHiddenModulesIn
    (
        rEvent isTimeToStop,
        RU32 processId,
        rSequence originalRequest
    )
{
    rList mods = NULL;
    _MemRange* memRanges = NULL;
    RU32 nRanges = 0;
    rList map = NULL;
    rSequence region = NULL;
    RU8 memType = 0;
    RU8 memProtect = 0;
    RU64 memBase = 0;
    RU64 memSize = 0;

    RPU8 pMem = NULL;

    RBOOL isPrefetched = FALSE;
    RBOOL isCurrentExec = FALSE;
    RBOOL isHidden = FALSE;

    rSequence procInfo = NULL;

#ifdef RPAL_PLATFORM_WINDOWS
    PIMAGE_DOS_HEADER pDos = NULL;
    PIMAGE_NT_HEADERS pNt = NULL;
#endif

    rpal_debug_info( "looking for hidden modules in process %d.", processId );

    if( NULL != ( mods = processLib_getProcessModules( processId ) ) )
    {
        if( assembleRanges( mods, &memRanges, &nRanges ) )
        {
            if( NULL != ( map = processLib_getProcessMemoryMap( processId ) ) )
            {
                // Now we got all the info needed for a single process, compare
                while( rpal_memory_isValid( isTimeToStop ) &&
                       !rEvent_wait( isTimeToStop, 0 ) &&
                       ( isPrefetched || rList_getSEQUENCE( map, RP_TAGS_MEMORY_REGION, &region ) ) )
                {
                    if( isPrefetched )
                    {
                        isPrefetched = FALSE;
                    }

                    if( rSequence_getRU8( region, RP_TAGS_MEMORY_TYPE, &memType ) &&
                        rSequence_getRU8( region, RP_TAGS_MEMORY_ACCESS, &memProtect ) &&
                        rSequence_getPOINTER64( region, RP_TAGS_BASE_ADDRESS, &memBase ) &&
                        rSequence_getRU64( region, RP_TAGS_MEMORY_SIZE, &memSize ) )
                    {
                        if( PROCESSLIB_MEM_TYPE_PRIVATE == memType ||
                            PROCESSLIB_MEM_TYPE_MAPPED == memType )
                        {
                            if( PROCESSLIB_MEM_ACCESS_EXECUTE == memProtect ||
                                PROCESSLIB_MEM_ACCESS_EXECUTE_READ == memProtect ||
                                PROCESSLIB_MEM_ACCESS_EXECUTE_READ_WRITE == memProtect ||
                                PROCESSLIB_MEM_ACCESS_EXECUTE_WRITE_COPY == memProtect )
                            {
                                isCurrentExec = TRUE;
                            }
                            else
                            {
                                isCurrentExec = FALSE;
                            }

                            // This check is somewhat redundant since it the memory
                            // regions are already filtered by not allowing TYPE_IMAGE.
                            // It's now not that expensive so running it in parallel
                            // might catch some edge case.(?)
                            if( !isMemInModule( memBase, memSize, memRanges, nRanges ) )
                            {
                                // Exec memory found outside of a region marked to belong to
                                // a module, keep looking in for module.
                                if( ( 1024 * 1024 * 10 ) >= memSize &&
                                    processLib_getProcessMemory( processId,
                                                                 NUMBER_TO_PTR( memBase ),
                                                                 memSize,
                                                                 (RPVOID*)&pMem,
                                                                 TRUE ) )
                                {
                                    isHidden = FALSE;
#ifdef RPAL_PLATFORM_WINDOWS
                                    // Let's just check for MZ and PE for now, we can get fancy later.
                                    pDos = (PIMAGE_DOS_HEADER)pMem;
                                    if( IS_WITHIN_BOUNDS( (RPU8)pMem, 
                                                          sizeof( IMAGE_DOS_HEADER ), 
                                                          pMem, 
                                                          memSize ) &&
                                        IMAGE_DOS_SIGNATURE == pDos->e_magic )
                                    {
                                        pNt = (PIMAGE_NT_HEADERS)( (RPU8)pDos + pDos->e_lfanew );

                                        if( IS_WITHIN_BOUNDS( pNt, sizeof( *pNt ), pMem, memSize ) &&
                                            IMAGE_NT_SIGNATURE == pNt->Signature )
                                        {
                                            if( isCurrentExec )
                                            {
                                                // If the current region is exec, we've got a hidden module.
                                                isHidden = TRUE;
                                            }
                                            else
                                            {
                                                // We need to check if the next section in memory is
                                                // executable and outside of known modules since the PE
                                                // headers may have been marked read-only before the .text.
                                                if( rList_getSEQUENCE( map, RP_TAGS_MEMORY_REGION, &region ) )
                                                {
                                                    isPrefetched = TRUE;

                                                    if( ( PROCESSLIB_MEM_TYPE_PRIVATE == memType ||
                                                        PROCESSLIB_MEM_TYPE_MAPPED == memType ) &&
                                                        ( PROCESSLIB_MEM_ACCESS_EXECUTE == memProtect ||
                                                        PROCESSLIB_MEM_ACCESS_EXECUTE_READ == memProtect ||
                                                        PROCESSLIB_MEM_ACCESS_EXECUTE_READ_WRITE == memProtect ||
                                                        PROCESSLIB_MEM_ACCESS_EXECUTE_WRITE_COPY == memProtect ) )
                                                    {
                                                        isHidden = TRUE;
                                                    }
                                                }
                                            }
                                        }
                                    }
#elif defined( RPAL_PLATFORM_LINUX ) || defined( RPAL_PLATFORM_MACOSX )
                                    if( isCurrentExec &&
                                        0x7F == ( pMem )[ 0 ] &&
                                        'E' == ( pMem )[ 1 ] &&
                                        'L' == ( pMem )[ 2 ] &&
                                        'F' == ( pMem )[ 3 ] )
                                    {
                                        isHidden = TRUE;
                                    }
#endif

                                    rpal_memory_free( pMem );

                                    if( isHidden &&
                                        !rEvent_wait( isTimeToStop, 0 ) )
                                    {
                                        rpal_debug_info( "found a hidden module in %d.", processId );

                                        if( NULL != ( procInfo = processLib_getProcessInfo( processId ) ) )
                                        {
                                            if( !rSequence_addSEQUENCE( region, RP_TAGS_PROCESS, procInfo ) )
                                            {
                                                rSequence_free( procInfo );
                                            }
                                        }

                                        rSequence_addTIMESTAMP( region, 
                                                                RP_TAGS_TIMESTAMP, 
                                                                rpal_time_getGlobal() );
                                        hbs_markAsRelated( originalRequest, region );
                                        notifications_publish( RP_TAGS_NOTIFICATION_HIDDEN_MODULE_DETECTED, 
                                                               region );
                                        break;
                                    }

                                    rpal_thread_sleep( _TIMEOUT_BETWEEN_MEM_REGIONS );
                                }
                            }
                        }
                    }
                }

                rList_free( map );
            }

            rpal_memory_free( memRanges );
        }

        rList_free( mods );
    }

    return NULL;
}

static
RPVOID
    lookForHiddenModules
    (
        rEvent isTimeToStop,
        RPVOID ctx
    )
{
    rSequence originalRequest = (rSequence)ctx;
    processLibProcEntry* procs = NULL;
    processLibProcEntry* proc = NULL;

    if( NULL != ( procs = processLib_getProcessEntries( TRUE ) ) )
    {
        proc = procs;

        while( 0 != proc->pid &&
            rpal_memory_isValid( isTimeToStop ) &&
            !rEvent_wait( isTimeToStop, 0 ) )
        {
            lookForHiddenModulesIn( isTimeToStop, proc->pid, originalRequest );

            proc++;
        }

        rpal_memory_free( procs );
    }

    return NULL;
}

static
RVOID
    scan_for_hidden_module
    (
        rpcm_tag eventType,
        rSequence event
    )
{
    RU32 pid = (RU32)(-1);
    rEvent dummy = NULL;

    UNREFERENCED_PARAMETER( eventType );

    rSequence_getRU32( event, RP_TAGS_PROCESS_ID, &pid );
    
    if( NULL != ( dummy = rEvent_create( TRUE ) ) )
    {
        if( (RU32)( -1 ) == pid )
        {
            lookForHiddenModules( dummy, event );
        }
        else
        {
            lookForHiddenModulesIn( dummy, pid, event );
        }

        rEvent_free( dummy );
    }
}

static
RPVOID
    lookForHiddenModulesConstantly
    (
        rEvent isTimeToStop,
        RPVOID ctx
    )
{
    rSequence originalRequest = (rSequence)ctx;
    processLibProcEntry* procs = NULL;
    processLibProcEntry* proc = NULL;

    while( rpal_memory_isValid( isTimeToStop ) && 
           !rEvent_wait( isTimeToStop, 0 ) )
    {
        if( NULL != ( procs = processLib_getProcessEntries( TRUE ) ) )
        {
            proc = procs;

            while( 0 != proc->pid &&
                   rpal_memory_isValid( isTimeToStop ) &&
                   !rEvent_wait( isTimeToStop, _TIMEOUT_BETWEEN_CONSTANT_PROCESSS ) )
            {
                if( hbs_whenCpuBelow( _CPU_WATERMARK, _MAX_CPU_WAIT, isTimeToStop ) )
                {
                    lookForHiddenModulesIn( isTimeToStop, proc->pid, originalRequest );
                }

                proc++;
            }

            rpal_memory_free( procs );
        }
    }

    return NULL;
}

//=============================================================================
// COLLECTOR INTERFACE
//=============================================================================

rpcm_tag collector_5_events[] = { RP_TAGS_NOTIFICATION_HIDDEN_MODULE_DETECTED,
                                  0 };

RBOOL
    collector_5_init
    (
        HbsState* hbsState,
        rSequence config
    )
{
    RBOOL isSuccess = FALSE;
    
    UNREFERENCED_PARAMETER( config );

    if( NULL != hbsState )
    {
        if( notifications_subscribe( RP_TAGS_NOTIFICATION_HIDDEN_MODULE_REQ, 
                                     NULL, 
                                     0, 
                                     NULL, 
                                     scan_for_hidden_module ) &&
            rThreadPool_task( hbsState->hThreadPool, lookForHiddenModulesConstantly, NULL ) )
        {
            isSuccess = TRUE;
        }
        else
        {
            notifications_unsubscribe( RP_TAGS_NOTIFICATION_HIDDEN_MODULE_REQ, NULL, scan_for_hidden_module );
        }
    }

    return isSuccess;
}

RBOOL
    collector_5_cleanup
    (
        HbsState* hbsState,
        rSequence config
    )
{
    RBOOL isSuccess = FALSE;

    UNREFERENCED_PARAMETER( config );

    if( NULL != hbsState )
    {
        notifications_unsubscribe( RP_TAGS_NOTIFICATION_HIDDEN_MODULE_REQ, NULL, scan_for_hidden_module );

        isSuccess = TRUE;
    }

    return isSuccess;
}
