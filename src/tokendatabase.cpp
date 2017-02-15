/**
 * Database responsible for resolving tokens between translation units
 * There is a filename database that manages filename to ID mappings. To
 * facilitate data updates between multiple token databases, each token
 * database should have a reference to the same filename database.
 *
 */

#include "tokendatabase.h"

#include <wx/filename.h>
#include <wx/string.h>
#include <iostream>
#include <wx/mstream.h>
#include <clang-c/Index.h>

#include "cclogger.h"

enum
{
    ClTokenPacketType_filenames = 1<<0,
    ClTokenPacketType_tokens = 1<<1
};

/** @brief Write an int to an output stream
 *
 * @param out wxOutputStream&
 * @param val const int
 * @return bool
 *
 */
static bool WriteInt( wxOutputStream& out, const int val )
{
    out.Write((const void*)&val, sizeof(val));
    return true;
}

/** @brief Write a long long to an outputstream
 *
 * @param out wxOutputStream&
 * @param val const longlong
 * @return bool
 *
 */
static bool WriteLongLong( wxOutputStream& out, const long long val )
{
    out.Write((const void*)&val, sizeof(val));
    return true;
}

/** @brief Write a string to an output stream
 *
 * @param out wxOutputStream&
 * @param str const char*
 * @return bool
 *
 */
static bool WriteString( wxOutputStream& out, const char* str )
{
    int len = 0;

    if (str != nullptr)
        len = strlen(str); // Need size in amount of bytes
    if (!WriteInt(out, len))
        return false;
    if (len > 0)
        out.Write((const void*)str, len);
    return true;
}

/** @brief Read an int from an input stream
 *
 * @param in wxInputStream&
 * @param out_Int int&
 * @return bool
 *
 */
static bool ReadInt( wxInputStream& in, int& out_Int )
{
    int val = 0;
    if (!in.CanRead())
        return false;
    in.Read(&val, sizeof(val));
    if (in.LastRead() != sizeof(val))
        return false;
    out_Int = val;

    return true;
}

/** @brief Read a long long from an input stream
 *
 * @param in wxInputStream&
 * @param out_LongLong long long&
 * @return bool
 *
 */
static bool ReadLongLong( wxInputStream& in, long long& out_LongLong )
{
    long long val = 0;
    if (!in.CanRead())
        return false;
    in.Read(&val, sizeof(val));
    if (in.LastRead() != sizeof(val))
        return false;
    out_LongLong = val;

    return true;
}

/** @brief Read a string from an input stream
 *
 * @param in wxInputStream&
 * @param out_String wxString&
 * @return bool
 *
 */
static bool ReadString( wxInputStream& in, wxString& out_String )
{
    int len;
    if (!ReadInt(in, len))
        return false;
    if (len == 0)
    {
        out_String = out_String.Truncate(0);
        return true;
    }
    if (!in.CanRead())
        return false;
    char buffer[len + 1];

    in.Read( buffer, len );
    buffer[len] = '\0';

    out_String = wxString::FromUTF8(buffer);

    return true;
}

/** @brief Write a token to an output stream
 *
 * @param token const ClAbstractToken&
 * @param out wxOutputStream&
 * @return bool
 *
 */
bool ClIndexToken::WriteOut( const ClIndexToken& token,  wxOutputStream& out )
{
    // This is a cached database so we don't care about endianness for now. Who will ever copy these from one platform to another?
    WriteString( out, token.identifier.utf8_str());
    WriteString( out, token.displayName.utf8_str());
    WriteString( out, token.USR.utf8_str() );
    WriteInt(out, (int)token.locationList.size());
    for (std::vector<ClIndexTokenLocation>::const_iterator it = token.locationList.begin(); it != token.locationList.end(); ++it )
    {
        WriteInt(out, it->tokenType);
        WriteInt(out, it->fileId);
        WriteInt(out, it->range.beginLocation.line);
        WriteInt(out, it->range.beginLocation.column);
        WriteInt(out, it->range.endLocation.line);
        WriteInt(out, it->range.endLocation.column);
    }
    WriteInt(out, (int)token.parentTokenList.size());
    for (std::vector< std::pair<wxString, wxString> >::const_iterator it = token.parentTokenList.begin(); it != token.parentTokenList.end(); ++it)
    {
        WriteString( out, it->first.utf8_str() );
        WriteString( out, it->second.utf8_str() );
    }
    WriteString( out, token.scope.first.utf8_str() );
    WriteString( out, token.scope.second.utf8_str() );
    return true;
}

/** @brief Read a token from an input stream
 *
 * @param token ClAbstractToken&
 * @param in wxInputStream&
 * @return bool
 *
 */
