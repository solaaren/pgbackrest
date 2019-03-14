/***********************************************************************************************************************************
Storage Helper
***********************************************************************************************************************************/
#include <string.h>

#include "common/debug.h"
#include "common/memContext.h"
#include "common/regExp.h"
#include "config/config.h"
#include "protocol/helper.h"
#include "storage/driver/cifs/storage.h"
#include "storage/driver/posix/storage.h"
#include "storage/driver/remote/storage.h"
#include "storage/driver/s3/storage.h"
#include "storage/helper.h"

/***********************************************************************************************************************************
Storage path constants
***********************************************************************************************************************************/
STRING_EXTERN(STORAGE_SPOOL_ARCHIVE_IN_STR,                         STORAGE_SPOOL_ARCHIVE_IN);
STRING_EXTERN(STORAGE_SPOOL_ARCHIVE_OUT_STR,                        STORAGE_SPOOL_ARCHIVE_OUT);

STRING_EXTERN(STORAGE_PATH_ARCHIVE_STR,                             STORAGE_PATH_ARCHIVE);
STRING_EXTERN(STORAGE_PATH_BACKUP_STR,                              STORAGE_PATH_BACKUP);

/***********************************************************************************************************************************
Local variables
***********************************************************************************************************************************/
static struct
{
    MemContext *memContext;                                         // Mem context for storage helper

    Storage *storageLocal;                                          // Local read-only storage
    Storage *storageLocalWrite;                                     // Local write storage
    Storage *storageRepo;                                           // Repository read-only storage
    Storage *storageSpool;                                          // Spool read-only storage
    Storage *storageSpoolWrite;                                     // Spool write storage

    String *stanza;                                                 // Stanza for storage
    bool stanzaInit;                                                // Has the stanza been initialized?
    RegExp *walRegExp;                                              // Regular expression for identifying wal files
} storageHelper;

