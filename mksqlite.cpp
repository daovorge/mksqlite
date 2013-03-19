/*
 * mksqlite: A MATLAB Interface To SQLite
 *
 * (c) 2008-2013 by M. Kortmann <mail@kortmann.de>
 * distributed under LGPL
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <string.h>
#define _strcmpi strcasecmp
#include <cctype>
#endif

#include <math.h>
#include <assert.h>
#include <limits.h>
#include <mex.h>
#include "sqlite3.h"
#include "deelx.h"

/* Versionnumber */
#define VERSION "1.13"

/* Default Busy Timeout */
#define DEFAULT_BUSYTIMEOUT 1000

/* get the SVN Revisionnumber */
#include "svn_revision.h"

// Data representation 
//(ref http://www.agner.org/optimize/calling_conventions.pdf, chapter 3)
// #include <stdint.h> doesn't compile with current MSVC and older matlab versions
#ifndef int32_t
#if UINT_MAX > 65535
  typedef signed   int         int32_t;  // int is usually 32 bits long
  typedef unsigned int         uint32_t; 
#else
  typedef signed   long        int32_t;  // except on 16-Bit Windows platform
  typedef unsigned long        uint32_t; 
#endif 
#endif
#ifndef int8_t
  typedef signed   char        int8_t;
  typedef unsigned char        uint8_t;
#endif
#ifndef int16_t
  typedef signed   short int   int16_t;
  typedef unsigned short int   uint16_t;
#endif
#ifndef INT32_MAX
#define INT32_MAX (0x3FFFFFFF)
#endif

// SQLite itself limits BLOBs to 1MB, mksqlite limits to INT32_MAX
#define MQSQLITE_MAX_BLOB_SIZE ((mwSize)INT32_MAX)

// static assertion (compile time), ensures int32_t and mwSize as 4 byte data representation
static char SA_UIN32[ (sizeof(uint32_t)==4 && sizeof(mwSize)==4) ? 1 : -1 ]; 

/* declare the MEX Entry function as pure C */
extern "C" void mexFunction(int nlhs, mxArray*plhs[], int nrhs, const mxArray*prhs[]);

/* Flag: Show the welcome message */
static bool FirstStart = false;

/* Flag: return NULL as NaN  */
static bool NULLasNaN  = false;
static const double g_NaN = mxGetNaN();

/* Flag: Check for unique fieldnames */
static bool check4uniquefields = true;

/* Convert UTF-8 to ascii, otherwise set slCharacterEncoding('UTF-8') */
static bool convertUTF8 = true;

/* Store type and dimensions of MATLAB vectors/arrays in BLOBs */
static bool use_typed_blobs   = false;
static const char TBH_MAGIC[] = "mkSQLite.tbh";
static char g_platform[11]    = {0};
static char g_endian[2]       = {0};
// typed BLOB header agreement
// native and free of matlab types, to provide data sharing with other applications
typedef struct {
  char magic[sizeof(TBH_MAGIC)];  // small fail-safe header check
  int16_t ver;                    // Struct size as kind of header version number for later backwards compatibility (may increase only!)
  int32_t clsid;                  // Matlab ClassID of variable (see mxClassID)
  char platform[11];              // Computer architecture: PCWIN, PCWIN64, GLNX86, GLNXA64, MACI, MACI64, SOL64
  char endian;                    // Byte order: 'L'ittle endian or 'B'ig endian
  int32_t sizeDims[1];            // Number of dimensions, followed by sizes of each dimension
                                  // First byte after header at &tbh->sizeDims[tbh->sizeDims[0]+1]
} typed_BLOB_header;
#define TBH_DATA(tbh)            ((void*)&tbh->sizeDims[tbh->sizeDims[0]+1])
#define TBH_DATA_OFFSET(nDims)   ((ptrdiff_t)&((typed_BLOB_header*) 0)->sizeDims[nDims+1])

/*
 * Table of used database ids.
 */
#define MaxNumOfDbs 5
static sqlite3* g_dbs[MaxNumOfDbs] = { 0 };

/*
 * Minimal structured "exception" handling with goto's, hence
 * a finally-block is missed to use try-catch-blocks here efficiently.
 * pseudo-code:
 **** %< ****
 * try-block
 * 
 * catch ( e )
 *  ->free local variables<-
 *  exit mex function and report error
 * 
 * catch (...)
 *  ->free local variables<-
 *  rethrow exception
 * 
 * ->free local variables<-
 * exit normally
 **** >% ****
 * 
 * Here, exception handling is done with goto's. Goto's are ugly and 
 * should be avoided in modern art of programming.
 * error handling is the lonely reason to use them: try-catch mechanism 
 * does the same but encapsulated and in a friendly and safe manner...
 */
static const char *g_finalize_msg = NULL;  // if assigned, function returns with an appropriate error message
static const char* SQL_ERR = "SQL_ERR";    // if attached to g_finalize_msg, function returns with least SQL error message
#define FINALIZE( msg ) { g_finalize_msg = msg; goto finalize; }
#define FINALIZEIF( cond, msg ) { if(cond) FINALIZE( msg ) }

void regexFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv);
void powFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv);

// memory freeing functions
void DestroyArray( mxArray *&pmxarr );
template <class T>
void Free( T *&pmxarr );


/*
 * a poor man localization.
 * every language have an table of messages.
 */

/* Number of message table to use */
static int Language = -1;

#define MSG_HELLO               messages[Language][ 0]
#define MSG_INVALIDDBHANDLE     messages[Language][ 1]
#define MSG_IMPOSSIBLE          messages[Language][ 2]
#define MSG_USAGE               messages[Language][ 3]
#define MSG_INVALIDARG          messages[Language][ 4]
#define MSG_CLOSINGFILES        messages[Language][ 5]
#define MSG_CANTCOPYSTRING      messages[Language][ 6]
#define MSG_NOOPENARG           messages[Language][ 7]
#define MSG_NOFREESLOT          messages[Language][ 8]
#define MSG_CANTOPEN            messages[Language][ 9]
#define MSG_DBNOTOPEN           messages[Language][10]
#define MSG_INVQUERY            messages[Language][11]
#define MSG_CANTCREATEOUTPUT    messages[Language][12]
#define MSG_UNKNWNDBTYPE        messages[Language][13]
#define MSG_BUSYTIMEOUTFAIL     messages[Language][14]
#define MSG_MSGUNIQUEWARN       messages[Language][15]
#define MSG_UNEXPECTEDARG       messages[Language][16]
#define MSG_MISSINGARG          messages[Language][17]
#define MSG_MEMERROR            messages[Language][18]
#define MSG_UNSUPPVARTYPE       messages[Language][19]
#define MSG_UNSUPPTBH           messages[Language][20]   
#define MSG_ERRPLATFORMDETECT   messages[Language][21]
#define MSG_WARNDIFFARCH        messages[Language][22]
#define MSG_BLOBTOOBIG          messages[Language][23]

