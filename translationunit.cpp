/*
 * Wrapper class around CXTranslationUnit
 */

#include <sdk.h>
#include <iostream>
#include "translationunit.h"

#ifndef CB_PRECOMP
#include <algorithm>
#endif // CB_PRECOMP

#include "tokendatabase.h"
#include "cclogger.h"

#if 0
class ClangVisitorContext
{
public:
    ClangVisitorContext(ClTranslationUnit* pTranslationUnit) :
        m_pTranslationUnit(pTranslationUnit)
    { }
    std::deque<wxString> m_ScopeStack;
    std::deque<CXCursor> m_CursorSt
};
#endif

struct ClangVisitorContext
{
    ClangVisitorContext(ClTokenDatabase* pDatabase)
    {
        database = pDatabase;
        tokenCount = 0;
    }
    ClTokenDatabase* database;
    unsigned long long tokenCount;
    ClFunctionScopeMap functionScopes;
};

static void ClInclusionVisitor(CXFile included_file, CXSourceLocation* inclusion_stack,
                               unsigned include_len, CXClientData client_data);

static CXChildVisitResult ClAST_Visitor(CXCursor cursor, CXCursor parent, CXClientData client_data);

ClTranslationUnit::ClTranslationUnit(const ClTranslUnitId id, CXIndex clIndex) :
    m_Id(id),
    m_FileId(-1),
    m_ClIndex(clIndex),
    m_ClTranslUnit(nullptr),
    m_LastCC(nullptr),
    m_LastPos(-1, -1),
    m_Occupied(false),
    m_LastParsed(wxDateTime::Now())
{
}
ClTranslationUnit::ClTranslationUnit(const ClTranslUnitId id) :
    m_Id(id),
    m_FileId(-1),
    m_ClIndex(nullptr),
    m_ClTranslUnit(nullptr),
    m_LastCC(nullptr),
    m_LastPos(-1, -1),
    m_Occupied(true),
    m_LastParsed(wxDateTime::Now())
{
}


#if __cplusplus >= 201103L
ClTranslationUnit::ClTranslationUnit(ClTranslationUnit&& other) :
    m_Id(other.m_Id),
    m_FileId(other.m_FileId),
    m_Files(std::move(other.m_Files)),
    m_ClIndex(other.m_ClIndex),
    m_ClTranslUnit(other.m_ClTranslUnit),
    m_LastCC(nullptr),
    m_LastPos(-1, -1)
{
    other.m_ClTranslUnit = nullptr;
}
#else
ClTranslationUnit::ClTranslationUnit(const ClTranslationUnit& other) :
    m_Id(other.m_Id),
    m_FileId( other.m_FileId ),
    m_ClIndex(other.m_ClIndex),
    m_ClTranslUnit(other.m_ClTranslUnit),
    m_LastCC(nullptr),
    m_LastPos(-1, -1)
{
    m_Files.swap(const_cast<ClTranslationUnit&>(other).m_Files);
    const_cast<ClTranslationUnit&>(other).m_ClTranslUnit = nullptr;
}
#endif

ClTranslationUnit::~ClTranslationUnit()
{
    if (m_LastCC)
        clang_disposeCodeCompleteResults(m_LastCC);
    if (m_ClTranslUnit)
    {
        clang_disposeTranslationUnit(m_ClTranslUnit);
    }
}

std::ostream& operator << (std::ostream& str, const std::vector<ClFileId> files)
{
    str<<"[ ";
    for ( std::vector<ClFileId>::const_iterator it = files.begin(); it != files.end(); ++it )
    {
        str<<*it<<", ";
    }
    str<<"]";
    return str;
}