bool ClIndexToken::ReadIn( ClIndexToken& token, wxInputStream& in )
{
    token.locationList.clear();
    token.parentTokenList.clear();
    int val = 0;
    if (!ReadString( in, token.identifier) )
        return false;
    if (!ReadString( in, token.displayName) )
        return false;
    if (!ReadString( in, token.USR ))
        return false;

    // Token position list
    if (!ReadInt(in, val))
        return false;
    for (unsigned int i = 0; i < (unsigned int)val; ++i)
    {
        int typ = 0;
        int fileId = 0;
        int beginLine = 0;
        int beginColumn = 0;
        int endLine = 0;
        int endColumn = 0;
        if (!ReadInt(in, typ))
            return false;
        if (!ReadInt(in, fileId))
            return false;
        if (!ReadInt(in, beginLine))
            return false;
        if (!ReadInt(in, beginColumn))
            return false;
        if (!ReadInt(in, endLine))
            return false;
        if (!ReadInt(in, endColumn))
            return false;
        token.locationList.push_back( ClIndexTokenLocation(static_cast<ClTokenType>(typ), fileId, ClTokenRange(ClTokenPosition(beginLine,beginColumn), ClTokenPosition(endLine, endColumn))));
        token.tokenTypeMask = static_cast<ClTokenType>(token.tokenTypeMask | typ);
    }

    // Child USR list
    if (!ReadInt(in, val))
        return false;

    for (unsigned int i = 0; i< (unsigned int)val; ++i)
    {
        wxString identifier;
        if (!ReadString( in, identifier ))
            return false;
        wxString USR;
        if (!ReadString( in, USR ))
            return false;
        token.parentTokenList.push_back( std::make_pair( identifier, USR ) );
    }
    if (!ReadString( in, token.scope.first ))
        return false;
    if (!ReadString( in, token.scope.second ))
        return false;

    return true;
}

/** @brief Filename database constructor
 */
ClFilenameDatabase::ClFilenameDatabase() :
    m_pFileEntries(new ClTreeMap<ClFilenameEntry>())
{

}

ClFilenameDatabase::ClFilenameDatabase(const ClFilenameDatabase& Other) :
    m_pFileEntries( Other.m_pFileEntries )
{

}

ClFilenameDatabase::~ClFilenameDatabase()
{
    delete m_pFileEntries;
}

/** @brief Write a filename database to an output stream
 *
 * @param db const ClFilenameDatabase&
 * @param out wxOutputStream&
 * @return bool
 *
 */
bool ClFilenameDatabase::WriteOut( const ClFilenameDatabase& db, wxOutputStream& out )
{
    int i;
    int cnt = db.m_pFileEntries->GetCount();

    WriteInt(out, cnt);
    for (i = 0; i < cnt; ++i)
    {
        ClFilenameEntry entry = db.m_pFileEntries->GetValue((ClFileId)i);
        if (!WriteString(out, entry.filename.utf8_str()))
            return false;
        long long ts = 0;
        if (entry.timestamp.IsValid())
            ts = entry.timestamp.GetValue().GetValue();
        if (!WriteLongLong(out, ts))
            return false;
    }
    return true;
}

/** @brief Read a filename database from an input stream
 *
 * @param db ClFilenameDatabase&
 * @param in wxInputStream&
 * @return bool
 *
 */
bool ClFilenameDatabase::ReadIn( ClFilenameDatabase& db, wxInputStream& in )
{
    int i;
    int packetCount = 0;
    if (!ReadInt(in, packetCount))
        return false;
    CCLogger::Get()->DebugLog( F(wxT("Reading %d filenames"), packetCount) );
    for (i = 0; i < packetCount; ++i)
    {
        wxString filename;
        if (!ReadString(in, filename))
            return false;
        long long ts = 0;
        if (!ReadLongLong(in, ts))
            return false;
        db.m_pFileEntries->Insert(filename, ClFilenameEntry(filename, wxDateTime(wxLongLong(ts))));
    }
    return true;
}

bool ClFilenameDatabase::HasFilename( const wxString &filename ) const
{
    assert(m_pFileEntries);
    wxFileName fln(filename.c_str());
    fln.Normalize(wxPATH_NORM_ALL & ~wxPATH_NORM_CASE);
#ifdef __WXMSW__
    wxPathFormat pathFormat = wxPATH_WIN;
#else
    wxPathFormat pathFormat = wxPATH_UNIX;
#endif // __WXMSW__

    const wxString& normFile = fln.GetFullPath(pathFormat);
    if (normFile.Len() <= 0 )
        return false;
    std::set<int> idList;

    m_pFileEntries->GetIdSet(normFile, idList);
    if (idList.empty())
        return false;
    return true;
}

/** @brief Get a filename id from a filename. Creates a new ID if the filename was not known yet.
 *
 * @param filename const wxString&
 * @return ClFileId
 *
 */