/* 0 = english message table */
static const char* messages_0[] = 
{
    "mksqlite Version " VERSION " " SVNREV ", an interface from MATLAB to SQLite\n"
    "(c) 2008-2013 by Martin Kortmann <mail@kortmann.de>\n"
    "based on SQLite Version %s - http://www.sqlite.org\n"
    "mksqlite uses the perl compatible regex engine DEELX Version 1.2 - http://www.regexlab.com (Sswater@gmail.com)\n"
    "UTF-8, parameter binding, regex and typed BLOBs: A.Martin, 2013-02-25, Volkswagen AG\n\n",
    
    "invalid database handle\n",
    "function not possible",
    "Usage: mksqlite([dbid,] command [, databasefile])\n",
    "no or wrong argument",
    "mksqlite: closing open databases.\n",
    "Can\'t copy string in getstring()",
    "Open without Databasename\n",
    "No free databasehandle available\n",
    "cannot open database\n%s, ",
    "database not open",
    "invalid query string (Semicolon?)",
    "cannot create output matrix",
    "unknown SQLITE data type",
    "cannot set busytimeout",
    "could not build unique fieldname for %s",
    "unexpected arguments passed",
    "missing argument list",
    "memory allocation error",
    "unsupported variable type",
    "unknown/unsupported typed blob header",
    "error while detecting platform",
    "BLOB stored on different platform",
    "BLOB exceeds maximum allowed size"
};

/* 1 = german message table */
static const char* messages_1[] = 
{
    "mksqlite Version " VERSION " " SVNREV ", ein MATLAB Interface zu SQLite\n"
    "(c) 2008-2013 by Martin Kortmann <mail@kortmann.de>\n"
    "basierend auf SQLite Version %s - http://www.sqlite.org\n"
    "mksqlite verwendet die Perl kompatible regex engine DEELX Version 1.2 - http://www.regexlab.com (Sswater@gmail.com)\n"
    "UTF-8, parameter binding, regex und typisierte BLOBs: A.Martin, 2013-02-25, Volkswagen AG\n\n",
    
    "ung�ltiger Datenbankhandle\n",
    "Funktion nicht m�glich",
    "Verwendung: mksqlite([dbid,] Befehl [, datenbankdatei])\n",
    "kein oder falsches Argument �bergeben",
    "mksqlite: Die noch ge�ffneten Datenbanken wurden geschlossen.\n",
    "getstring() kann keine neue zeichenkette erstellen",
    "Open Befehl ohne Datenbanknamen\n",
    "Kein freier Datenbankhandle verf�gbar\n",
    "Datenbank konnte nicht ge�ffnet werden\n%s, ",
    "Datenbank nicht ge�ffnet",
    "ung�ltiger query String (Semikolon?)",
    "Kann Ausgabematrix nicht erstellen",
    "unbek. SQLITE Datentyp",
    "busytimeout konnte nicht gesetzt werden",
    "konnte keinen eindeutigen Bezeichner f�r Feld %s bilden",
    "Argumentliste zu lang",
    "keine Argumentliste angegeben",
    "Fehler bei Speichermanagement", 
    "Nicht unterst�tzter Variablentyp",
    "Unbekannter oder nicht unterst�tzter typisierter BLOB Header",
    "Fehler beim Identifizieren der Platform",
    "BLOB wurde unter abweichender Platform erstellt",
    "BLOB ist zu gro�"
};

/*
 * Message Tables
 */
static const char **messages[] = 
{
    messages_0,   /* English messages */
    messages_1    /* German messages  */
};

/*
 * Converto UTF-8 Strings to 8Bit and vice versa
 */
static int utf2latin( const unsigned char *s, unsigned char *buffer )
{
    int cnt = 0;
    unsigned char ch, *p = buffer ? buffer : &ch;

    if( s ) 
    {
        while( *s ) 
        {
            if( *s < 128 ) 
            {
                *p = *s++;
            }
            else 
            {
                *p = ( s[0] << 6 ) | ( s[1] & 63 );
                s += 2;
            }
            if( buffer ) 
            {
                p++;
            }
            cnt++;
        }
        *p = 0;
        cnt++;
    }

    return cnt;
}

static int latin2utf( const unsigned char *s, unsigned char *buffer )
{
    int cnt = 0;
    unsigned char ch[2], *p = buffer ? buffer : ch;

    if( s ) 
    {
        while( *s ) 
        {
            if( *s < 128 ) 
            {
                *p++ = *s++;
                cnt++;
            }
            else 
            {
                *p++ = 128 + 64 + ( *s >> 6 );
                *p++ = 128 + ( *s++ & 63 );
                cnt += 2;
            }
            if( !buffer ) 
            {
                p = ch;
            }
        }
        *p = 0;
        cnt++;
    }

    return cnt;
}

/*
 * duplicate a string, 
 */
static char* strnewdup(const char* s)
{
    char *newstr = 0;
    
    if (convertUTF8)
    {
        if (s)
        {
            int buflen = utf2latin( (unsigned char*)s, NULL );

            newstr = new char [buflen];
            if( newstr ) 
            {
                utf2latin( (unsigned char*)s, (unsigned char*)newstr );
            }
        }
    }
    else
    {
        if (s)
        {
            newstr = new char [strlen(s) +1];
            if (newstr)
                strcpy(newstr, s);
        }

    }
    
    return newstr;
}

/*
 * a single Value of an database row, including data type information
 */
class Value
{
public:
    int         m_Type;
    int         m_Size;

    char*       m_StringValue;
    double      m_NumericValue;
    
                Value ()  : m_Type(0), m_Size(0), 
                            m_StringValue(0), m_NumericValue(0.0) {}
    virtual    ~Value ()    { if (m_StringValue) delete [] m_StringValue; } 
};

/*
 * all values of an database row
 */
class Values
{
public:
    int         m_Count;
    Value*      m_Values;
    
    Values*     m_NextValues;
    
                Values(int n)   : m_Count(n), m_NextValues(0)
                                  { m_Values = new Value[n]; }
            
    virtual    ~Values()          { delete [] m_Values; }
};

/*
 * close left over databases.
 */
