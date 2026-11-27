#ifndef SHAREDROWS_H
#define SHAREDROWS_H

// A vector of rows that copies are free to share (copy on write): the particles
// of one component are loaded once and every pane of a particle window looks at
// the same rows (#604). Copying a SharedRows is a pointer copy; reading never
// copies; a window that must change its rows goes through mut(), which makes a
// private copy first when other windows still look at the same ones.
//
// Only the const half of the vector API is here on purpose: a write that forgot
// mut() does not compile, so a pane can never move the rows of the others by
// accident. Header-only and std-only so it is unit-tested without Qt or VTK.

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

template<class T>
class SharedRows
{
public:
    using value_type = T;
    using Storage = std::vector<T>;
    using const_iterator = typename Storage::const_iterator;

    SharedRows() = default;
    SharedRows(const Storage &rows) : m_rows(std::make_shared<Storage>(rows)) {}
    SharedRows(Storage &&rows) : m_rows(std::make_shared<Storage>(std::move(rows))) {}

    // Copies share: two handles on the same rows, no row is copied.
    SharedRows(const SharedRows &) = default;
    SharedRows(SharedRows &&) = default;
    SharedRows &operator=(const SharedRows &) = default;
    SharedRows &operator=(SharedRows &&) = default;

    // Rows from a load: this handle looks at them from now on; the handles that
    // shared the old rows keep those.
    SharedRows &operator=(const Storage &rows)
    {
        m_rows = std::make_shared<Storage>(rows);
        return *this;
    }
    SharedRows &operator=(Storage &&rows)
    {
        m_rows = std::make_shared<Storage>(std::move(rows));
        return *this;
    }

    // ── read ────────────────────────────────────────────────────────────────
    std::size_t size() const { return m_rows ? m_rows->size() : 0; }
    bool empty() const { return size() == 0; }
    const T &operator[](std::size_t i) const { return (*m_rows)[i]; }
    const T &at(std::size_t i) const { return m_rows->at(i); }
    const T &front() const { return m_rows->front(); }
    const T &back() const { return m_rows->back(); }
    const T *data() const { return m_rows ? m_rows->data() : nullptr; }
    const_iterator begin() const { return view().begin(); }
    const_iterator end() const { return view().end(); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }
    // For the functions that take a const std::vector& (no row is copied).
    const Storage &rows() const { return view(); }
    operator const Storage &() const { return view(); }

    // ── sharing ─────────────────────────────────────────────────────────────
    /// True when the other handle looks at the very same rows (not merely equal
    /// ones): what makes a pane's copy cost nothing.
    bool sharesWith(const SharedRows &other) const { return m_rows && m_rows == other.m_rows; }
    /// How many handles look at these rows (1: private).
    long handles() const { return m_rows ? m_rows.use_count() : 0; }
    /// Identity of the rows, for a cache key; null while there are none.
    const void *identity() const { return m_rows.get(); }

    // ── write ───────────────────────────────────────────────────────────────
    /// The rows to change: private to this handle (copied first when shared).
    Storage &mut()
    {
        if (!m_rows)
            m_rows = std::make_shared<Storage>();
        else if (m_rows.use_count() > 1)
            m_rows = std::make_shared<Storage>(*m_rows);
        return *m_rows;
    }
    /// Lets go of the rows without copying them; the other handles keep theirs.
    void reset() { m_rows.reset(); }

private:
    const Storage &view() const
    {
        static const Storage none;
        return m_rows ? *m_rows : none;
    }

    std::shared_ptr<Storage> m_rows;
};

#endif // SHAREDROWS_H