ClFileId ClFilenameDatabase::GetFilenameId(const wxString& filename) const
{
    assert(m_pFileEntries);
    wxFileName fln(filename.c_str());
    fln.Normalize(wxPATH_NORM_ALL & ~wxPATH_NORM_CASE);
#ifdef __WXMSW__
    wxPathFormat pathFormat = wxPATH_WIN;
#else
    wxPathFormat pathFormat = wxPATH_UNIX;
#endif // __WXMSW__
    const wxString& normFile = fln.GetFullPath(pathFormat);
    std::set<int> idList;

    m_pFileEntries->GetIdSet(normFile, idList);
    if (idList.empty())
    {
        wxString f = wxString(normFile.c_str());
        wxDateTime ts; // Timestamp updated when file was parsed into the token database.
        ClFilenameEntry entry(f,ts);
        return m_pFileEntries->Insert(f, entry);
    }
    return *idList.begin();
}

/** @brief Get the filename from its ID
 *
 * @param fId const ClFileId
 * @return wxString
 *
 */
wxString ClFilenameDatabase::GetFilename( const ClFileId fId) const
{
    assert(m_pFileEntries);

    if (!m_pFileEntries->HasValue(fId))
    {
        return wxEmptyString;
    }

    ClFilenameEntry entry = m_pFileEntries->GetValue(fId);
    const wxChar* val = entry.filename.c_str();
    if (val == nullptr)
    {
        return wxEmptyString;
    }

    return wxString(val);
}

/** @brief Get the timestamp from a filename
 *
 * @param fId const ClFileId
 * @return const wxDateTime
 *
 * @note Not a reference returned due to multithreading
 */
const wxDateTime ClFilenameDatabase::GetFilenameTimestamp( const ClFileId fId ) const
{
    assert(m_pFileEntries->HasValue(fId));

    ClFilenameEntry entry = m_pFileEntries->GetValue(fId);
    return entry.timestamp;
}

/** @brief Update the filename timestamp to indicate we have processed this file at this timestamp.
 *
 * @param fId const ClFileId
 * @param timestamp const wxDateTime&
 * @return void
 *
 */
void ClFilenameDatabase::UpdateFilenameTimestamp( const ClFileId fId, const wxDateTime& timestamp )
{
    assert(m_pFileEntries->HasValue(fId));

    ClFilenameEntry& entryRef = m_pFileEntries->GetValue(fId);
    entryRef.timestamp = timestamp;
}

/** @brief Constructor
 *
 * @param pIndexDB ClTokenIndexDatabase*
 *
 */
ClTokenDatabase::ClTokenDatabase(ClTokenIndexDatabase* pIndexDB) :
        m_pTokenIndexDB( pIndexDB ),
        m_pLocalTokenIndexDB( nullptr ),
        m_pTokens(new ClTreeMap<ClAbstractToken>()),
        m_pFileTokens(new ClTreeMap<ClTokenId>())
{
    if (!pIndexDB)
    {
        m_pLocalTokenIndexDB = new ClTokenIndexDatabase();
        m_pTokenIndexDB = m_pLocalTokenIndexDB;
    }
}

/** @brief Copy constructor
 *
 * @param other The other ClTokenDatabase
 *
 */
ClTokenDatabase::ClTokenDatabase( const ClTokenDatabase& other) :
    m_pTokenIndexDB(nullptr),
    m_pLocalTokenIndexDB(nullptr),
    m_pTokens(nullptr),
    m_pFileTokens(nullptr)
{
    CCLogger::Get()->DebugLog(wxT("Copying ClTokenDatabase"));
    if (other.m_pLocalTokenIndexDB)
    {
        m_pLocalTokenIndexDB = new ClTokenIndexDatabase(*other.m_pLocalTokenIndexDB);
        m_pTokenIndexDB = m_pLocalTokenIndexDB;
    }
    else
    {
        m_pTokenIndexDB = other.m_pTokenIndexDB;
    }
    m_pTokens = new ClTreeMap<ClAbstractToken>(*other.m_pTokens);
    m_pFileTokens = new ClTreeMap<int>(*other.m_pFileTokens);
}

/** @brief Destructor
 */
ClTokenDatabase::~ClTokenDatabase()
{
    delete m_pTokens;
    delete m_pFileTokens;
    delete m_pLocalTokenIndexDB;
}

/** @brief Swap 2 token databases
 *
 * @param first ClTokenDatabase&
 * @param second ClTokenDatabase&
 * @return void
 *
 */
void swap( ClTokenDatabase& first, ClTokenDatabase& second )
{
    using std::swap;

    swap(first.m_pTokenIndexDB, second.m_pTokenIndexDB);
    swap(first.m_pLocalTokenIndexDB, second.m_pLocalTokenIndexDB);
    swap(first.m_pTokens, second.m_pTokens);
    swap(first.m_pFileTokens, second.m_pFileTokens);
}

