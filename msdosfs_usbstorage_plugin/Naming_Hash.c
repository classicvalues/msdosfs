//
//  Naming_Hash.c
//  usbstorage_plugin
//
//  Created by Or Haimovich on 12/02/2019.
//

#include "Naming_Hash.h"
#include <errno.h>
#include <assert.h>
#include "Conv.h"
#include "DirOPS_Handler.h"
#include "Logger.h"

#define MAX_HASH_KEY (HASH_TABLE_SIZE)

/*
 * On macOS, We want to perform fast lookups for directories which contain up to 20K files.
 * 20K files was chosen because of the DCIM limitation of 10K files per dir.
 * (we assume 10K files and an Apple-Double file for each one --> 20K total).
 * on iOS, we have memory limitations, so we use a smaller HT.
 */
#if TARGET_OS_OSX
#define MAX_HASH_VALUES (20480)
#define LOW_BOUND_HASH_VALUES (20000)
#else
#define MAX_HASH_VALUES (4096)
#define LOW_BOUND_HASH_VALUES (4000)
#endif

// -------------------------------------------------------------------------------------
static uint32_t hash(struct unistr255* psName);
static void ht_remove_from_list(LF_HashTable_t* psHT, LF_TableEntry_t* psTEToRemove, LF_TableEntry_t* psPrevTE, uint32_t uKey);
static void ht_remove_rand_item(LF_HashTable_t* psHT, uint32_t uStartKey);
// -------------------------------------------------------------------------------------

/* creates hash for a hashtab */
static uint32_t hash(struct unistr255* psName)
{
    uint64_t hashval = 0;
    for (int i=0; i <  psName->length ; i++)
        hashval = psName->chars[i] + 31 * hashval;
    return (hashval % MAX_HASH_KEY);
}

static void ht_remove_from_list(LF_HashTable_t* psHT, LF_TableEntry_t* psTEToRemove, LF_TableEntry_t* psPrevTE, uint32_t uKey )
{
    //This is the first entry
    if (psPrevTE == NULL)
    {
        psHT->psEntries[uKey] = psHT->psEntries[uKey]->psNextEntry;
    }
    else
    {
        psPrevTE->psNextEntry = psTEToRemove->psNextEntry;
    }

    if (psHT->uCounter == 0)
    {
        // something wrong with our counting and we should never get here
        // but still need to free psTEToRemove memory
        MSDOS_LOG(LEVEL_FAULT, "ht_remove_from_list: hashtable is empty");
        free(psTEToRemove);
        return;
    }

    psHT->uCounter--;

    free(psTEToRemove);
}

/* force inserts the key-val pair */
static void ht_remove_rand_item(LF_HashTable_t* psHT, uint32_t uStartKey)
{
    bool bRemoved = false;
    uint32_t uRandKey = uStartKey;
    while (!bRemoved)
    {
        if (psHT->psEntries[uRandKey] != NULL)
        {
            ht_remove_from_list(psHT, psHT->psEntries[uRandKey], NULL, uRandKey);
            bRemoved = true;
        }
        uRandKey = rand() % MAX_HASH_KEY;
    }
}

int ht_AllocateHashTable(struct LF_HashTable** ppTable)
{
    *ppTable = (struct LF_HashTable*) malloc(sizeof(struct LF_HashTable));
    if (*ppTable == NULL)
        return ENOMEM;

    memset(*ppTable, 0, sizeof(struct LF_HashTable));
    MultiReadSingleWrite_Init(&(*ppTable)->sHTLck);
    return 0;
}

// Should be locked from outside the function
LF_TableEntry_t* ht_LookupByName(LF_HashTable_t* psHT, struct unistr255* psName, uint32_t* puKey)
{
    if (psHT == NULL) return NULL;
    uint32_t uKey = hash(psName);
    if (puKey != NULL)
        *puKey = uKey;

    return psHT->psEntries[uKey];
}

/* Assumtion : Under lock */
LF_TableEntry_t* ht_LookupByEntry(LF_HashTable_t* psHT, struct unistr255* psName, uint64_t uEntry, LF_TableEntry_t** ppsPrevTE, uint32_t* puKey)
{
    if (psHT == NULL) return NULL;
    CONV_Unistr255ToLowerCase( psName );
    LF_TableEntry_t* psTE = ht_LookupByName(psHT, psName, puKey);
    LF_TableEntry_t* psPrevTE = NULL;

    /* step through linked list */
    for (; psTE != NULL; psTE = psTE->psNextEntry)
    {
        if (psTE->uEntryOffsetInDir == uEntry)
        {
            if (ppsPrevTE != NULL)
                *ppsPrevTE = psPrevTE;

            MultiReadSingleWrite_FreeRead(&psHT->sHTLck);
            return psTE; /* found */
        }
        psPrevTE = psTE;
    }

    return NULL; /* not found */
}