static void CloseDBs(void)
{
    /*
     * Is there any database left?
     */
    bool dbsClosed = false;
    for (int i = 0; i < MaxNumOfDbs; i++)
    {
        /*
         * close it
         */
        if (g_dbs[i])
        {
            sqlite3_close(g_dbs[i]);
            g_dbs[i] = 0;
            dbsClosed = true;
        }
    }
    if (dbsClosed)
    {
        /*
         * Set the language to english if something
         * goes wrong before the language could been set
         */
        if (Language < 0)
            Language = 0;
        /*
         * and inform the user
         */
        mexWarnMsgTxt (MSG_CLOSINGFILES);
    }
}

/*
 * Get the last SQLite Error Code as an Error Identifier
 */
static const char* TransErrToIdent(sqlite3 *db)
{
    static char dummy[32];

    int errorcode = sqlite3_errcode(db);
    
    switch(errorcode)
     {    
        case SQLITE_OK:         return ("SQLITE:OK");
        case SQLITE_ERROR:      return ("SQLITE:ERROR");
        case SQLITE_INTERNAL:   return ("SQLITE:INTERNAL");
        case SQLITE_PERM:       return ("SQLITE:PERM");
        case SQLITE_ABORT:      return ("SQLITE:ABORT");
        case SQLITE_BUSY:       return ("SQLITE:BUSY");
        case SQLITE_LOCKED:     return ("SQLITE:LOCKED");
        case SQLITE_NOMEM:      return ("SQLITE:NOMEM");
        case SQLITE_READONLY:   return ("SQLITE:READONLY");
        case SQLITE_INTERRUPT:  return ("SQLITE:INTERRUPT");
        case SQLITE_IOERR:      return ("SQLITE:IOERR");
        case SQLITE_CORRUPT:    return ("SQLITE:CORRUPT");
        case SQLITE_NOTFOUND:   return ("SQLITE:NOTFOUND");
        case SQLITE_FULL:       return ("SQLITE:FULL");
        case SQLITE_CANTOPEN:   return ("SQLITE:CANTOPEN");
        case SQLITE_PROTOCOL:   return ("SQLITE:PROTOCOL");
        case SQLITE_EMPTY:      return ("SQLITE:EMPTY");
        case SQLITE_SCHEMA:     return ("SQLITE:SCHEMA");
        case SQLITE_TOOBIG:     return ("SQLITE:TOOBIG");
        case SQLITE_CONSTRAINT: return ("SQLITE:CONSTRAINT");
        case SQLITE_MISMATCH:   return ("SQLITE:MISMATCH");
        case SQLITE_MISUSE:     return ("SQLITE:MISUSE");
        case SQLITE_NOLFS:      return ("SQLITE:NOLFS");
        case SQLITE_AUTH:       return ("SQLITE:AUTH");
        case SQLITE_FORMAT:     return ("SQLITE:FORMAT");
        case SQLITE_RANGE:      return ("SQLITE:RANGE");
        case SQLITE_NOTADB:     return ("SQLITE:NOTADB");
        case SQLITE_ROW:        return ("SQLITE:ROW");
        case SQLITE_DONE:       return ("SQLITE:DONE");

        default:
            sprintf (dummy, "SQLITE:%d", errorcode);
            return dummy;
     }
}

/*
 * Convert an String to char *
 */
static char *getstring(const mxArray *a)
{
    size_t count = mxGetM(a) * mxGetN(a) + 1;
    char *c = (char *) mxCalloc(count,sizeof(char));

    if (!c || mxGetString(a,c,(int)count))
        mexErrMsgTxt(MSG_CANTCOPYSTRING);

    if (convertUTF8)
    {
        char *buffer = NULL;
        int buflen;

        buflen = latin2utf( (unsigned char*)c, (unsigned char*)buffer );
        buffer = (char *) mxCalloc( buflen, sizeof(char) );

        if( !buffer )
        {
            Free( c ); // Needless due to mexErrMsgTxt(), but clean
            mexErrMsgTxt(MSG_CANTCOPYSTRING);
        }

        latin2utf( (unsigned char*)c, (unsigned char*)buffer );

        Free( c );

        return buffer;
    }
   
    return c;
}

/*
 * get an integer value from an numeric
 */
static int getinteger(const mxArray* a)
{
    switch (mxGetClassID(a))
    {
        case mxINT8_CLASS  : return (int) *((int8_t*)   mxGetData(a));
        case mxUINT8_CLASS : return (int) *((uint8_t*)  mxGetData(a));
        case mxINT16_CLASS : return (int) *((int16_t*)  mxGetData(a));
        case mxUINT16_CLASS: return (int) *((uint16_t*) mxGetData(a));
        case mxINT32_CLASS : return (int) *((int32_t*)  mxGetData(a));
        case mxUINT32_CLASS: return (int) *((uint32_t*) mxGetData(a));
        case mxSINGLE_CLASS: return (int) *((float*)    mxGetData(a));
        case mxDOUBLE_CLASS: return (int) *((double*)   mxGetData(a));
    }
    
    return 0;
}

/*
 * This ist the Entry Function of this Mex-DLL
 */