std::set<ClFileId> ClTokenIndexDatabase::LookupTokenFileList( const wxString& identifier, const wxString& USR, const ClTokenType typeMask ) const
{
    std::set<ClFileId> retList;
    std::set<int> idList;

    wxMutexLocker locker(m_Mutex);

    m_pIndexTokenMap->GetIdSet( identifier, idList );
    for (std::set<int>::const_iterator it = idList.begin(); it != idList.end(); ++it)
    {
        ClIndexToken& token = m_pIndexTokenMap->GetValue(*it);
        if (((token.tokenTypeMask&typeMask)==typeMask )&&((USR.Length() == 0)||(token.USR.Length() == 0)||(USR == token.USR)))
        {
            for (std::vector<ClIndexTokenLocation>::const_iterator it2 = token.locationList.begin(); it2 != token.locationList.end(); ++it2)
                retList.insert( it2->fileId );
        }
    }
    return retList;
}

std::set< std::pair<ClFileId, wxString> > ClTokenIndexDatabase::LookupTokenOverrides( const wxString& identifier, const wxString& USR, const ClTokenType typeMask ) const
{
    std::set<std::pair<ClFileId, wxString> > retList;
    std::set<int> idList;

    wxMutexLocker locker(m_Mutex);

    m_pIndexTokenMap->GetIdSet( identifier, idList );
    for (std::set<int>::const_iterator it = idList.begin(); it != idList.end(); ++it)
    {
        ClIndexToken& token = m_pIndexTokenMap->GetValue(*it);
        if ((token.tokenTypeMask&typeMask)==typeMask)
        {
            if (std::find( token.parentTokenList.begin(), token.parentTokenList.end(), std::make_pair(identifier,USR) ) != token.parentTokenList.end())
            {
                for (std::vector<ClIndexTokenLocation>::const_iterator it2 = token.locationList.begin(); it2 != token.locationList.end(); ++it2)
                    retList.insert( std::make_pair( it2->fileId, token.USR ) );
            }
        }
    }
    return retList;
}

void ClTokenIndexDatabase::UpdateToken( const wxString& identifier, const wxString& displayName, const ClFileId fileId, const wxString& USR, const ClTokenType tokType, const ClTokenRange& tokenRange, const std::vector< std::pair<wxString,wxString> >& overrideTokenList, const std::pair<wxString,wxString>& scope)
{
    std::set<int> idList;

    wxMutexLocker locker(m_Mutex);

    m_pIndexTokenMap->GetIdSet( identifier, idList );
    for (std::set<int>::const_iterator it = idList.begin(); it != idList.end(); ++it)
    {
        ClIndexToken& token = m_pIndexTokenMap->GetValue( *it );
        if ( token.USR == USR )
        {
            token.tokenTypeMask = static_cast<ClTokenType>(token.tokenTypeMask | tokType);
            ClIndexTokenLocation location(tokType, fileId, tokenRange);
            if (std::find(token.locationList.begin(), token.locationList.end(), location) == token.locationList.end())
                token.locationList.push_back( location );
            for (std::vector<std::pair<wxString,wxString> >::const_iterator usrIt = overrideTokenList.begin(); usrIt != overrideTokenList.end(); ++usrIt)
            {
                if (std::find( token.parentTokenList.begin(), token.parentTokenList.end(), *usrIt ) == token.parentTokenList.end())
                {
                    token.parentTokenList.push_back( *usrIt );
                }
            }
            wxString fid = wxString::Format( wxT("%d"), fileId );
            m_pFileTokens->Remove( fid, *it );
            m_pFileTokens->Insert( fid, *it );
            m_bModified = true;
            return;
        }
    }
    ClTokenId id = m_pIndexTokenMap->Insert( identifier, ClIndexToken(identifier, displayName, fileId, USR, tokType, tokenRange, overrideTokenList, scope) );
    wxString fid = wxString::Format( wxT("%d"), fileId );
    m_pFileTokens->Insert( fid, id );
    m_bModified = true;
}

/** @brief Removes all token references that refer to the specified file
 *
 * @param fileId const ClFileId
 * @return void
 *
 */
void ClTokenIndexDatabase::RemoveFileTokens(const ClFileId fileId)
{
    wxString key = wxString::Format(wxT("%d"), fileId);
    std::set<ClTokenId> tokenList;

    wxMutexLocker locker(m_Mutex);
    m_pFileTokens->GetIdSet(key, tokenList);
    for (std::set<ClTokenId>::iterator it = tokenList.begin(); it != tokenList.end(); ++it)
    {
        if (!m_pIndexTokenMap->HasValue( *it ))
            continue;
        ClIndexToken& tok = m_pIndexTokenMap->GetValue( m_pFileTokens->GetValue( *it ) );
        for ( std::vector< ClIndexTokenLocation >::iterator itLoc = tok.locationList.begin(); itLoc != tok.locationList.end(); ++itLoc )
        {
            if (itLoc->fileId == fileId)
            {
                itLoc = tok.locationList.erase( itLoc );
                if (itLoc == tok.locationList.end())
                    break;
            }
        }
    }
    m_pFileTokens->Remove(key);
}