CXCodeCompleteResults* ClTranslationUnit::CodeCompleteAt(const wxString& complete_filename, const ClTokenPosition& complete_location,
                                                         struct CXUnsavedFile* unsaved_files, unsigned num_unsaved_files )
{
    if (m_ClTranslUnit == nullptr)
    {
        return nullptr;
    }
    //if (m_LastPos.Equals(complete_location.line, complete_location.column)&&(m_LastCC)&&m_LastCC->NumResults)
    //{
    //    fprintf(stdout,"%s: Returning last CC %d,%d (%d)\n", __PRETTY_FUNCTION__, (int)complete_location.line, (int)complete_location.column,  m_LastCC->NumResults);
    //    return m_LastCC;
    //}
    if (m_LastCC)
        clang_disposeCodeCompleteResults(m_LastCC);
    m_LastCC = clang_codeCompleteAt(m_ClTranslUnit, (const char*)complete_filename.ToUTF8(), complete_location.line, complete_location.column,
                                    unsaved_files, num_unsaved_files,
                                    clang_defaultCodeCompleteOptions()
                                    | CXCodeComplete_IncludeCodePatterns
                                    | CXCodeComplete_IncludeBriefComments);
    m_LastPos.Set(complete_location.line, complete_location.column);
    if (m_LastCC)
    {
        unsigned numDiag = clang_codeCompleteGetNumDiagnostics(m_LastCC);
        //unsigned int IsIncomplete = 0;
        //CXCursorKind kind = clang_codeCompleteGetContainerKind(m_LastCC, &IsIncomplete );
        unsigned int diagIdx = 0;
        std::vector<ClDiagnostic> diaglist;
        for (diagIdx=0; diagIdx < numDiag; ++diagIdx)
        {
            CXDiagnostic diag = clang_codeCompleteGetDiagnostic(m_LastCC, diagIdx);
            ExpandDiagnostic(diag, complete_filename, diaglist);
        }
    }

    return m_LastCC;
}

const CXCompletionResult* ClTranslationUnit::GetCCResult(unsigned index)
{
    if (m_LastCC && index < m_LastCC->NumResults)
        return m_LastCC->Results + index;
    return nullptr;
}

CXCursor ClTranslationUnit::GetTokenAt(const wxString& filename, const ClTokenPosition& location)
{
    if (m_ClTranslUnit == nullptr)
    {
        return clang_getNullCursor();
    }
    return clang_getCursor(m_ClTranslUnit, clang_getLocation(m_ClTranslUnit, GetFileHandle(filename), location.line, location.column));
}

/**
 * Parses the supplied file and unsaved files
 */
void ClTranslationUnit::Parse(const wxString& filename, ClFileId fileId, const std::vector<const char*>& args, const std::map<wxString, wxString>& unsavedFiles)
{
    CCLogger::Get()->DebugLog(F(_T("ClTranslationUnit::Parse %s id=%d"), filename.c_str(), (int)m_Id));

    if (m_LastCC)
    {
        clang_disposeCodeCompleteResults(m_LastCC);
        m_LastCC = nullptr;
    }
    if (m_ClTranslUnit)
    {
        clang_disposeTranslationUnit(m_ClTranslUnit);
        m_ClTranslUnit = nullptr;
    }

    // TODO: check and handle error conditions
    std::vector<CXUnsavedFile> clUnsavedFiles;
    std::vector<wxCharBuffer> clFileBuffer;
    for (std::map<wxString, wxString>::const_iterator fileIt = unsavedFiles.begin();
            fileIt != unsavedFiles.end(); ++fileIt)
    {
        CXUnsavedFile unit;
        clFileBuffer.push_back(fileIt->first.ToUTF8());
        unit.Filename = clFileBuffer.back().data();
        clFileBuffer.push_back(fileIt->second.ToUTF8());
        unit.Contents = clFileBuffer.back().data();
#if wxCHECK_VERSION(2, 9, 4)
        unit.Length   = clFileBuffer.back().length();
#else
        unit.Length   = strlen(unit.Contents); // extra work needed because wxString::Length() treats multibyte character length as '1'
#endif
        clUnsavedFiles.push_back(unit);
    }
    m_FileId = fileId;
    m_Files.push_back( fileId );
    m_LastParsed = wxDateTime::Now();
    m_FunctionScopes.clear();

    if (filename.length() != 0)
    {
        m_ClTranslUnit = clang_parseTranslationUnit(m_ClIndex, filename.ToUTF8().data(), args.empty() ? nullptr : &args[0], args.size(),
                         //clUnsavedFiles.empty() ? nullptr : &clUnsavedFiles[0], clUnsavedFiles.size(),
                         nullptr, 0,
                         clang_defaultEditingTranslationUnitOptions()
                         | CXTranslationUnit_CacheCompletionResults
                         | CXTranslationUnit_IncludeBriefCommentsInCodeCompletion
                         | CXTranslationUnit_DetailedPreprocessingRecord
                         | CXTranslationUnit_PrecompiledPreamble
                         //CXTranslationUnit_CacheCompletionResults |
                         //    CXTranslationUnit_Incomplete | CXTranslationUnit_DetailedPreprocessingRecord |
                         //    CXTranslationUnit_CXXChainedPCH
                                                   );
        if (m_ClTranslUnit == nullptr)
        {
            return;
        }
        //Reparse(0, nullptr); // seems to improve performance for some reason?
        int ret = clang_reparseTranslationUnit(m_ClTranslUnit, clUnsavedFiles.size(),
                                               clUnsavedFiles.empty() ? nullptr : &clUnsavedFiles[0],
                                               clang_defaultReparseOptions(m_ClTranslUnit) );
        if (ret != 0)
        {
            CCLogger::Get()->Log(_T("ReparseTranslationUnit failed"));
            // clang spec specifies that the only valid operation on the translation unit after a failure is to dispose the TU
            clang_disposeTranslationUnit(m_ClTranslUnit);
            m_ClTranslUnit = nullptr;
            return;
        }
    }
}