/* inserts the key-val pair */
int ht_insert(LF_HashTable_t* psHT, struct unistr255* psName, uint64_t uEntryNum, bool bForceInsert)
{
    if (psHT == NULL) return EINVAL;

    int err = 0;
    uint32_t uKey;
    bool bNeedToEvictRandom = false;
    MultiReadSingleWrite_LockWrite(&psHT->sHTLck);

    //check if we can insert more values
    if (ht_reached_max_bound(psHT))
    {
        if (!bForceInsert)
        {
            psHT->bIncomplete = true;
            MultiReadSingleWrite_FreeWrite(&psHT->sHTLck);
            return 0;
        }
        else
        {
            bNeedToEvictRandom = true;
        }
    }

    LF_TableEntry_t* psTE = ht_LookupByEntry( psHT, psName, uEntryNum, NULL, &uKey);
    if (psTE != NULL)
    {
        err = EEXIST;
        goto exit;
    }

    //If we are in force insert and there is no space
    //evict a random item
    if (bNeedToEvictRandom)
    {
        ht_remove_rand_item(psHT, uKey);
    }

    LF_TableEntry_t* psPrevTE = NULL;
    LF_TableEntry_t* psNewTE = (LF_TableEntry_t*) malloc (sizeof(LF_TableEntry_t));
    if (psNewTE == NULL)
    {
        err= ENOMEM;
        goto exit;
    }

    psNewTE->psNextEntry = NULL;
    psNewTE->uEntryOffsetInDir = uEntryNum;
    psTE = psHT->psEntries[uKey];
    /* look for the correct place */
    /* if this is the first one */
    if (psTE == NULL)
    {
        psHT->psEntries[uKey] = psNewTE;
        goto exit;
    }

    for (; psTE != NULL; psTE = psTE->psNextEntry)
    {
        if (psTE->uEntryOffsetInDir > uEntryNum)
        {
            psNewTE->psNextEntry = psTE;
            (psPrevTE == NULL) ? (psHT->psEntries[uKey] = psNewTE) : (psPrevTE->psNextEntry = psNewTE);

            goto exit;
        }

        psPrevTE = psTE;
    }

    // We got to the last entry
    if (psTE == NULL && psPrevTE != NULL)
    {
        psPrevTE->psNextEntry = psNewTE;
    }

exit:
    if (!err) psHT->uCounter++;
    MultiReadSingleWrite_FreeWrite(&psHT->sHTLck);
    return err;
}

bool ht_reached_low_bound(LF_HashTable_t* psHT)
{
    if (psHT == NULL) return false;
    return (psHT->uCounter <= LOW_BOUND_HASH_VALUES);
}

bool ht_reached_max_bound(LF_HashTable_t* psHT)
{
    if (psHT == NULL) return false;
    return !(psHT->uCounter < MAX_HASH_VALUES);
}

int ht_remove(LF_HashTable_t* psHT, const char *pcUTF8Name, uint64_t uEntry)
{
    if (psHT == NULL) return EINVAL;
    LF_TableEntry_t* psPrevTE;
    uint32_t uKey;
    struct unistr255* psName = (struct unistr255*) malloc(sizeof(struct unistr255));
    if ( psName == NULL )
    {
        return ENOMEM;
    }
    memset( psName, 0, sizeof(struct unistr255));

    // Convert the search name to UTF-16
    uint32_t uFlags = (DIROPS_IsDotOrDotDotName(pcUTF8Name))? 0 : UTF_SFM_CONVERSIONS;
    int iError = CONV_UTF8ToUnistr255((const uint8_t *)pcUTF8Name, strlen(pcUTF8Name), psName, uFlags);
    if ( iError != 0 )
    {
        goto exit;
    }

    MultiReadSingleWrite_LockWrite(&psHT->sHTLck);
    
    LF_TableEntry_t* psTE = ht_LookupByEntry( psHT, psName, uEntry, &psPrevTE, &uKey);
    if (psTE == NULL)
    {
        MultiReadSingleWrite_FreeWrite(&psHT->sHTLck);
        iError = (psHT->bIncomplete)? 0 : ENOENT;
        goto exit;
    }

    ht_remove_from_list(psHT, psTE, psPrevTE, uKey);

    MultiReadSingleWrite_FreeWrite(&psHT->sHTLck);

exit:
    if (psName != NULL)
        free(psName);

    return iError;
}

/* recursively frees table entriy chains, starting with last one added */
void ht_free_all(LF_HashTable_t *psHT)
{
    if (psHT == NULL) return;
    MultiReadSingleWrite_LockWrite(&psHT->sHTLck);

    LF_TableEntry_t* psTE = NULL;
    LF_TableEntry_t* psNextTE = NULL;

    for (uint64_t uKey = 0; uKey < MAX_HASH_KEY; uKey++)
    {
        if (psHT->psEntries[uKey] != NULL)
        {
            for (psTE = psHT->psEntries[uKey]; psTE != NULL; psTE = psNextTE)
            {
                psNextTE = psTE->psNextEntry;
                free(psTE);
            }
            psHT->psEntries[uKey] = NULL;
        }
    }

    //Free the table as well
    MultiReadSingleWrite_FreeWrite(&psHT->sHTLck);
}

void ht_DeAllocateHashTable(LF_HashTable_t *psHT)
{
    if (psHT == NULL) return;
    ht_free_all(psHT);
    MultiReadSingleWrite_DeInit(&psHT->sHTLck);
    free(psHT);
}

void ht_set_incomplete(LF_HashTable_t* psHT, bool val)
{
    if (psHT == NULL) return;
    MultiReadSingleWrite_LockWrite(&psHT->sHTLck);
    psHT->bIncomplete = val;
    MultiReadSingleWrite_FreeWrite(&psHT->sHTLck);
}