bool ClTokenIndexDatabase::LookupTokenPosition(const wxString& identifier, const ClFileId fileId, const wxString& USR, const ClTokenType tokenTypeMask, ClTokenPosition& out_Position) const
{
    std::set<int> idList;
    wxMutexLocker locker(m_Mutex);

    m_pIndexTokenMap->GetIdSet( identifier, idList );
    for (std::set<int>::const_iterator it = idList.begin(); it != idList.end(); ++it)
    {
        ClIndexToken& token = m_pIndexTokenMap->GetValue( *it );
        if ((token.tokenTypeMask&tokenTypeMask)==tokenTypeMask)
        {
            if ((USR.Length() == 0)||(USR == token.USR))
            {
                for (std::vector<ClIndexTokenLocation>::const_iterator it = token.locationList.begin(); it != token.locationList.end(); ++it)
                {
                    if (((it->tokenType&tokenTypeMask)==tokenTypeMask)&&(it->fileId == fileId) )
                    {
                        out_Position = it->range.beginLocation;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool ClTokenIndexDatabase::LookupTokenPosition(const wxString& identifier, const ClFileId fileId, const wxString& USR, const ClTokenType tokenTypeMask, ClTokenRange& out_Range) const
{
    std::set<int> idList;

    wxMutexLocker locker(m_Mutex);

    m_pIndexTokenMap->GetIdSet( identifier, idList );
    for (std::set<int>::const_iterator it = idList.begin(); it != idList.end(); ++it)
    {
        ClIndexToken& token = m_pIndexTokenMap->GetValue( *it );
        if ((token.tokenTypeMask&tokenTypeMask)==tokenTypeMask)
        {
            if ((USR.Length() == 0)||(USR == token.USR))
            {
                for (std::vector<ClIndexTokenLocation>::const_iterator it = token.locationList.begin(); it != token.locationList.end(); ++it)
                {
                    if (((it->tokenType&tokenTypeMask)==tokenTypeMask)&&(it->fileId == fileId) )
                    {
                        out_Range = it->range;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool ClTokenIndexDatabase::LookupTokenDisplayName(const wxString& identifier, const wxString& USR, wxString& out_DisplayName) const
{
    std::set<int> idList;

    wxMutexLocker locker(m_Mutex);

    m_pIndexTokenMap->GetIdSet( identifier, idList );
    for (std::set<int>::const_iterator it = idList.begin(); it != idList.end(); ++it)
    {
        ClIndexToken& token = m_pIndexTokenMap->GetValue( *it );
        if ((USR.Length() == 0)||(USR == token.USR))
        {
            out_DisplayName = token.displayName;
            wxString parentScopeDisplayName;
            if (LookupTokenDisplayName( token.scope.first, token.scope.second, parentScopeDisplayName ))
            {
                out_DisplayName = parentScopeDisplayName + wxT("::")+out_DisplayName;
            }
            return true;
        }
    }
    return false;
}


/** @brief Get all tokens linked to a file ID
 *
 * @param fId const ClFileId
 * @return std::vector<ClTokenId>
 *
 */
void ClTokenIndexDatabase::GetFileTokens(const ClFileId fId, const int tokenTypeMask, std::vector<ClIndexToken>& out_tokens) const
{
    wxString key = wxString::Format(wxT("%d"), fId);
    std::set<int> tokenList;

    wxMutexLocker locker(m_Mutex);
    m_pFileTokens->GetIdSet(key, tokenList);
    CCLogger::Get()->DebugLog( F(wxT(" file %d tok: %d from total %d in %p"), (int)fId, (int)tokenList.size(), (int)m_pFileTokens->GetCount(), m_pFileTokens) );
    for (std::set<int>::const_iterator it = tokenList.begin(); it != tokenList.end(); ++it)
    {
        const ClIndexToken& tok = m_pIndexTokenMap->GetValue( m_pFileTokens->GetValue( *it ) );
        if ( (tokenTypeMask==0)||(tok.tokenTypeMask&tokenTypeMask) )
        {
            //CCLogger::Get()->DebugLog( F(wxT("Adding token ")+tok.identifier+wxT(" with mask %x USR=")+tok.USR, (int)tok.tokenTypeMask) );
            out_tokens.push_back(tok);
        }
    }
}

void ClTokenIndexDatabase::AddToken( const wxString& identifier, const ClIndexToken& token)
{
    if (identifier.Length() > 0)
    {
        wxMutexLocker locker(m_Mutex);

        ClTokenId id = m_pIndexTokenMap->Insert( identifier, token );
        for (std::vector<ClIndexTokenLocation>::const_iterator it = token.locationList.begin(); it != token.locationList.end(); ++it)
        {
            wxString fid = wxString::Format( wxT("%d"), it->fileId );
            m_pFileTokens->Remove( fid, id );
            m_pFileTokens->Insert( fid, id );
//            CCLogger::Get()->DebugLog( F(wxT("Mapped token %d to file %d tot %d in %p"), (int)id, (int)it->fileId, (int)m_pFileTokens->GetCount(), m_pFileTokens) );
        }
        m_bModified = true;
    }
}



/** @brief Read a tokendatabase from disk
 *
 * @param tokenDatabase The tokendatabase to read into
 * @param in The wxInputStream to read from
 * @return true if the operation succeeded, false otherwise
 *
 */
bool ClTokenIndexDatabase::ReadIn( ClTokenIndexDatabase& tokenDatabase, wxInputStream& in )
{
    char buffer[5];
    in.Read( buffer, 4 );
    buffer[4] = '\0';
    int version = 0xff;
    if (!ReadInt(in, version))
        return false;
    int i = 0;
    if (version != 0x05)
    {
        CCLogger::Get()->DebugLog(F(wxT("Wrong version of token database: %d"), version));
        return false;
    }
    int major = 0;
    int minor = 0;
    if (!ReadInt(in, major))
        return false;
    if (!ReadInt(in, minor))
        return false;
    if (major != CINDEX_VERSION_MAJOR)
    {
        CCLogger::Get()->Log( wxT("Major version mismatch between Clang indexdb and libclang") );
        return false;
    }
    if (minor != CINDEX_VERSION_MINOR)
    {
        CCLogger::Get()->Log( wxT("Minor version mismatch between Clang indexdb and libclang") );
        return false;
    }
    tokenDatabase.Clear();
    int read_count = 0;

    wxMutexLocker locker(tokenDatabase.m_Mutex);
    while (in.CanRead())
    {
        int packetType = 0;
        if (!ReadInt(in, packetType))
        {
            CCLogger::Get()->DebugLog(wxT("TokenIndexDatabase: Could not read packet type"));
            return false;
        }
        switch (packetType)
        {
        case 0:
            return true;
        case ClTokenPacketType_filenames:
            if (!ClFilenameDatabase::ReadIn(tokenDatabase.m_FileDB, in))
            {
                CCLogger::Get()->DebugLog( wxT("Failed to read filename database") );
                return false;
            }
            break;
        case ClTokenPacketType_tokens:
            int packetCount = 0;
            if (!ReadInt(in, packetCount))
                return false;
            for (i = 0; i < packetCount; ++i)
            {
                wxString identifier;
                if (!ReadString( in, identifier ))
                    return false;
                ClIndexToken token;
                int tokenCount = 0;
                if (!ReadInt(in, tokenCount))
                    return false;
                for (int j=0; j<tokenCount; ++j)
                {
                    if (!ClIndexToken::ReadIn( token, in ))
                        return false;
                    tokenDatabase.AddToken( identifier, token );
                    read_count++;
                }
            }
            break;
        }
    }
    return true;
}

/** @brief Write the database to an output stream
 *
 * @param tokenDatabase The database to write
 * @param out Were to write the database to
 * @return true if the operation was successful, false otherwise
 *
 */
bool ClTokenIndexDatabase::WriteOut( const ClTokenIndexDatabase& tokenDatabase, wxOutputStream& out )
{
    int cnt;
    out.Write("ClDb", 4); // Magic number
    WriteInt(out, 0x5); // Version number
    WriteInt(out, CINDEX_VERSION_MAJOR);
    WriteInt(out, CINDEX_VERSION_MINOR);

    WriteInt(out, ClTokenPacketType_filenames);
    wxMutexLocker locker(tokenDatabase.m_Mutex);
    if (!ClFilenameDatabase::WriteOut(tokenDatabase.m_FileDB, out))
        return false;

    WriteInt(out, ClTokenPacketType_tokens);
    std::set<wxString> tokens = tokenDatabase.m_pIndexTokenMap->GetKeySet();
    cnt = tokens.size();
    WriteInt(out, cnt);

    uint32_t written_count = 0;
    for (std::set<wxString>::const_iterator it = tokens.begin(); it != tokens.end(); ++it)
    {
        WriteString( out, (const char*)it->utf8_str() );

        std::set<int> tokenIds;
        tokenDatabase.m_pIndexTokenMap->GetIdSet( *it, tokenIds );
        cnt = tokenIds.size();
        WriteInt(out, cnt);
        for (std::set<int>::const_iterator it = tokenIds.begin(); it != tokenIds.end(); ++it)
        {
            ClIndexToken tok = tokenDatabase.m_pIndexTokenMap->GetValue(*it);
            if (!ClIndexToken::WriteOut(tok, out))
                return false;
            written_count++;
        }
    }
    WriteInt(out, 0);
    return true;
}

/** @brief Clear the token database
 *
 */
void ClTokenDatabase::Clear()
{
    delete m_pTokens;
    m_pTokens = new ClTreeMap<ClAbstractToken>(),
    delete m_pFileTokens;
    m_pFileTokens = new ClTreeMap<int>();
}

/** @brief Get an ID for a filename. Creates a new ID if the filename was not known yet.
 *
 * @param filename Full path to the filename
 * @return ClFileId
 *
 */
ClFileId ClTokenDatabase::GetFilenameId(const wxString& filename) const
{
    return m_pTokenIndexDB->GetFilenameId(filename);
}

/** @brief Get the filename that is known with an ID
 *
 * @param fId The file ID
 * @return Full path to the file
 *
 */
wxString ClTokenDatabase::GetFilename(ClFileId fId) const
{
    return m_pTokenIndexDB->GetFilename( fId );
}

/** @brief Get the timestamp of the filename entry when the file was last parsed
 *
 * @param fId The file id
 * @return wxDateTime object which represents a timestamp
 *
 */
wxDateTime ClTokenDatabase::GetFilenameTimestamp( const ClFileId fId ) const
{
    return m_pTokenIndexDB->GetFilenameTimestamp(fId);
}

/** @brief Insert or update a token into the token database
 *
 * @param token The token to insert/update
 * @return ClTokenId of the updated token or if the token allready existed the newly inserted token
 *
 */
ClTokenId ClTokenDatabase::InsertToken( const ClAbstractToken& token )
{
    ClTokenId tId = GetTokenId(token.identifier, token.fileId, token.tokenType, token.tokenHash);
    if (tId == wxNOT_FOUND)
    {
        tId = m_pTokens->Insert(wxString(token.identifier), token);
        wxString filen = wxString::Format(wxT("%d"), token.fileId);
        m_pFileTokens->Insert(filen, tId);
    }
    return tId;
}

/** @brief Find a token ID by value
 *
 * @param identifier const wxString&
 * @param fileId ClFileId
 * @param tokenType ClTokenType
 * @param USR wxString
 * @return ClTokenId
 *
 */
ClTokenId ClTokenDatabase::GetTokenId( const wxString& identifier, const ClFileId fileId, const ClTokenType tokenType, const int tokenHash ) const
{
    std::set<int> ids;
    m_pTokens->GetIdSet(identifier, ids);
    for (std::set<int>::const_iterator itr = ids.begin();
            itr != ids.end(); ++itr)
    {
        if (m_pTokens->HasValue(*itr))
        {
            ClAbstractToken tok = m_pTokens->GetValue(*itr);
            if (   (tok.tokenHash == tokenHash)
                && ((tok.tokenType == tokenType) || (tokenType == ClTokenType_Unknown))
                && ((tok.fileId == fileId) || (fileId == wxNOT_FOUND)) )
            {

                return *itr;
            }
        }
    }
    return wxNOT_FOUND;
}

/** @brief Get a token with it's ID
 *
 * @param tId const ClTokenId
 * @return ClAbstractToken
 *
 * @note No reference returned for multi-threading reasons
 */
ClAbstractToken ClTokenDatabase::GetToken(const ClTokenId tId) const
{
    assert(m_pTokens->HasValue(tId));
    return m_pTokens->GetValue(tId);
}

/** @brief Find the token IDs of all matches of an identifier
 *
 * @param identifier const wxString&
 * @return std::vector<ClTokenId>
 *
 */
void ClTokenDatabase::GetTokenMatches(const wxString& identifier, std::set<ClTokenId>& out_List) const
{
    m_pTokens->GetIdSet(identifier, out_List);
}

/** @brief Get all tokens linked to a file ID
 *
 * @param fId const ClFileId
 * @return std::vector<ClTokenId>
 *
 */
void ClTokenDatabase::GetFileTokens(const ClFileId fId, std::set<ClTokenId>& out_tokens) const
{
    wxString key = wxString::Format(wxT("%d"), fId);
    m_pFileTokens->GetIdSet(key, out_tokens);
}

void ClTokenDatabase::GetTokenScopes(const ClFileId fileId, const unsigned TokenTypeMask, std::vector<ClTokenScope>& out_Scopes) const
{
    std::set<ClTokenId> tokenIds;
    wxString key = wxString::Format(wxT("%d"), fileId);
    m_pFileTokens->GetIdSet(key, tokenIds);
    for (std::set<ClTokenId>::const_iterator it = tokenIds.begin(); it != tokenIds.end(); ++it)
    {
        ClAbstractToken token = GetToken( *it );
        if (token.tokenType&TokenTypeMask)
        {
            wxString scopeName = token.scope.first;
            out_Scopes.push_back( ClTokenScope(token.displayName, scopeName, token.range) );
        }
    }
}


/** @brief Shrink the database to reclaim some memory
 *
 * @return void
 *
 */
void ClTokenDatabase::Shrink()
{
    m_pTokens->Shrink();
    m_pFileTokens->Shrink();
}

/** @brief Update a token that was previously cleared
 *
 * @param freeTokenId const ClTokenId
 * @param token const ClAbstractToken&
 *
 */
void ClTokenDatabase::UpdateToken( const ClTokenId freeTokenId, const ClAbstractToken& token )
{
    ClAbstractToken tokenRef = m_pTokens->GetValue(freeTokenId);
    m_pTokens->RemoveIdKey(tokenRef.identifier, freeTokenId);
    assert((tokenRef.fileId == wxNOT_FOUND) && "Only an unused token can be updated");
    tokenRef.fileId = token.fileId;
    tokenRef.identifier = token.identifier;
    tokenRef.range = token.range;
    tokenRef.tokenHash = token.tokenHash;
    tokenRef.tokenType = token.tokenType;
    wxString filen = wxString::Format(wxT("%d"), token.fileId);
    m_pFileTokens->Insert(filen, freeTokenId);
}

/** @brief Remove a token from the token database
 *
 * @param tokenId const ClTokenId
 * @return void
 *
 * This will not remove the token, it will only clear it in memory and will later on be reused.
 */
void ClTokenDatabase::RemoveToken( const ClTokenId tokenId )
{
    ClAbstractToken oldToken = GetToken(tokenId);
    wxString key = wxString::Format(wxT("%d"), oldToken.fileId);
    m_pFileTokens->Remove(key, tokenId);
    ClAbstractToken t;
    // We just invalidate it here. Real removal is rather complex
    UpdateToken(tokenId, t);
}

unsigned long ClTokenDatabase::GetTokenCount()
{
    return m_pTokens->GetCount();
}

void ClTokenDatabase::StoreIndexes() const
{
    ClTokenId id;
    std::set<wxString> keySet = m_pFileTokens->GetKeySet();
    for (std::set<wxString>::const_iterator it = keySet.begin(); it != keySet.end(); ++it)
    {
        ClFileId fId = wxAtoi( *it );
        m_pTokenIndexDB->RemoveFileTokens( fId );
    }
    //uint32_t cnt = m_pTokenIndexDB->GetTokenCount();
    for (id=0; id < m_pTokens->GetCount(); ++id)
    {
        ClAbstractToken& token = m_pTokens->GetValue( id );
        m_pTokenIndexDB->UpdateToken( token.identifier, token.displayName, token.fileId, token.USR, token.tokenType, token.range, token.parentTokenList, token.scope );
    }
    //uint32_t cnt2 = m_pTokenIndexDB->GetTokenCount();
    //CCLogger::Get()->DebugLog( F(wxT("StoreIndexes: Stored %d tokens. %d tokens extra. Total: %d (merged) IndexDb: %p"), (int)m_pTokens->GetCount(), (int)cnt2 - cnt, cnt2, m_pTokenIndexDB) );
}

bool ClTokenDatabase::LookupTokenDefinition( const ClFileId fileId, const wxString& identifier, const wxString& usr, ClTokenPosition& out_Position) const
{
    std::set<ClTokenId> tokenIdList;
    GetTokenMatches(identifier, tokenIdList);

    for (std::set<ClTokenId>::const_iterator it = tokenIdList.begin(); it != tokenIdList.end(); ++it)
    {
        ClAbstractToken tok = GetToken( *it );
        if (tok.fileId == fileId)
        {
            if ( (tok.tokenType&ClTokenType_DefGroup) == ClTokenType_DefGroup ) // We only want token definitions
            {
                if( (usr.Length() == 0)||(tok.USR == usr))
                {
                    out_Position = tok.range.beginLocation;
                    return true;
                }
            }
        }
    }
    return false;
}

bool ClTokenDatabase::LookupTokenDefinition( const ClFileId fileId, const wxString& identifier, const wxString& usr, ClTokenRange& out_Range) const
{
    std::set<ClTokenId> tokenIdList;
    GetTokenMatches(identifier, tokenIdList);

    for (std::set<ClTokenId>::const_iterator it = tokenIdList.begin(); it != tokenIdList.end(); ++it)
    {
        ClAbstractToken tok = GetToken( *it );
        if (tok.fileId == fileId)
        {
            if ( (tok.tokenType&ClTokenType_DefGroup) == ClTokenType_DefGroup ) // We only want token definitions
            {
                if( (usr.Length() == 0)||(tok.USR == usr))
                {
                    out_Range = tok.range;
                    return true;
                }
            }
        }
    }
    return false;
}