/***********************************************************************************************************************************
Create the storage helper memory context
***********************************************************************************************************************************/
static void
storageHelperInit(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.memContext == NULL)
    {
        MEM_CONTEXT_BEGIN(memContextTop())
        {
            storageHelper.memContext = memContextNew("storageHelper");
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN_VOID();
}

/***********************************************************************************************************************************
Initialize the stanza and error if it changes
***********************************************************************************************************************************/
static void
storageHelperStanzaInit(const bool stanzaRequired)
{
    FUNCTION_TEST_VOID();

    // If the stanza is NULL and the storage has not already been initialized then initialize the stanza
    if (!storageHelper.stanzaInit)
    {
        if (stanzaRequired && cfgOptionStr(cfgOptStanza) == NULL)
            THROW(AssertError, "stanza cannot be NULL for this storage object");

        MEM_CONTEXT_BEGIN(storageHelper.memContext)
        {
            storageHelper.stanza = strDup(cfgOptionStr(cfgOptStanza));
            storageHelper.stanzaInit = true;
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN_VOID();
}

/***********************************************************************************************************************************
Get a local storage object
***********************************************************************************************************************************/
const Storage *
storageLocal(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.storageLocal == NULL)
    {
        storageHelperInit();

        MEM_CONTEXT_BEGIN(storageHelper.memContext)
        {
            storageHelper.storageLocal = storageDriverPosixInterface(
                storageDriverPosixNew(
                    FSLASH_STR, STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, false, NULL));
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN(storageHelper.storageLocal);
}

/***********************************************************************************************************************************
Get a writable local storage object

This should be used very sparingly.  If writes are not needed then always use storageLocal() or a specific storage object instead.
***********************************************************************************************************************************/
const Storage *
storageLocalWrite(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.storageLocalWrite == NULL)
    {
        storageHelperInit();

        MEM_CONTEXT_BEGIN(storageHelper.memContext)
        {
            storageHelper.storageLocalWrite = storageDriverPosixInterface(
                storageDriverPosixNew(
                    FSLASH_STR, STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, true, NULL));
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN(storageHelper.storageLocalWrite);
}

/***********************************************************************************************************************************
Construct a repo path from an expression and path
***********************************************************************************************************************************/
static String *
storageRepoPathExpression(const String *expression, const String *path)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, expression);
        FUNCTION_TEST_PARAM(STRING, path);
    FUNCTION_TEST_END();

    ASSERT(expression != NULL);

    String *result = NULL;

    if (strEqZ(expression, STORAGE_REPO_ARCHIVE))
    {
        // Contruct the base path
        if (storageHelper.stanza != NULL)
            result = strNewFmt(STORAGE_PATH_ARCHIVE "/%s", strPtr(storageHelper.stanza));
        else
            result = strNew(STORAGE_PATH_ARCHIVE);

        // If a subpath should be appended, determine if it is WAL path, else just append the subpath
        if (path != NULL)
        {
            StringList *pathSplit = strLstNewSplitZ(path, "/");
            String *file = strLstSize(pathSplit) == 2 ? strLstGet(pathSplit, 1) : NULL;

            if (file != NULL && regExpMatch(storageHelper.walRegExp, file))
                strCatFmt(result, "/%s/%s/%s", strPtr(strLstGet(pathSplit, 0)), strPtr(strSubN(file, 0, 16)), strPtr(file));
            else
                strCatFmt(result, "/%s", strPtr(path));
        }
    }
    else if (strEqZ(expression, STORAGE_REPO_BACKUP))
    {
        // Contruct the base path
        if (storageHelper.stanza != NULL)
            result = strNewFmt(STORAGE_PATH_BACKUP "/%s", strPtr(storageHelper.stanza));
        else
            result = strNew(STORAGE_PATH_BACKUP);

        // Append subpath if provided
        if (path != NULL)
            strCatFmt(result, "/%s", strPtr(path));
    }
    else
        THROW_FMT(AssertError, "invalid expression '%s'", strPtr(expression));

    FUNCTION_TEST_RETURN(result);
}

/***********************************************************************************************************************************
Get the repo storage
***********************************************************************************************************************************/
static Storage *
storageRepoGet(const String *type, bool write)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, type);
        FUNCTION_TEST_PARAM(BOOL, write);
    FUNCTION_TEST_END();

    ASSERT(type != NULL);

    Storage *result = NULL;

    // Use remote storage
    if (!repoIsLocal())
    {
        result = storageDriverRemoteInterface(
            storageDriverRemoteNew(
                STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, write, storageRepoPathExpression,
                protocolRemoteGet(protocolStorageTypeRepo)));
    }
    // Use the CIFS driver
    else if (strEqZ(type, STORAGE_TYPE_CIFS))
    {
        result = storageDriverCifsInterface(
            storageDriverCifsNew(
                cfgOptionStr(cfgOptRepoPath), STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, write,
                storageRepoPathExpression));
    }
    // Use the Posix driver
    else if (strEqZ(type, STORAGE_TYPE_POSIX))
    {
        result = storageDriverPosixInterface(
            storageDriverPosixNew(
                cfgOptionStr(cfgOptRepoPath), STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, write,
                storageRepoPathExpression));
    }
    // Use the S3 driver
    else if (strEqZ(type, STORAGE_TYPE_S3))
    {
        result = storageDriverS3Interface(
            storageDriverS3New(
                cfgOptionStr(cfgOptRepoPath), write, storageRepoPathExpression, cfgOptionStr(cfgOptRepoS3Bucket),
                cfgOptionStr(cfgOptRepoS3Endpoint), cfgOptionStr(cfgOptRepoS3Region), cfgOptionStr(cfgOptRepoS3Key),
                cfgOptionStr(cfgOptRepoS3KeySecret), cfgOptionTest(cfgOptRepoS3Token) ? cfgOptionStr(cfgOptRepoS3Token) : NULL,
                cfgOptionTest(cfgOptRepoS3Host) ? cfgOptionStr(cfgOptRepoS3Host) : NULL,
                STORAGE_DRIVER_S3_PORT_DEFAULT, STORAGE_DRIVER_S3_TIMEOUT_DEFAULT, cfgOptionBool(cfgOptRepoS3VerifySsl),
                cfgOptionTest(cfgOptRepoS3CaFile) ? cfgOptionStr(cfgOptRepoS3CaFile) : NULL,
                cfgOptionTest(cfgOptRepoS3CaPath) ? cfgOptionStr(cfgOptRepoS3CaPath) : NULL));
    }
    else
        THROW_FMT(AssertError, "invalid storage type '%s'", strPtr(type));

    FUNCTION_TEST_RETURN(result);
}

/***********************************************************************************************************************************
Get a read-only repository storage object
***********************************************************************************************************************************/
const Storage *
storageRepo(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.storageRepo == NULL)
    {
        storageHelperInit();
        storageHelperStanzaInit(false);

        MEM_CONTEXT_BEGIN(storageHelper.memContext)
        {
            storageHelper.walRegExp = regExpNew(STRING_CONST("^[0-F]{24}"));
            storageHelper.storageRepo = storageRepoGet(cfgOptionStr(cfgOptRepoType), false);
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN(storageHelper.storageRepo);
}

/***********************************************************************************************************************************
Get a spool storage object
***********************************************************************************************************************************/
static String *
storageSpoolPathExpression(const String *expression, const String *path)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, expression);
        FUNCTION_TEST_PARAM(STRING, path);
    FUNCTION_TEST_END();

    ASSERT(expression != NULL);
    ASSERT(storageHelper.stanza != NULL);

    String *result = NULL;

    if (strEqZ(expression, STORAGE_SPOOL_ARCHIVE_IN))
    {
        if (path == NULL)
            result = strNewFmt(STORAGE_PATH_ARCHIVE "/%s/in", strPtr(storageHelper.stanza));
        else
            result = strNewFmt(STORAGE_PATH_ARCHIVE "/%s/in/%s", strPtr(storageHelper.stanza), strPtr(path));
    }
    else if (strEqZ(expression, STORAGE_SPOOL_ARCHIVE_OUT))
    {
        if (path == NULL)
            result = strNewFmt(STORAGE_PATH_ARCHIVE "/%s/out", strPtr(storageHelper.stanza));
        else
            result = strNewFmt(STORAGE_PATH_ARCHIVE "/%s/out/%s", strPtr(storageHelper.stanza), strPtr(path));
    }
    else
        THROW_FMT(AssertError, "invalid expression '%s'", strPtr(expression));

    FUNCTION_TEST_RETURN(result);
}