void ClTranslationUnit::Reparse( const std::map<wxString, wxString>& unsavedFiles)
{
    CCLogger::Get()->DebugLog(F(_T("ClTranslationUnit::Reparse id=%d"), (int)m_Id));

    if (m_ClTranslUnit == nullptr)
    {
        return;
    }
    std::vector<CXUnsavedFile> clUnsavedFiles;
    std::vector<wxCharBuffer> clFileBuffer;
    for (std::map<wxString, wxString>::const_iterator fileIt = unsavedFiles.begin();
         fileIt != unsavedFiles.end(); ++fileIt)
    {
        CXUnsavedFile unit;
        clFileBuffer.push_back(fileIt->first.ToUTF8());
        unit.Filename = clFileBuffer.back().data();
        clFileBuffer.push_back(fileIt->second.ToUTF8());
        unit.Contents = clFileBuffer.back().data();
#if wxCHECK_VERSION(2, 9, 4)
        unit.Length   = clFileBuffer.back().length();
#else
        unit.Length   = strlen(unit.Contents); // extra work needed because wxString::Length() treats multibyte character length as '1'
#endif
        clUnsavedFiles.push_back(unit);
    }


    // TODO: check and handle error conditions
    int ret = clang_reparseTranslationUnit(m_ClTranslUnit, clUnsavedFiles.size(),
                                           clUnsavedFiles.empty() ? nullptr : &clUnsavedFiles[0],
                                           clang_defaultReparseOptions(m_ClTranslUnit)
                                           //CXTranslationUnit_CacheCompletionResults | CXTranslationUnit_PrecompiledPreamble |
                                           //CXTranslationUnit_Incomplete | CXTranslationUnit_DetailedPreprocessingRecord |
                                           //CXTranslationUnit_CXXChainedPCH
                                          );
    if (ret != 0)
    {
        //assert(false&&"clang_reparseTranslationUnit should succeed");
        CCLogger::Get()->Log(_T("ReparseTranslationUnit failed"));

        // The only thing we can do according to Clang documentation is dispose it...
        clang_disposeTranslationUnit(m_ClTranslUnit);
        m_ClTranslUnit = nullptr;
        return;
    }
    if (m_LastCC)
    {
        clang_disposeCodeCompleteResults(m_LastCC);
        m_LastCC = nullptr;
    }
    m_LastParsed = wxDateTime::Now();

    CCLogger::Get()->DebugLog(F(_T("ClTranslationUnit::Reparse id=%d finished"), (int)m_Id));
}

void ClTranslationUnit::ProcessAllTokens(ClTokenDatabase& database, std::vector<ClFileId>& out_includeFileList, ClFunctionScopeMap& out_functionScopes) const
{
    if (m_ClTranslUnit == nullptr)
        return;
    std::pair<std::vector<ClFileId>*, ClTokenDatabase*> visitorData = std::make_pair(&out_includeFileList, &database);
    clang_getInclusions(m_ClTranslUnit, ClInclusionVisitor, &visitorData);
    //m_Files.reserve(1024);
    //m_Files.push_back(m_FileId);
    //std::sort(m_Files.begin(), m_Files.end());
    //std::unique(m_Files.begin(), m_Files.end());
    out_includeFileList.push_back( m_FileId );
    std::sort(out_includeFileList.begin(), out_includeFileList.end());
    std::unique(out_includeFileList.begin(), out_includeFileList.end());
#if __cplusplus >= 201103L
    //m_Files.shrink_to_fit();
    fileList.schrink_to_fit();
#else
    //std::vector<ClFileId>(m_Files).swap(m_Files);
    std::vector<ClFileId>(out_includeFileList).swap(out_includeFileList);
#endif
    struct ClangVisitorContext ctx(&database);
    //unsigned rc =
    clang_visitChildren(clang_getTranslationUnitCursor(m_ClTranslUnit), ClAST_Visitor, &ctx);
    CCLogger::Get()->DebugLog(F(_T("ClTranslationUnit::UpdateTokenDatabase %d finished: %d tokens processed, %d function scopes"), (int)m_Id, (int)ctx.tokenCount, (int)ctx.functionScopes.size()));
    out_functionScopes = ctx.functionScopes;
}

