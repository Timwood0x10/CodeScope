#ifndef CODESCOPE_MODEL_PROJECT_STATE_INTERNAL_H
#define CODESCOPE_MODEL_PROJECT_STATE_INTERNAL_H

#include <sqlite3.h>

#include <string>

/// Internals shared by project_state_builder.cpp and project_state_queries.cpp.
///
/// The split moved the query helpers out of the builder; both halves still need
/// the same statement guard and the same null-safe column reader, and two copies
/// would drift the first time one of them fixes a leak.
namespace model
{
namespace detail
{

/// RAII guard around a sqlite3_stmt: finalizes on every exit path.
class StmtGuard {
    public:
	explicit StmtGuard(sqlite3_stmt *stmt = nullptr)
		: stmt_(stmt)
	{
	}
	~StmtGuard()
	{
		if (stmt_)
			sqlite3_finalize(stmt_);
	}
	StmtGuard(const StmtGuard &) = delete;
	StmtGuard &operator=(const StmtGuard &) = delete;
	StmtGuard(StmtGuard &&other) noexcept
		: stmt_(other.stmt_)
	{
		other.stmt_ = nullptr;
	}
	sqlite3_stmt *get() const
	{
		return stmt_;
	}

    private:
	sqlite3_stmt *stmt_;
};

/// Text column of the current row, never null.
inline std::string colText(sqlite3_stmt *stmt, int col)
{
	const unsigned char *t = sqlite3_column_text(stmt, col);
	if (!t)
		return "";
	return std::string(reinterpret_cast<const char *>(t));
}

} // namespace detail
} // namespace model

#endif // CODESCOPE_MODEL_PROJECT_STATE_INTERNAL_H