void mexFunction(int nlhs, mxArray*plhs[], int nrhs, const mxArray*prhs[])
{
    mxArray* pArgs       = NULL;  // Cell array of parameters behind command, in case of an SQL command only
    sqlite3_stmt *st     = NULL;  // SQL statement (sqlite bridge)
    char *command        = NULL;  // the SQL command (superseeded by query)
    g_finalize_msg       = NULL;  // pointer to actual error message
    int FirstArg         = 0;
    
    mexAtExit(CloseDBs);
    
    /*
     * Get the current Language
     */
    if (Language == -1)
    {
#ifdef _WIN32        
        switch(PRIMARYLANGID(GetUserDefaultLangID()))
        {
            case LANG_GERMAN:
                Language = 1;
                break;
                
            default:
                Language = 0;
        }
#else
        Language = 0;
#endif
    }
    
    /*
     * Print Version Information
     */
    if (! FirstStart)
    {
        FirstStart = true;

        mexPrintf (MSG_HELLO, sqlite3_libversion());
        
        mxArray *plhs[3] = {0};
        
        if( 0 == mexCallMATLAB( 3, plhs, 0, NULL, "computer" ) )
        {
            mxGetString( plhs[0], g_platform, sizeof( g_platform ) );
            mxGetString( plhs[2], g_endian, 2 );

            mexPrintf( "Platform: %s, %s\n\n", g_platform, g_endian[0] == 'L' ? "little endian" : "big endian" );
            
            DestroyArray( plhs[0] );
            DestroyArray( plhs[1] );
            DestroyArray( plhs[2] );
        }
        else
        {
            FirstStart = false;
            mexErrMsgTxt( MSG_ERRPLATFORMDETECT );
        }
    }
    
    int db_id = 0;
    int CommandPos = 0;
    int NumArgs = nrhs;
    
    /*
     * Check if the first argument is a number, then we have to use
     * this number as an database id.
     */
    if (nrhs >= 1 && mxIsNumeric(prhs[0]))
    {
        db_id = getinteger(prhs[0]);
        if (db_id < 0 || db_id > MaxNumOfDbs)
        {
            mexPrintf(MSG_INVALIDDBHANDLE);
            FINALIZE( MSG_IMPOSSIBLE );
        }
        db_id --;
        CommandPos ++;
        NumArgs --;
    }

    /*
     * no argument -> fail
     */
    if (NumArgs < 1)
    {
        mexPrintf(MSG_USAGE);
        FINALIZE( MSG_INVALIDARG );
    }
    
    /*
     * The next (or first if no db number available) is the command,
     * it has to be a string.
     * This fails also, if the first arg is a db-id and there is no 
     * further argument
     */
    if (! mxIsChar(prhs[CommandPos]))
    {
        mexPrintf(MSG_USAGE);
        FINALIZE( MSG_INVALIDARG );
    }
    
    /*
     * Get the command string
     */
    command = getstring(prhs[CommandPos]);
    
    /*
     * Adjust the Argument pointer and counter
     */
    FirstArg = CommandPos +1;
    NumArgs --;
    
    if (! strcmp(command, "version mex"))
    {
        if ( nlhs == 0 )
        {
            mexPrintf( "mksqlite Version %s\n", VERSION );
        } else {
            plhs[0] = mxCreateString( VERSION );
        }
    } else if (! strcmp(command, "version sql"))
    {
        if ( nlhs == 0 )
        {
            mexPrintf( "SQLite Version %s\n", SQLITE_VERSION );
        } else {
            plhs[0] = mxCreateString( SQLITE_VERSION );
        }
    } 
    else if (! strcmp(command, "open"))
    {
        /*
         * open a database. There has to be one string argument,
         * the database filename
         */
        if (NumArgs != 1 || !mxIsChar(prhs[FirstArg]))
        {
            mexPrintf(MSG_NOOPENARG, mexFunctionName());
            FINALIZE( MSG_INVALIDARG );
        }
        
        // No Memoryleak 'command not freed' when getstring fails
        // Matlab Help:
        // "If your application called mxCalloc or one of the 
        // mxCreate* routines to allocate memory, mexErrMsgTxt 
        // automatically frees the allocated memory."
        char* dbname = getstring(prhs[FirstArg]);

        /*
         * Is there an database ID? The close the database with the same id 
         */
        if (db_id > 0 && g_dbs[db_id])
        {
            sqlite3_close(g_dbs[db_id]);
            g_dbs[db_id] = 0;
        }

        /*
         * If there isn't an database id, then try to get one
         */
        if (db_id < 0)
        {
            for (int i = 0; i < MaxNumOfDbs; i++)
            {
                if (g_dbs[i] == 0)
                {
                    db_id = i;
                    break;
                }
            }
        }
        /*
         * no database id? sorry, database id table full
         */
        if (db_id < 0)
        {
            plhs[0] = mxCreateDoubleScalar((double) 0);
            mexPrintf(MSG_NOFREESLOT);
            Free(dbname);  // Needless due to mexErrMsgTxt(), but clean
            
            FINALIZE( MSG_IMPOSSIBLE );
        }
       
        /*
         * Open the database
         */
        int rc = sqlite3_open(dbname, &g_dbs[db_id]);
        
        if( SQLITE_OK != rc )
        {
            /*
             * Anything wrong? free the database id and inform the user
             */
            mexPrintf(MSG_CANTOPEN, sqlite3_errmsg(g_dbs[db_id]));
            sqlite3_close(g_dbs[db_id]);

            g_dbs[db_id] = 0;
            plhs[0] = mxCreateDoubleScalar((double) 0);
            
            Free(dbname);   // Needless due to mexErrMsgTxt(), but clean
            
            FINALIZE( MSG_IMPOSSIBLE );
        }
        
        /*
         * Set Default Busytimeout
         */
        rc = sqlite3_busy_timeout(g_dbs[db_id], DEFAULT_BUSYTIMEOUT);
        if( SQLITE_OK != rc )
        {
            /*
             * Anything wrong? free the database id and inform the user
             */
            mexPrintf(MSG_CANTOPEN, sqlite3_errmsg(g_dbs[db_id]));
            sqlite3_close(g_dbs[db_id]);

            g_dbs[db_id] = 0;
            plhs[0] = mxCreateDoubleScalar((double) 0);
            
            Free(dbname);   // Needless due to mexErrMsgTxt(), but clean
            
            FINALIZE( MSG_BUSYTIMEOUTFAIL );
        }
        
        // attach new SQL commands to opened database
        sqlite3_create_function( g_dbs[db_id], "pow", 2, SQLITE_UTF8, NULL, powFunc, NULL, NULL );     // power function (math)
        sqlite3_create_function( g_dbs[db_id], "regex", 2, SQLITE_UTF8, NULL, regexFunc, NULL, NULL ); // regular expressions (MATCH mode)
        sqlite3_create_function( g_dbs[db_id], "regex", 3, SQLITE_UTF8, NULL, regexFunc, NULL, NULL ); // regular expressions (REPLACE mode)
        
        /*
         * return value will be the used database id
         */
        plhs[0] = mxCreateDoubleScalar((double) db_id +1);
        Free(dbname);
    }
    else if (! strcmp(command, "close"))
    {
        /*
         * close a database
         */

        /*
         * There should be no Argument to close
         */
        FINALIZEIF( NumArgs > 0, MSG_INVALIDARG );
        
        /*
         * if the database id is < 0 than close all open databases
         */
        if (db_id < 0)
        {
            for (int i = 0; i < MaxNumOfDbs; i++)
            {
                if (g_dbs[i])
                {
                    sqlite3_close(g_dbs[i]);
                    g_dbs[i] = 0;
                }
            }
        }
        else
        {
            /*
             * If the database is open, then close it. Otherwise
             * inform the user
             */
            FINALIZEIF( ! g_dbs[db_id], MSG_DBNOTOPEN );

            sqlite3_close(g_dbs[db_id]);
            g_dbs[db_id] = 0;
        }
    }
    else if (! strcmp(command, "status"))
    {
        /*
         * There should be no Argument to status
         */
        FINALIZEIF( NumArgs > 0, MSG_INVALIDARG );
        
        for (int i = 0; i < MaxNumOfDbs; i++)
        {
            mexPrintf("DB Handle %d: %s\n", i, g_dbs[i] ? "OPEN" : "CLOSED");
        }
    }
    else if (! _strcmpi(command, "setbusytimeout"))
    {
        /*
         * There should be one Argument, the Timeout in ms
         */
        FINALIZEIF( NumArgs != 1 || !mxIsNumeric(prhs[FirstArg]), MSG_INVALIDARG );
        FINALIZEIF( ! g_dbs[db_id], MSG_DBNOTOPEN );

        /*
         * Set Busytimeout
         */
        int TimeoutValue = getinteger(prhs[FirstArg]);

        int rc = sqlite3_busy_timeout(g_dbs[db_id], TimeoutValue);
        if( SQLITE_OK != rc )
        {
            /*
             * Anything wrong? free the database id and inform the user
             */
            mexPrintf(MSG_CANTOPEN, sqlite3_errmsg(g_dbs[db_id]));
            sqlite3_close(g_dbs[db_id]);

            g_dbs[db_id] = 0;
            plhs[0] = mxCreateDoubleScalar((double) 0);

            FINALIZE( MSG_BUSYTIMEOUTFAIL );
        }
    }
    else if (! _strcmpi(command, "check4uniquefields"))
    {
        if (NumArgs == 0)
        {
            plhs[0] = mxCreateDoubleScalar((double) check4uniquefields);
        }
        else 
        {
            FINALIZEIF( NumArgs != 1 || !mxIsNumeric(prhs[FirstArg]), MSG_INVALIDARG );
            check4uniquefields = (getinteger(prhs[FirstArg])) ? true : false;
        }
    }
    else if (! _strcmpi(command, "convertUTF8"))
    {
        if (NumArgs == 0)
        {
            plhs[0] = mxCreateDoubleScalar((double) convertUTF8);
        }
        else
        {
            FINALIZEIF( NumArgs != 1 || !mxIsNumeric(prhs[FirstArg]), MSG_INVALIDARG );
            convertUTF8 = (getinteger(prhs[FirstArg])) ? true : false;
        }
    }
    else if (! _strcmpi(command, "typedBLOBs"))
    {
        if (NumArgs == 0)
        {
            plhs[0] = mxCreateDoubleScalar((double) use_typed_blobs);
        }
        else
        {
            FINALIZEIF( NumArgs != 1 || !mxIsNumeric(prhs[FirstArg]), MSG_INVALIDARG );
            use_typed_blobs = (getinteger(prhs[FirstArg])) ? true : false;
        }
    }
    else if (! _strcmpi(command, "NULLasNaN"))
    {
        if (NumArgs == 0)
        {
            plhs[0] = mxCreateDoubleScalar((double) NULLasNaN);
        }
        else
        {
            FINALIZEIF( NumArgs != 1 || !mxIsNumeric(prhs[FirstArg]), MSG_INVALIDARG );
            NULLasNaN = (getinteger(prhs[FirstArg])) ? true : false;
        }
    }
    else
    {
        /*
         * database id < 0? That's an error...
         */
        if (db_id < 0)
        {
            mexPrintf(MSG_INVALIDDBHANDLE);
            FINALIZE( MSG_IMPOSSIBLE );
        }
        
        /*
         * database not open? -> error
         */
        FINALIZEIF( !g_dbs[db_id], MSG_DBNOTOPEN );
        
        /*
         * Every unknown command is treated as an sql query string
         */
        const char* query = command;

        /*
         * emulate the "show tables" sql query
         */
        if (! _strcmpi(query, "show tables"))
        {
            query = "SELECT name as tablename FROM sqlite_master "
                    "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
                    "UNION ALL "
                    "SELECT name as tablename FROM sqlite_temp_master "
                    "WHERE type IN ('table','view') "
                    "ORDER BY 1";
        }

        /*
         * complete the query
         */
        // TODO: What does this test? sqlite3_complete() returns 1 if the string is complete and valid...
        FINALIZEIF( sqlite3_complete(query) != 0, MSG_INVQUERY );
        
        /*
         * and prepare it
         * if anything is wrong with the query, than complain about it.
         */
        FINALIZEIF( SQLITE_OK != sqlite3_prepare_v2(g_dbs[db_id], query, -1, &st, 0), SQL_ERR );
        
        /*
         * Parameter binding 
         */
        mwSize bind_names_count = sqlite3_bind_parameter_count( st );
        
        // If there are no placeholdes in the SQL statement, no
        // arguments are allowed. There must be at least one
        // placeholder.
        FINALIZEIF( !bind_names_count && NumArgs > 0, MSG_UNEXPECTEDARG );

        if ( !NumArgs || !mxIsCell( prhs[FirstArg] ))  // mxIsCell() not called when NumArgs==0 !
        {
            // Arguments passed as list, or no arguments
            // More parameters than needed is not allowed
            FINALIZEIF( NumArgs > (mwSize) bind_names_count, MSG_UNEXPECTEDARG ); 

            // Collect parameter list in one cell array
            pArgs = mxCreateCellMatrix( bind_names_count, 1 );
            for ( int i = 0; pArgs && i < bind_names_count; i++ )
            {
                if ( i < NumArgs )
                {
                    // deep copy into cell array
                    mxSetCell( pArgs, i, mxDuplicateArray( prhs[FirstArg+i] ) );
                }
                else
                {
                    // not passed arguments result in empty arrays
                    mxSetCell( pArgs, i, mxCreateLogicalMatrix( 0, 0 ) );
                }
            }
        }
        else
        {
            // Parameters may be (and are) packed in only one single 
            // cell array
            FINALIZEIF( NumArgs > 1, MSG_UNEXPECTEDARG );

            // Make a deep copy
            pArgs = mxDuplicateArray( prhs[FirstArg] );
        }
        
        // if parameters needed for parameter binding, 
        // at least one parameter has to be passed 
        FINALIZEIF( !pArgs, MSG_MISSINGARG );

        for ( int iParam = 0; iParam < bind_names_count; iParam++ )
        {
            // item must not be destroyed! (Matlab crashes)
            mxArray *item = mxGetCell( pArgs, iParam );

            if ( !item || mxIsEmpty( item ) )
                continue; // Empty parameters are omitted as NULL by sqlite

            if ( mxIsComplex( item ) || mxIsCell( item ) || mxIsStruct( item ) )
            {
                // No complex values, nested cells or structs allowed
                FINALIZE( MSG_UNSUPPVARTYPE );
            }

            size_t    szElement   = mxGetElementSize( item );      // size of one element in bytes
            size_t    cntElements = mxGetNumberOfElements( item ); // numer of elements in cell array
            mxClassID clsid       = mxGetClassID( item );
            char*     str_value   = NULL;

            if ( cntElements > 1 && clsid != mxCHAR_CLASS )
            {
                if( !use_typed_blobs ){
                    // matrix arguments are omitted as blobs, except string arguments
                    void* blob = mxGetData( item );
                    // SQLite makes a lokal copy of the blob (thru SQLITE_TRANSIENT)
                    if ( SQLITE_OK != sqlite3_bind_blob( st, iParam+1, blob, 
                                                         (mwSize) (cntElements * szElement), 
                                                         SQLITE_TRANSIENT ) )
                    {
                        FINALIZE( SQL_ERR );
                    }
                }
                else
                {
                    mwSize item_nDims        = mxGetNumberOfDimensions( item );
                    const mwSize *pitem_dims = mxGetDimensions( item );
                    mwSize sizeBlob          = (mwSize) TBH_DATA_OFFSET( item_nDims ) +
                                               (mwSize) (cntElements * szElement);

                    FINALIZEIF( sizeBlob > MQSQLITE_MAX_BLOB_SIZE, MSG_BLOBTOOBIG );
                    
                    switch( clsid )
                    {
                      case mxLOGICAL_CLASS:
                      case mxCHAR_CLASS:
                      case mxDOUBLE_CLASS:
                      case mxSINGLE_CLASS:
                      case mxINT8_CLASS:
                      case mxUINT8_CLASS:
                      case mxINT16_CLASS:
                      case mxUINT16_CLASS:
                      case mxINT32_CLASS:
                      case mxUINT32_CLASS:
                      case mxINT64_CLASS:
                      case mxUINT64_CLASS:
                        break;
                      default:
                        FINALIZE( MSG_UNSUPPVARTYPE );
                    }

                    void *blob = sqlite3_malloc( sizeBlob );
                    FINALIZEIF( NULL == blob, MSG_MEMERROR );

                    typed_BLOB_header* tbh = (typed_BLOB_header*) blob;
                    tbh->ver               = (int16_t)sizeof(*tbh);
                    tbh->clsid             = (int32_t)clsid;
                    tbh->endian            = g_endian[0];
                    tbh->sizeDims[0]       = (int32_t)item_nDims;
                    strcpy( tbh->magic, TBH_MAGIC );
                    strncpy( tbh->platform, g_platform, sizeof( g_platform ) - 1 );

                    for( mwSize j = 0; j < item_nDims; j++ )
                    {
                        tbh->sizeDims[j+1] = (int32_t)pitem_dims[j];
                    }

                    // TODO: Do byteswapping here if big endian? (Most platforms use little endian)
                    memcpy( TBH_DATA( tbh ), mxGetData( item ), (size_t) (cntElements * szElement) );

                    // sqlite takes custody of the blob, even if sqlite3_bind_blob() fails
                    if ( SQLITE_OK != sqlite3_bind_blob( st, iParam+1, blob, sizeBlob, sqlite3_free ) )
                    {
                        FINALIZE( SQL_ERR );
                    }
                }
            }
            else
            {
                switch ( clsid )
                {
                    case mxLOGICAL_CLASS:
                    case mxINT8_CLASS:
                    case mxUINT8_CLASS:
                    case mxINT16_CLASS:
                    case mxINT32_CLASS:
                    case mxUINT16_CLASS:
                    case mxUINT32_CLASS:
                        // scalar integer value
                        if ( SQLITE_OK != sqlite3_bind_int( st, iParam+1, (int)mxGetScalar( item ) ) )
                        {
                            FINALIZE( SQL_ERR );
                        }
                        break;
                    case mxDOUBLE_CLASS:
                    case mxSINGLE_CLASS:
                        // scalar floating point value
                        if ( SQLITE_OK != sqlite3_bind_double( st, iParam+1, mxGetScalar( item ) ) )
                        {
                            FINALIZE( SQL_ERR );
                        }
                        break;
                    case mxCHAR_CLASS:
                    {
                        // string argument
                        char* str_value = mxArrayToString( item );
                        if ( str_value && convertUTF8 )
                        {
                            int len = latin2utf( (unsigned char*)str_value, NULL ); // get the size only
                            unsigned char *temp = (unsigned char*)mxCalloc( len, sizeof(char) ); // allocate memory
                            if ( temp )
                            {
                                latin2utf( (unsigned char*)str_value, temp );
                                Free( str_value );
                                str_value = (char*)temp;
                            }
                            else
                            {
                                Free ( str_value );
                                FINALIZE( MSG_MEMERROR );
                            }
                        }
                        // SQLite makes a lokal copy of the blob (thru SQLITE_TRANSIENT)
                        if ( SQLITE_OK != sqlite3_bind_text( st, iParam+1, str_value, -1, SQLITE_TRANSIENT ) )
                        {
                            Free( str_value );
                            FINALIZE( SQL_ERR );
                        }
                        Free( str_value );
                        break;
                    }
                    default:
                        // other variable classed are invalid here
                        FINALIZE( MSG_INVALIDARG );
                }
            }
        }

        /*
         * Any results?
         */
        int ncol = sqlite3_column_count(st);
        if (ncol > 0)
        {
            char **fieldnames = new char *[ncol];   /* Column names */
            Values* allrows = 0;                    /* All query results */
            Values* lastrow = 0;                    /* pointer to the last result row */
            int rowcount = 0;                       /* number of result rows */
            
            /*
             * Get the column names of the result set
             */
            for( int iCol=0; iCol<ncol; iCol++ )
            {
                const char *cname = sqlite3_column_name(st, iCol);
                
                fieldnames[iCol] = new char [strlen(cname) +1];
                strcpy (fieldnames[iCol], cname);
                /*
                 * replace invalid chars by '_', so we can build
                 * valid MATLAB structs
                 */
                char *mk_c = fieldnames[iCol];
                while (*mk_c)
                {
//                    if ((*mk_c == ' ') || (*mk_c == '*') || (*mk_c == '?') || !isprint(*mk_c))
                    if ( !isalnum(*mk_c) )
                        *mk_c = '_';
                    mk_c++;
                }
            }
            /*
             * Check for duplicate colum names
             */
            if (check4uniquefields)
            {
                for( int iCol = 0; iCol < (ncol -1); iCol++ )
                {
                    for( int jCol = iCol+1; jCol < ncol; jCol++ )
                    {
                        if (! strcmp(fieldnames[iCol], fieldnames[jCol]))
                        {
                            /*
                             * Change the duplicate colum name to be unique
                             * by adding _x to it. x counts from 1 to 99
                             */
                            char *newcolumnname = new char [strlen(fieldnames[jCol]) + 4];
                            int k;
                            
                            for( k = 1; k < 100; k++ )
                            {
                                /*
                                 * Build new name
                                 */
                                sprintf(newcolumnname, "%s_%d", fieldnames[jCol], k);

                                /*
                                 * is it already unique? */
                                bool unique = true;
                                for( int lCol = 0; lCol < ncol; lCol++ )
                                {
                                    if (! strcmp(fieldnames[lCol], newcolumnname))
                                    {
                                        unique = false;
                                        break;
                                    }
                                }
                                if (unique)
                                    break;
                            }
                            if (k == 100)
                            {
                                mexWarnMsgTxt(MSG_MSGUNIQUEWARN);
                            }
                            else
                            {
                                /*
                                 * New unique Identifier found, assign it
                                 */
                                delete [] fieldnames[jCol];
                                fieldnames[jCol] = newcolumnname;
                            }
                        }
                    }
                }
            }
            /*
             * get the result rows from the engine
             *
             * We cannot get the number of result lines, so we must
             * read them in a loop and save them into an temporary list.
             * Later, we can transfer this List into an MATLAB array of structs.
             * This way, we must allocate enough memory for two result sets,
             * but we save time by allocating the MATLAB Array at once.
             */
            for(;;)
            {
                /*
                 * Advance to teh next row
                 */
                int step_res = sqlite3_step(st);

                /*
                 * no row left? break out of the loop
                 */
                if (step_res != SQLITE_ROW)
                    break;

                /*
                 * get new memory for the result
                 */
                Values* RecordValues = new Values(ncol);
                
                Value *v = RecordValues->m_Values;
                for (int jCol = 0; jCol < ncol; jCol++, v++)
                {
                     int fieldtype = sqlite3_column_type(st, jCol);

                     v->m_Type = fieldtype;
                     v->m_Size = 0;
                     
                     switch (fieldtype)
                     {
                         case SQLITE_NULL:      v->m_NumericValue = g_NaN;                                      break;
                         case SQLITE_INTEGER:   v->m_NumericValue = (double) sqlite3_column_int(st, jCol);      break;
                         case SQLITE_FLOAT:     v->m_NumericValue = (double) sqlite3_column_double(st, jCol);   break;
                         case SQLITE_TEXT:      v->m_StringValue  = strnewdup((const char*) sqlite3_column_text(st, jCol));   break;
                         case SQLITE_BLOB:      
                            {
                                v->m_Size = sqlite3_column_bytes(st, jCol);
                                if (v->m_Size > 0)
                                {
                                    v->m_StringValue = new char[v->m_Size];
                                    memcpy(v->m_StringValue, sqlite3_column_blob(st, jCol), v->m_Size);
                                }
                                else
                                {
                                    v->m_Size = 0;
                                }
                            }
                            break;
                         default:
                            FINALIZE( MSG_UNKNWNDBTYPE );
                     }
                }
                /*
                 * and add this row to the list of all result rows
                 */
                if (! lastrow)
                {
                    allrows = lastrow = RecordValues;
                }
                else
                {
                    lastrow->m_NextValues = RecordValues;
                    lastrow = lastrow->m_NextValues;
                }
                /*
                 * we have one more...
                 */
                rowcount ++;
            }
            
            /*
             * end the sql engine
             */
            sqlite3_finalize(st);
            st = NULL;

            /*
             * got nothing? return an empty result to MATLAB
             */
            if (rowcount == 0 || ! allrows)
            {
                plhs[0] = mxCreateDoubleMatrix(0,0,mxREAL);
                FINALIZEIF( NULL == plhs[0], MSG_CANTCREATEOUTPUT );
            }
            else
            {
                /*
                 * Allocate an array of MATLAB structs to return as result
                 */
                int ndims[2];
                int index;
                
                ndims[0] = rowcount;
                ndims[1] = 1;
                
                plhs[0] = mxCreateStructArray( 2, ndims, ncol, (const char**)fieldnames );
                FINALIZEIF( NULL == plhs[0], MSG_CANTCREATEOUTPUT );
                
                /*
                 * transfer the result rows from the temporary list into the result array
                 */
                lastrow = allrows;
                index = 0;
                while(lastrow)
                {
                    Value* recordvalue = lastrow->m_Values;
                    
                    for (int fieldnr = 0; fieldnr < ncol; fieldnr++, recordvalue++)
                    {
                        if (recordvalue -> m_Type == SQLITE_TEXT)
                        {
                            mxArray* c = mxCreateString(recordvalue->m_StringValue);
                            mxSetFieldByNumber(plhs[0], index, fieldnr, c);
                        }
                        else if (recordvalue -> m_Type == SQLITE_NULL && !NULLasNaN)
                        {
                            mxArray* out_double = mxCreateDoubleMatrix(0,0,mxREAL);
                            mxSetFieldByNumber(plhs[0], index, fieldnr, out_double);
                        }
                        else if (recordvalue -> m_Type == SQLITE_BLOB)
                        {
                            if (recordvalue->m_Size > 0)
                            {
                                if( !use_typed_blobs )
                                {
                                    int NumDims[2]={1,1};
                                    NumDims[1]=recordvalue->m_Size;
                                    mxArray*out_uchar8=mxCreateNumericArray(2, NumDims, mxUINT8_CLASS, mxREAL);
                                    unsigned char *v = (unsigned char *) mxGetData(out_uchar8);

                                    memcpy(v, recordvalue->m_StringValue, recordvalue->m_Size);

                                    mxSetFieldByNumber(plhs[0], index, fieldnr, out_uchar8);
                                }
                                else
                                {
                                    void* blob             = (void*)recordvalue->m_StringValue;
                                    typed_BLOB_header* tbh = (typed_BLOB_header*) blob;
                                    int16_t ver            = tbh->ver;
                                    
                                    if ( ver != (int16_t)sizeof( *tbh ) )
                                    {
                                        // TODO: handle new header versions here...
                                        FINALIZE( MSG_UNSUPPTBH );
                                    }
                                    
                                    mxClassID clsid      = (mxClassID)tbh->clsid;
                                    char* platform       = tbh->platform;
                                    char endian          = tbh->endian;
                                    int32_t item_nDims   = tbh->sizeDims[0];
                                    int32_t* pSize       = &tbh->sizeDims[1];
                                    int32_t sizeBlob     = (int32_t)((ptrdiff_t)recordvalue->m_Size - TBH_DATA_OFFSET( item_nDims ));
                                    
                                    if( g_endian[0] != endian || 0 != strncmp( g_platform, platform, sizeof( g_platform ) - 1 ) )
                                    {
                                        mexWarnMsgTxt( MSG_WARNDIFFARCH );
                                        // TODO: warning, error or automatic conversion..?
                                        // since mostly platforms (except SunOS) use LE encoding
                                        // and unicode is not supported here, there is IMHO no need 
                                        // for conversions...
                                    }
                                    
                                    if ( 0 != strncmp( tbh->magic, TBH_MAGIC, strlen( TBH_MAGIC ) ) || ver != (int16_t)sizeof( *tbh ) )
                                    {
                                        FINALIZE( MSG_UNSUPPTBH );
                                    }
                                    
                                    switch( clsid )
                                    {
                                        case mxLOGICAL_CLASS:
                                        case mxCHAR_CLASS:
                                        case mxDOUBLE_CLASS:
                                        case mxSINGLE_CLASS:
                                        case mxINT8_CLASS:
                                        case mxUINT8_CLASS:
                                        case mxINT16_CLASS:
                                        case mxUINT16_CLASS:
                                        case mxINT32_CLASS:
                                        case mxUINT32_CLASS:
                                        case mxINT64_CLASS:
                                        case mxUINT64_CLASS:
                                        break;
                                      default:
                                        FINALIZE( MSG_UNSUPPVARTYPE );
                                    }
                                    
                                    mxArray *array = mxCreateNumericArray( item_nDims, pSize, clsid, mxREAL );
                                    FINALIZEIF( NULL == array, MSG_MEMERROR );
                                    FINALIZEIF( sizeBlob != mxGetNumberOfElements( array ) * mxGetElementSize( array ), MSG_INVALIDARG );
                                    
                                    // TODO: Do byteswapping here if needed, depend on endian?
                                    memcpy( (void*)mxGetData( array ), TBH_DATA( tbh ), sizeBlob );
                                    mxSetFieldByNumber(plhs[0], index, fieldnr, array);
                                }
                            }
                            else
                            {
                                // empty BLOB
                                mxArray* out_double = mxCreateDoubleMatrix(0,0,mxREAL);
                                mxSetFieldByNumber(plhs[0], index, fieldnr, out_double);
                            }
                        }
                        else
                        {
                            mxArray* out_double = mxCreateDoubleScalar(recordvalue->m_NumericValue);
                            mxSetFieldByNumber(plhs[0], index, fieldnr, out_double);
                        }
                    }
                    allrows = lastrow;
                    lastrow = lastrow->m_NextValues;
                    delete allrows;
                    index++;
                }
            }
            for( int iCol=0; iCol<ncol; iCol++ )
                delete [] fieldnames[iCol];
            delete [] fieldnames;
        }
        else
        {
            /*
             * no result, cleanup the sqlite engine
             */
            int res = sqlite3_step(st);
            sqlite3_finalize(st);
            st = NULL;

            plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL);
            FINALIZEIF( NULL == plhs[0], MSG_CANTCREATEOUTPUT );
            FINALIZEIF( SQLITE_DONE != res, SQL_ERR );
        }
    }
        