void ClTranslationUnit::GetDiagnostics(const wxString& filename,  std::vector<ClDiagnostic>& diagnostics)
{
    if (m_ClTranslUnit == nullptr)
    {
        return;
    }
    CXDiagnosticSet diagSet = clang_getDiagnosticSetFromTU(m_ClTranslUnit);
    ExpandDiagnosticSet(diagSet, filename, diagnostics);
    clang_disposeDiagnosticSet(diagSet);
}

CXFile ClTranslationUnit::GetFileHandle(const wxString& filename) const
{
    return clang_getFile(m_ClTranslUnit, filename.ToUTF8().data());
}

static void RangeToColumns(CXSourceRange range, unsigned& rgStart, unsigned& rgEnd)
{
    CXSourceLocation rgLoc = clang_getRangeStart(range);
    clang_getSpellingLocation(rgLoc, nullptr, nullptr, &rgStart, nullptr);
    rgLoc = clang_getRangeEnd(range);
    clang_getSpellingLocation(rgLoc, nullptr, nullptr, &rgEnd, nullptr);
}

/** @brief Expand the diagnostics into the supplied list. This appends the diagnostics to the passed list.
 *
 * @param diag CXDiagnostic
 * @param filename const wxString&
 * @param inout_diagnostics std::vector<ClDiagnostic>&
 * @return void
 *
 */
void ClTranslationUnit::ExpandDiagnostic( CXDiagnostic diag, const wxString& filename, std::vector<ClDiagnostic>& inout_diagnostics )
{
    if (diag == nullptr)
        return;
    CXSourceLocation loc = clang_getDiagnosticLocation(diag);
    if (clang_equalLocations(loc, clang_getNullLocation()))
        return;
    switch (clang_getDiagnosticSeverity(diag))
    {
    case CXDiagnostic_Ignored:
    case CXDiagnostic_Note:
        return;
    default:
        break;
    }
    unsigned line;
    unsigned column;
    CXFile file;
    clang_getSpellingLocation(loc, &file, &line, &column, nullptr);
    CXString str = clang_getFileName(file);
    wxString flName = wxString::FromUTF8(clang_getCString(str));
    clang_disposeString(str);

    if (flName == filename)
    {
        size_t numRnges = clang_getDiagnosticNumRanges(diag);
        unsigned rgStart = 0;
        unsigned rgEnd = 0;
        for (size_t j = 0; j < numRnges; ++j) // often no range data (clang bug?)
        {
            RangeToColumns(clang_getDiagnosticRange(diag, j), rgStart, rgEnd);
            if (rgStart != rgEnd)
                break;
        }
        if (rgStart == rgEnd) // check if there is FixIt data for the range
        {
            numRnges = clang_getDiagnosticNumFixIts(diag);
            for (size_t j = 0; j < numRnges; ++j)
            {
                CXSourceRange range;
                clang_getDiagnosticFixIt(diag, j, &range);
                RangeToColumns(range, rgStart, rgEnd);
                if (rgStart != rgEnd)
                    break;
            }
        }
        if (rgEnd == 0) // still no range -> use the range of the current token
        {
            CXCursor token = clang_getCursor(m_ClTranslUnit, loc);
            RangeToColumns(clang_getCursorExtent(token), rgStart, rgEnd);
        }
        if (rgEnd < column || rgStart > column) // out of bounds?
            rgStart = rgEnd = column;
        str = clang_formatDiagnostic(diag, 0);
        wxString diagText = wxString::FromUTF8(clang_getCString(str));
        clang_disposeString(str);
        if (diagText.StartsWith(wxT("warning: ")) )
        {
            diagText = diagText.Right( diagText.Length() - 9 );
        }
        else if (diagText.StartsWith(wxT("error: ")) )
        {
            diagText = diagText.Right( diagText.Length() - 7 );
        }
        ClDiagnosticSeverity sev = sWarning;
        switch ( clang_getDiagnosticSeverity(diag))
        {
        case CXDiagnostic_Error:
        case CXDiagnostic_Fatal:
            sev = sError;
            break;
        case CXDiagnostic_Note:
            sev = sNote;
            break;
        case CXDiagnostic_Warning:
        case CXDiagnostic_Ignored:
            sev = sWarning;
            break;
        }
        std::vector<ClDiagnosticFixit> fixitList;
        unsigned numFixIts = clang_getDiagnosticNumFixIts( diag );
        for (unsigned fixIdx = 0; fixIdx < numFixIts; ++fixIdx)
        {
            CXSourceRange sourceRange;
            str = clang_getDiagnosticFixIt( diag, fixIdx, &sourceRange );
            unsigned fixitStart = rgStart;
            unsigned fixitEnd = rgEnd;
            RangeToColumns(sourceRange, fixitStart, fixitEnd);
            wxString text = wxString::FromUTF8( clang_getCString(str) );
            clang_disposeString(str);
            fixitList.push_back( ClDiagnosticFixit(text, fixitStart, fixitEnd) );
        }
        inout_diagnostics.push_back(ClDiagnostic( line, rgStart, rgEnd, sev, flName, diagText, fixitList ));
    }
}

