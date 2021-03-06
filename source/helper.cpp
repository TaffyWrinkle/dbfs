//****************************************************************************
//      Copyright (c) Microsoft Corporation. All rights reserved.
//      Licensed under the MIT license.
//
// File: helper.c
//
// Purpose:
//   This file contains the definitions of helper functions used by
//   sqlfs and SQLQuery.
//

#include "UtilsPrivate.h"

// ---------------------------------------------------------------------------
// Method: CalculateDumpPath
//
// Description:
//    This method concatenates the dump directory path to the provided 
//    relative path.
//
//    FUSE always gets paths relative to the mount directory.
//
// Returns:
//    VOID
//
std::string
CalculateDumpPath(
    string path)
{
    return(g_UserPaths.m_dumpPath + path);
}

// ---------------------------------------------------------------------------
// Method: ReturnErrnoAndPrintError
//
// Description:
//    This method prints the ERRNO error string along with the function that 
//    caused the error and a custom string that is passed.
//
// Returns:
//    -errno. Fuse always returns -errno.
//
int
ReturnErrnoAndPrintError(
    const char* func,
    std::string error_str)
{
    int     result;
    int     status;
    FILE*   outFile;

    result = -errno;
    (void)outFile;      //To suppress warning
    
    if (g_InVerbose)
    {
        outFile = stderr;
        status = SUCCESS;

        if (g_UseLogFile == true)
        {
            outFile = fopen(g_UserPaths.m_logfilePath.c_str(), "a");
            if (!outFile)
            {
                status = FAILURE;
            }
        }

        if (status == SUCCESS)
        {
            fprintf(outFile, "SQLFS Error in %s :: Reason - %s, Details - %s\n",
                func, error_str.c_str(), strerror(errno));

            if (outFile != stderr)
            {
                fclose(outFile);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Method: PrintError
//
// Description:
//    This method prints error message provided if verbose mode is enabled 
//    either on STDERR or the log file depending on whether the log file
//    paramater was passed at startup.
//
// Returns:
//    VOID
//
void
PrintMsg(const char* format, ...)
{
    FILE*   outFile;
    va_list argptr;
    int     status;

    (void)outFile;      //To suppress warning

    if (g_InVerbose)
    {
        outFile = stderr;
        status = SUCCESS;

        if (g_UseLogFile == true)
        {
            outFile = fopen(g_UserPaths.m_logfilePath.c_str(), "a");
            if (!outFile)
            {
                status = FAILURE;
            }
        }

        if (status == SUCCESS)
        {
            va_start(argptr, format);
            vfprintf(outFile, format, argptr);
            va_end(argptr);

            if (outFile != stderr)
            {
                fclose(outFile);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Method: GetServerDetails
//
// Description:
//    This method is a helper function used to get the details like 
//    hostname/IP, username and password for a given server name.
//
//    It searches the in-memory struct ServerInfo for this information.
//
//    The list is constant post initialization and read opeations will not
//    require synchronization primitives.
//
// Returns:
//    VOID
//
void
GetServerDetails(
    string servername,
    string& hostname,
    string& username,
    string& password)
{
    auto search = g_ServerInfoMap.find(servername);

    if (search != g_ServerInfoMap.end())
    {
        hostname = search->second->m_hostname;
        username = search->second->m_username;
        password = search->second->m_password;
    }
    else
    {
        PrintMsg("Unknown server %s\n", servername.c_str());
        KillSelf();
    }
}


// ---------------------------------------------------------------------------
// Method: CreateFile
//
// Description:
//    This method creates a file with write permissions. Or truncates to zero
//    if one already exists.
//
//    Note - Absolute path need to be provided to the function.
//
// Returns:
//    VOID
//
void
CreateFile(
    const char* path)
{
    FILE*   fp;

    fp = fopen(path, "w+");
    if (!fp)
    {
        PrintMsg("Error creating file %s %s\n", path, strerror(errno));
        KillSelf();
    }
    else
    {
        fclose(fp);
    }

    // Add the extended attribute indicating this is a locally created DMV
    // file.
    //
    // Keep size as 0 as we do not want to store a value for the attribute
    // - just want to create the attribute so pass in size as 0.
    //
    int status = setxattr(path,
                          g_LocallyGeneratedFiles, // name
                          "1",  // value
                          0,    // size
                          0);   // default value.
    if (status)
    {
        PrintMsg("Error setting extended attributes for file %s : %s\n", path, strerror(errno));
        KillSelf();
    }
}

// ---------------------------------------------------------------------------
// Method: IsDbfsFile
//
// Description:
//  This checks if the file pointed to by the provided path is a
//  dbfs file (created by this tool).
//
// Returns:
//    true if file is a dmv - otherwise false.
//
bool
IsDbfsFile(
    const char* path)
{
    string fpath;
    bool result = false;

    fpath = CalculateDumpPath(path);

    // Check if this is a DMV file by checking for the
    // custom attribute.
    //
    int length = getxattr(fpath.c_str(),
                          g_LocallyGeneratedFiles,
                          NULL,
                          0);

    // We expect the length to be 0.
    // -1 would mean that the value doesn't exist.
    //
    if (length >= 0)
    {
        result = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Method: CreateCustomQueriesDir
//
// Description:
//    Create a custom query directory and populate the directory with
// custom query output file. The file is empty until it is opened. When
// the file is opened, the content will be populated.
//
// Returns:
//    VOID
//
static void
CreateCustomQueriesDir(
    const string& dumpDir,
    const string& servername)
{
    int error;

    // Create the custom query folder for each server.
    //
    string customQueryFolderPath = dumpDir + LINUX_PATH_DELIM + CUSTOM_QUERY_FOLDER_NAME;
    error = mkdir(customQueryFolderPath.c_str(), DEFAULT_PERMISSIONS);
    if (error == 0)
    {
        // Create custom query output files.
        //
        CreateCustomQueriesOutputFiles(
            servername,
            customQueryFolderPath);
    }
    else
    {
        PrintMsg("mkdir failed for %s- %s\n", customQueryFolderPath.c_str(),
                 strerror(errno));
    }
}

// ---------------------------------------------------------------------------
// Method: CreateDMVFiles
//
// Description:
//    This method creates the empty DMV files for a given server.
//    The location of the files (as seen) is <MOUNT DIR>/<SERVER NAME>/. 
//    Of course the files are actually getting created in the dump directory.
//
//    The method requests the server for the list of DMV's and based on the
//    version of the server - may or may not create the .json files. 
//    Only for SQL Server 2016 (version 16) does the method create the .json.
//
//    This only happens at startup so no issue with synchronization.
//
// Returns:
//    VOID
//
static void
CreateDMVFiles(
    const string& dumpDir,
    const string& servername,
    const string& hostname,
    const string& username,
    const string& password,
    const int version)
{
    string          dmvQuery;
    string          filepath;
    string          responseString;
    int             error;
    vector<string>  filenames;
    int             numEntries;

    // Query SQL server for all the DMV files to be created.
    //
    // ** Note **
    // schema_id = 4 selects DMV's (leaves out INFORMATION_SCHEMA).
    //
    dmvQuery = "SELECT name from sys.system_views where schema_id = 4";
    error = ExecuteQuery(dmvQuery, responseString, hostname,
        username, password, TYPE_TSV);
    if (error)
    {
        PrintMsg("Failed to query DMV list\n");
    }
    else
    {
        // Tokenising response to extract DMV names.
        //
        filenames = Split(responseString, '\n');

        // On success, it will have at least two entries.
        numEntries = filenames.size();
        assert(numEntries > 1);

        // We need to skip the first name because the result of the SQL Query
        // includes the name of the column as well in the output.
        // We do not want to create a file corresponding to the column name.
        //
        for (int i = 0; i < numEntries; i++)
        {
            if (i == 0)
            {
                continue;
            }

            // Create the regular file - TSV.
            //
            filepath = StringFormat("%s/%s", dumpDir.c_str(), filenames[i].c_str());
            CreateFile(filepath.c_str());

            if (version >= 16)
            {
                // Creating the json file.
                //
                filepath = StringFormat("%s.json", filepath.c_str());
                CreateFile(filepath.c_str());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Method: CreateDbfsFiles
//
// Description:
//    This method creates the empty DMV files and custom query files
//    for a given server. The location of the files (as seen) is
//    <MOUNT DIR>/<SERVER NAME>/. Of course the files are actually getting
//    created in the dump directory.
//
// Returns:
//    VOID
//
void
CreateDbfsFiles(
    const string& servername,
    const string& hostname,
    const string& username,
    const string& password,
    const int version)
{
    string          fpath;
    int             error;

    fpath = CalculateDumpPath(servername);

    // Creating folder for this server's data.
    //
    error = mkdir(fpath.c_str(), DEFAULT_PERMISSIONS);
    if (error == 0)
    {
        CreateCustomQueriesDir(fpath, servername);

        CreateDMVFiles(fpath, servername, hostname, username, password, version);
    }
    else
    {
        PrintMsg("mkdir failed for %s- %s\n", fpath.c_str(), 
            strerror(errno));

        PrintMsg("There was an error creating the folders to hold the server DMV files. Exiting.\n");

        // Abort in case of any error.
        //
        KillSelf();
    }
}

// ---------------------------------------------------------------------------
// Method: KillSelf
//
// Description:
//    This method exits the program and in doing so the function DestroySQLFs
//    is called. This ensures a graceful shutdown of the program which
//    also ensures that the mount directory is unmounted at exit.
//
//    We are basically trying to leave the system back in the same state as
//    before the SQL Fs was stated. 
//
// Returns:
//    -1.
//
void
KillSelf()
{
    // Close SQLFs
    //
    kill(getpid(), SIGHUP);
}

// ---------------------------------------------------------------------------
// Method: GetServerInfo
//
// Description:
//    Given a server name, look up ServerInfo from ServerInfoMap.
//    The map was created by parsing the config file.
//
// Returns:
//    Pointer to ServerInfo object or NULL if the server does not exist
//
ServerInfo* GetServerInfo(
    const string& servername)
{
    ServerInfo* serverInfo = NULL;

    // Lookup the server name in the map
    //
    auto server = g_ServerInfoMap.find(servername);
    if (server != g_ServerInfoMap.end())
    {
        serverInfo = server->second;
    }

    return serverInfo;
}

// ---------------------------------------------------------------------------
// Method: GetUserCustomQueryPath
//
// Description:
//    Given a server name, get a customer query path that was specified by
//    the user in config file.
//
// Returns:
//    - Empty string if the server does not exist.
//    - Canonical path to custom query directory that user specify in config
//      file for that specific servername.
//
string GetUserCustomQueryPath(
    const string& servername)
{
    string customQueryPath;
    ServerInfo* serverInfo = GetServerInfo(servername);
    if (serverInfo)
    {
        customQueryPath = serverInfo->m_customQueriesPath;
    }
    return customQueryPath;
}
