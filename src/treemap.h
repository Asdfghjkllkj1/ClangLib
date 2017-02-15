#ifndef TREEMAP_H
#define TREEMAP_H

#include <vector>
#include <set>

struct TreeNode;
class wxString;
template<typename _TpVal> class ClTreeMap;

template<>
class ClTreeMap<int>
{
public:
    ClTreeMap();
    ClTreeMap(const ClTreeMap<int>& other);
    ~ClTreeMap();
    int Insert(const wxString& key, int value); // returns value
    void Remove(const wxString& key, int value);
    void Remove(const wxString& key);
    void Shrink();
    void GetIdSet(const wxString& key, std::set<int>& out_ids) const;
    int GetValue(int id) const; // returns id
    int GetCount() const;
    std::set<wxString> GetKeySet() const;
private:
    TreeNode* m_Root;
};

template<typename _TpVal>
class ClTreeMap
{
public:
    // returns the id of the value inserted
    int Insert(const wxString& key, const _TpVal& value)
    {
        m_Data.push_back(value);
        return m_Tree.Insert(key, m_Data.size() - 1);
    }

    void Shrink()
    {
        m_Tree.Shrink();
#if __cplusplus >= 201103L
        m_Data.shrink_to_fit();
#else
        std::vector<_TpVal>(m_Data).swap(m_Data);
#endif
    }

    void GetIdSet(const wxString& key, std::set<int>& out_ids) const
    {
        m_Tree.GetIdSet(key, out_ids);
    }
    void RemoveIdKey( const wxString& key, int id )
    {
        m_Tree.Remove(key, id);
    }
    void Remove( const wxString& key )
    {
        m_Tree.Remove(key);
    }

    bool HasValue(int id)
    {
        if (id < 0)
            return false;
        if (id >= (int)m_Data.size())
            return false;
        return true;
    }

    _TpVal& GetValue(int id)
    {
        return m_Data[id];
    }
    int GetCount() const
    {
        return m_Data.size();
    }
    std::set<wxString> GetKeySet() const
    {
        return m_Tree.GetKeySet();
    }

private:
    ClTreeMap<int> m_Tree;
    std::vector<_TpVal> m_Data;
};

#endif // TREEMAP_H