/** @brief Expand a Clang CXDiagnosticSet into our clanglib vector representation
 *
 * @param diagSet The CXDiagnosticSet
 * @param filename Filename that this diagnostic targets
 * @param diagnostics[out] The returned diagnostics vector
 * @return void
 *
 */
void ClTranslationUnit::ExpandDiagnosticSet(CXDiagnosticSet diagSet, const wxString& filename, std::vector<ClDiagnostic>& diagnostics)
{
    size_t numDiags = clang_getNumDiagnosticsInSet(diagSet);
    for (size_t i = 0; i < numDiags; ++i)
    {
        CXDiagnostic diag = clang_getDiagnosticInSet(diagSet, i);
        ExpandDiagnostic(diag, filename, diagnostics);
        //ExpandDiagnosticSet(clang_getChildDiagnostics(diag), diagnostics);
        clang_disposeDiagnostic(diag);
    }
}

void ClTranslationUnit::UpdateFunctionScopes( const ClFileId fileId, const ClFunctionScopeList &functionScopes )
{
    m_FunctionScopes.erase(fileId);
    m_FunctionScopes.insert(std::make_pair(fileId, functionScopes));
}

/** @brief Calculate a hash from a Clang token
 *
 * @param token CXCompletionString
 * @param identifier wxString&
 * @return unsigned
 *
 */
unsigned HashToken(CXCompletionString token, wxString& identifier)
{
    unsigned hVal = 2166136261u;
    size_t upperBound = clang_getNumCompletionChunks(token);
    for (size_t i = 0; i < upperBound; ++i)
    {
        CXString str = clang_getCompletionChunkText(token, i);
        const char* pCh = clang_getCString(str);
        if (clang_getCompletionChunkKind(token, i) == CXCompletionChunk_TypedText)
            identifier = wxString::FromUTF8(*pCh =='~' ? pCh + 1 : pCh);
        for (; *pCh; ++pCh)
        {
            hVal ^= *pCh;
            hVal *= 16777619u;
        }
        clang_disposeString(str);
    }
    return hVal;
}

/** @brief Static function used in the Clang AST visitor functions
 *
 * @param inclusion_stack
 * @return void ClInclusionVisitor(CXFile included_file, CXSourceLocation*
 *
 */
static void ClInclusionVisitor(CXFile included_file, CXSourceLocation* WXUNUSED(inclusion_stack),
                               unsigned WXUNUSED(include_len), CXClientData client_data)
{
    CXString filename = clang_getFileName(included_file);
    wxFileName inclFile(wxString::FromUTF8(clang_getCString(filename)));
    if (inclFile.MakeAbsolute())
    {
        std::pair<std::vector<ClFileId>*, ClTokenDatabase*>* data = static_cast<std::pair<std::vector<ClFileId>*, ClTokenDatabase*>*>(client_data);
        ClFileId fileId = data->second->GetFilenameId( inclFile.GetFullPath() );
        data->first->push_back( fileId );
        //clTranslUnit->first->AddInclude(clTranslUnit->second->GetFilenameId(inclFile.GetFullPath()));
    }
    clang_disposeString(filename);
}