/***********************************************************************************************************************************
Get a read-only spool storage object
***********************************************************************************************************************************/
const Storage *
storageSpool(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.storageSpool == NULL)
    {
        storageHelperInit();
        storageHelperStanzaInit(true);

        MEM_CONTEXT_BEGIN(storageHelper.memContext)
        {
            storageHelper.storageSpool = storageDriverPosixInterface(
                storageDriverPosixNew(
                    cfgOptionStr(cfgOptSpoolPath), STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, false,
                    storageSpoolPathExpression));
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN(storageHelper.storageSpool);
}

/***********************************************************************************************************************************
Get a writable spool storage object
***********************************************************************************************************************************/
const Storage *
storageSpoolWrite(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.storageSpoolWrite == NULL)
    {
        storageHelperInit();
        storageHelperStanzaInit(true);

        MEM_CONTEXT_BEGIN(storageHelper.memContext)
        {
            storageHelper.storageSpoolWrite = storageDriverPosixInterface(
                storageDriverPosixNew(
                    cfgOptionStr(cfgOptSpoolPath), STORAGE_MODE_FILE_DEFAULT, STORAGE_MODE_PATH_DEFAULT, true,
                    storageSpoolPathExpression));
        }
        MEM_CONTEXT_END();
    }

    FUNCTION_TEST_RETURN(storageHelper.storageSpoolWrite);
}

/***********************************************************************************************************************************
Free all storage helper objects.

This should be done on any config load to ensure that stanza changes are honored.  Currently this is only done in testing, but in
the future it will likely be done in production as well.
***********************************************************************************************************************************/
void
storageHelperFree(void)
{
    FUNCTION_TEST_VOID();

    if (storageHelper.memContext != NULL)
        memContextFree(storageHelper.memContext);

    memset(&storageHelper, 0, sizeof(storageHelper));

    FUNCTION_TEST_RETURN_VOID();
}
