#include "factors.h"
#include <sqlite3.h>
#include <algorithm>
#include <cstring>

namespace resolver
{

double factorImportMatch(uint64_t project_id, void *db,
			 const std::string &caller_file,
			 const std::string &candidate_file,
			 const std::string &candidate_name)
{
	// Check if the caller's file imports the candidate's module.
	// Query: import table where file_path = caller_file
	// and target_path contains candidate's module name.
	// Returns 1.0 if import found, 0.0 otherwise.
	sqlite3 *handle = static_cast<sqlite3 *>(db);
	const char *sql = "SELECT COUNT(*) FROM import "
			  "WHERE project_id=? AND file_path=? "
			  "AND (target_path LIKE ? OR alias=?)";
	sqlite3_stmt *stmt = nullptr;
	if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK)
		return 0.0;
	sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
	sqlite3_bind_text(stmt, 2, caller_file.c_str(), -1, SQLITE_STATIC);

	// Use the candidate's module path for matching
	std::string module_path = candidate_file;
	size_t slash = module_path.rfind('/');
	if (slash != std::string::npos)
		module_path = module_path.substr(0, slash);

	std::string like_pattern = "%" + module_path + "%";
	sqlite3_bind_text(stmt, 3, like_pattern.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, candidate_name.c_str(), -1, SQLITE_STATIC);

	double result = 0.0;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int count = sqlite3_column_int(stmt, 0);
		if (count > 0)
			result = 1.0;
	}
	sqlite3_finalize(stmt);
	return result;
}

double factorNamespaceMatch(const std::string &caller_file,
			    const std::string &candidate_file)
{
	// Extract directory paths and compare.
	size_t c_slash = caller_file.rfind('/');
	size_t t_slash = candidate_file.rfind('/');
	if (c_slash == std::string::npos || t_slash == std::string::npos)
		return 0.0;

	std::string caller_dir = caller_file.substr(0, c_slash);
	std::string cand_dir = candidate_file.substr(0, t_slash);

	if (caller_dir == cand_dir)
		return 1.0; // Same package

	// Check if they share a common parent directory
	// (sibling packages within the same module)
	size_t c_parent = caller_dir.rfind('/');
	size_t t_parent = cand_dir.rfind('/');
	if (c_parent == std::string::npos || t_parent == std::string::npos)
		return 0.0;

	std::string caller_parent = caller_dir.substr(0, c_parent);
	std::string cand_parent = cand_dir.substr(0, t_parent);
	if (caller_parent == cand_parent && caller_parent.length() > 0)
		return 0.5; // Sibling packages

	return 0.0;
}

double factorDistanceMatch(const std::string &caller_file,
			   const std::string &candidate_file)
{
	if (caller_file == candidate_file)
		return 1.0; // Same file

	size_t c_slash = caller_file.rfind('/');
	size_t t_slash = candidate_file.rfind('/');
	if (c_slash == std::string::npos || t_slash == std::string::npos)
		return 0.0;

	std::string caller_dir = caller_file.substr(0, c_slash);
	std::string cand_dir = candidate_file.substr(0, t_slash);
	if (caller_dir == cand_dir)
		return 0.3; // Same directory

	return 0.0;
}

double factorSignatureMatch(int caller_arity, int candidate_arity)
{
	if (caller_arity == 0 && candidate_arity == 0)
		return 0.5; // Both unknown arity
	if (caller_arity == candidate_arity)
		return 1.0; // Exact match
	if (candidate_arity == 0)
		return 0.5; // Candidate has unknown arity
	return -0.5; // Known-different arity — penalty
}

double factorConstructorMatch(const std::string &ref_name,
			      const std::string &candidate_name,
			      int candidate_kind)
{
	// Constructor match: reference name matches a class/struct name.
	// candidate_kind: 2 = Class, 3 = Struct
	if (candidate_kind != 2 && candidate_kind != 3)
		return 0.0;
	if (ref_name == candidate_name)
		return 1.0; // Exact class/struct name match
	return 0.0;
}

double factorReceiverMatch(const std::string &ref_name,
			   const std::string &caller_file,
			   const std::string &candidate_name,
			   const std::string &candidate_file)
{
	// Receiver match: for method calls like a.method(),
	// check if the candidate is a method of the caller's receiver type.
	// This is a simplified check: if caller and candidate share
	// the same package prefix, boost the score.
	size_t c_slash = caller_file.rfind('/');
	size_t t_slash = candidate_file.rfind('/');
	if (c_slash == std::string::npos || t_slash == std::string::npos)
		return 0.0;

	std::string caller_pkg = caller_file.substr(0, c_slash);
	std::string cand_pkg = candidate_file.substr(0, t_slash);

	// Same package: likely receiver match
	if (caller_pkg == cand_pkg)
		return 1.0;

	// Different package: check if candidate is in a sub-package
	if (cand_pkg.find(caller_pkg) == 0)
		return 0.5;

	return 0.0;
}

} // namespace resolver