finalize:        
    if ( st )
    {
        sqlite3_clear_bindings( st );
        sqlite3_finalize( st );
    }
    
    Free( command );  
    DestroyArray( pArgs );

    // mexErrMsg*() functions automatically free all 
    // allocated memory by mxCalloc() ans mxCreate*() functions.

    if( g_finalize_msg == SQL_ERR )
    {
        mexErrMsgIdAndTxt(TransErrToIdent(g_dbs[db_id]), sqlite3_errmsg(g_dbs[db_id]));
    }
    
    if( g_finalize_msg )
    {
        mexErrMsgTxt( g_finalize_msg );
    }
}

void powFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
    assert( argc == 2 ) ;
    double base, exponent, result;
    switch( sqlite3_value_type( argv[0] ) )
    {
        case SQLITE_NULL:
            sqlite3_result_null( ctx );
            return;
        default:
            base = sqlite3_value_double( argv[0] );
    }
    
    switch( sqlite3_value_type( argv[1] ) )
    {
        case SQLITE_NULL:
            sqlite3_result_null( ctx );
            return;
        default:
            exponent = sqlite3_value_double( argv[1] );
    }

    try
    {
        result = pow( base, exponent );
    }
    catch( ... )
    {
        sqlite3_result_error( ctx, "pow(): evaluation error", -1 );
        return;
    }
    sqlite3_result_double( ctx, result );
}

void regexFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
    assert( argc >= 2 );
    char *str, *pattern, *replace = NULL;
    
    sqlite3_result_null( ctx );
    
    str = strnewdup( (const char*)sqlite3_value_text( argv[0] ) );
    pattern = strnewdup( (const char*)sqlite3_value_text( argv[1] ) );
    
    if( argc > 2 )
    {
        replace = strnewdup( (const char*)sqlite3_value_text( argv[2] ) );
    }
    
    CRegexpT <char> regexp( pattern );

    // find and match
    MatchResult result = regexp.Match( str );

    // result
    if( result.IsMatched() )
    {
        char *str_value = NULL;
        
        if( argc == 2 )
        {
            // Match mode
            int start = result.GetStart();
            int end   = result.GetEnd  ();
            int len   = end - start;

            str_value = (char*)mxCalloc( len + 1, sizeof(char) );
            
            if( str_value && len > 0 )
            {
                strncpy( str_value, &str[start], len );
            }
        }
        else
        {
            // Replace mode
            char* result = regexp.Replace( str, replace );
            
            if( result )
            {
                int len = (int)strlen( result );
                str_value = (char*)mxCalloc( len + 1, sizeof(char) );
                
                if( str_value && len )
                {
                    strncpy( str_value, result, len );
                }
                
                CRegexpT<char>::ReleaseString( result );
            }
        }
        
        if( str_value && convertUTF8 )
        {
            int len = latin2utf( (unsigned char*)str_value, NULL ); // get the size only
            char *temp = (char*)mxCalloc( len, sizeof(char) ); // allocate memory
            if ( temp )
            {
                latin2utf( (unsigned char*)str_value, (unsigned char*)temp );
                Free( str_value );
                str_value = temp;
            }
        }
        
        if( str_value )
        {
            sqlite3_result_text( ctx, str_value, -1, SQLITE_TRANSIENT );
            Free( str_value );
        }
    }
   
    if( str )
    {
        delete[] str;
    }
    
    if( pattern )
    {
        delete[] pattern;
    }
    
    if( replace )
    {
        delete[] replace;
    }
}


// Matlab documentation is missing the issue, if mxFree and mxDestroyArray
// accept NULL pointers. Tested without crash, but what will be in further 
// Matlab versions...
// So we do it on our own:

void DestroyArray( mxArray *&pmxarr )
{
    if( pmxarr )
    {
        mxDestroyArray( pmxarr );
        pmxarr = NULL;
    }
}

template <class T>
void Free( T *&pmxarr )
{
    if( pmxarr )
    {
        mxFree( (void*)pmxarr );
        pmxarr = NULL;
    }
}

/*
 *
 * Formatierungsanweisungen f�r den Editor vim
 *
 * vim:ts=4:ai:sw=4
 */