/** @brief Static function used in the Clang AST visitor functions
 *
 * @param parent
 * @return CXChildVisitResult ClAST_Visitor(CXCursor cursor, CXCursor
 *
 */
static CXChildVisitResult ClAST_Visitor(CXCursor cursor, CXCursor WXUNUSED(parent), CXClientData client_data)
{
    ClTokenType typ = ClTokenType_Unknown;
    CXChildVisitResult ret = CXChildVisit_Break; // should never happen
    switch (cursor.kind)
    {
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_ClassDecl:
    case CXCursor_EnumDecl:
    case CXCursor_Namespace:
    case CXCursor_ClassTemplate:
        ret = CXChildVisit_Recurse;
        typ = ClTokenType_ScopeDecl;
        break;

    case CXCursor_FieldDecl:
        ret = CXChildVisit_Continue;
        break;
    case CXCursor_EnumConstantDecl:
        ret = CXChildVisit_Continue;
        break;
    case CXCursor_FunctionDecl:
        typ = ClTokenType_FuncDecl;
        ret = CXChildVisit_Continue;
        break;
    case CXCursor_VarDecl:
        typ = ClTokenType_VarDecl;
        ret = CXChildVisit_Continue;
        break;
    case CXCursor_ParmDecl:
        typ = ClTokenType_ParmDecl;
        ret = CXChildVisit_Continue;
        break;
    case CXCursor_TypedefDecl:
        //case CXCursor_MacroDefinition: // this can crash Clang on Windows
        ret = CXChildVisit_Continue;
        break;
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_FunctionTemplate:
        typ = ClTokenType_FuncDecl;
        ret = CXChildVisit_Continue;
        break;

    default:
        return CXChildVisit_Recurse;
    }

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile clFile;
    unsigned line = 1, col = 1;
    clang_getSpellingLocation(loc, &clFile, &line, &col, nullptr);
    CXString str = clang_getFileName(clFile);
    wxString filename = wxString::FromUTF8(clang_getCString(str));
    clang_disposeString(str);
    if (filename.IsEmpty())
        return ret;

    CXCompletionString token = clang_getCursorCompletionString(cursor);
    wxString identifier;
    unsigned tokenHash = HashToken(token, identifier);
    if (!identifier.IsEmpty())
    {
        wxString displayName;
        wxString scopeName;
        while (!clang_Cursor_isNull(cursor))
        {
            switch (cursor.kind)
            {
            case CXCursor_Namespace:
            case CXCursor_StructDecl:
            case CXCursor_ClassDecl:
            case CXCursor_ClassTemplate:
            case CXCursor_ClassTemplatePartialSpecialization:
            case CXCursor_CXXMethod:
                str = clang_getCursorDisplayName(cursor);
                if (displayName.Length() == 0)
                    displayName = wxString::FromUTF8(clang_getCString(str));
                else
                {
                    if (scopeName.Length() > 0)
                        scopeName = scopeName.Prepend(wxT("::"));
                    scopeName = scopeName.Prepend(wxString::FromUTF8(clang_getCString(str)));
                }
                clang_disposeString(str);
                break;
            default:
                break;
            }
            cursor = clang_getCursorSemanticParent(cursor);
        }
        struct ClangVisitorContext* ctx = static_cast<struct ClangVisitorContext*>(client_data);
        ClFileId fileId = ctx->database->GetFilenameId(filename);
        ClAbstractToken tok(typ, fileId, ClTokenPosition(line, col), identifier, tokenHash);
        ctx->database->InsertToken(tok);
        ctx->tokenCount++;
        if (displayName.Length() > 0)
        {
            if (ctx->functionScopes[fileId].size() > 0)
            {
                // Save some memory
                if (ctx->functionScopes[fileId].back().scopeName.IsSameAs( scopeName ) )
                {
                    scopeName = ctx->functionScopes[fileId].back().scopeName;
                    if (ctx->functionScopes[fileId].back().functionName.IsSameAs( displayName ))
                    {
                        return ret; // Duplicate...
                    }
                }
            }
            ctx->functionScopes[fileId].push_back( ClFunctionScope(displayName, scopeName, ClTokenPosition(line, col)) );
        }
    }
    return ret;
}
