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
 // Same-module calls don't need an import statement — score 1.0.
 {
  size_t c_slash = caller_file.rfind('/');
  size_t t_slash = candidate_file.rfind('/');
  if (c_slash != std::string::npos && t_slash != std::string::npos) {
   std::string caller_dir = caller_file.substr(0, c_slash);
   std::string cand_dir = candidate_file.substr(0, t_slash);
   if (caller_dir == cand_dir)
    return 1.0;
  }
 }
 // Check if the caller's file imports the candidate's module.
 // Query: import table where file_path = caller_file
 // and target_path contains candidate's module path.
 // Returns 1.0 if import found, 0.0 otherwise.
 sqlite3 *handle = static_cast<sqlite3 *>(db);
 const char *sql = "SELECT COUNT(*) FROM import "
     "WHERE project_id=? AND file_path=? "
     "AND target_path LIKE ?";
 sqlite3_stmt *stmt = nullptr;
 if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK)
  return 0.0;
 sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(project_id));
 sqlite3_bind_text(stmt, 2, caller_file.c_str(), -1, SQLITE_STATIC);

 // Use the candidate's module path for matching
 // target_path stores the full import path (e.g. "github.com/org/pkg"
 // or "crate::module::func"), so we match the module directory name.
 std::string module_path = candidate_file;
 size_t slash = module_path.rfind('/');
 if (slash != std::string::npos)
  module_path = module_path.substr(0, slash);
 // Extract the last component of the module path for matching
 // against import target_path entries.
 size_t prev_slash = module_path.rfind('/');
 std::string module_name = (prev_slash != std::string::npos)
          ? module_path.substr(prev_slash + 1)
          : module_path;

 std::string like_pattern = "%" + module_name + "%";
 sqlite3_bind_text(stmt, 3, like_pattern.c_str(), -1, SQLITE_STATIC);

 double result = 0.0;
 if (sqlite3_step(stmt) == SQLITE_ROW) {
  int count = sqlite3_column_int(stmt, 0);
  if (count > 0)
   result = 1.0;
 }
 sqlite3_finalize(stmt);

 // If no direct import found, also check if the candidate's file
 // imports the caller's module (bidirectional reference).
 if (result == 0.0) {
  const char *rev_sql = "SELECT COUNT(*) FROM import "
          "WHERE project_id=? AND file_path=? "
          "AND target_path LIKE ?";
  sqlite3_stmt *rev_st = nullptr;
  if (sqlite3_prepare_v2(handle, rev_sql, -1, &rev_st, nullptr) != SQLITE_OK)
   return 0.0;
  sqlite3_bind_int64(rev_st, 1, static_cast<int64_t>(project_id));
  sqlite3_bind_text(rev_st, 2, candidate_file.c_str(), -1, SQLITE_STATIC);

  // Extract caller's module name
  std::string caller_module = caller_file;
  size_t c_slash = caller_module.rfind('/');
  if (c_slash != std::string::npos)
   caller_module = caller_module.substr(0, c_slash);
  size_t c_prev = caller_module.rfind('/');
  std::string caller_mod_name = (c_prev != std::string::npos)
        ? caller_module.substr(c_prev + 1)
        : caller_module;

  std::string rev_pattern = "%" + caller_mod_name + "%";
  sqlite3_bind_text(rev_st, 3, rev_pattern.c_str(), -1, SQLITE_STATIC);

  if (sqlite3_step(rev_st) == SQLITE_ROW) {
   int count = sqlite3_column_int(rev_st, 0);
   if (count > 0)
    result = 1.0;
  }
  sqlite3_finalize(rev_st);
 }

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
  return kScoreExactMatch; // Same package

 // Check if they share a common parent directory
 // (sibling packages within the same module)
 size_t c_parent = caller_dir.rfind('/');
 size_t t_parent = cand_dir.rfind('/');
 if (c_parent == std::string::npos || t_parent == std::string::npos)
  return 0.0;

 std::string caller_parent = caller_dir.substr(0, c_parent);
 std::string cand_parent = cand_dir.substr(0, t_parent);
 if (caller_parent == cand_parent && caller_parent.length() > 0)
  return kScoreSiblingModule; // Sibling packages

 return 0.0;
}

double factorDistanceMatch(const std::string &caller_file,
       const std::string &candidate_file)
{
 if (caller_file == candidate_file)
  return kScoreExactMatch; // Same file

 size_t c_slash = caller_file.rfind('/');
 size_t t_slash = candidate_file.rfind('/');
 if (c_slash == std::string::npos || t_slash == std::string::npos)
  return 0.0;

 std::string caller_dir = caller_file.substr(0, c_slash);
 std::string cand_dir = candidate_file.substr(0, t_slash);
 if (caller_dir == cand_dir)
  return kScoreSameDirectory; // Same directory

 return 0.0;
}

double factorSignatureMatch(int caller_arity, int candidate_arity)
{
	if (caller_arity == 0 && candidate_arity == 0)
		return kScorePartialMatch; // Both unknown arity
	if (caller_arity == candidate_arity)
		return kScoreExactMatch; // Exact match
	if (candidate_arity == 0)
		return kScorePartialMatch; // Candidate has unknown arity
	return kScorePenalty; // Known-different arity — penalty
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
  return kScoreExactMatch; // Exact class/struct name match
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
  return kScoreExactMatch;

 // Different package: check if candidate is in a sub-package
 if (cand_pkg.find(caller_pkg) == 0)
  return kScorePartialMatch;

 return 0.0;
}

double factorCommonNamePenalty(const std::string &name)
{
	static const std::unordered_set<std::string> kCommonNames = {
		"Len", "Init", "Run", "Stop", "Close", "Open", "Read",
		"Write", "Get", "Set", "Add", "Remove", "Update", "Delete",
		"Create", "New", "Process", "Handle", "Execute", "Start",
		"End", "Error", "String", "Format", "Marshal", "Unmarshal",
		"Equals", "Compare", "Hash", "Copy", "Clone", "Reset",
		"Clear", "IsEmpty", "IsValid", "HasNext", "Next", "Previous",
		"First", "Last", "Value", "Key", "Int", "Float", "Bool",
		"Bytes", "Size", "Cap",
	};
	if (kCommonNames.count(name) > 0)
		return kCommonNamePenaltyValue;
	return 0.0;
}

} // namespace resolver